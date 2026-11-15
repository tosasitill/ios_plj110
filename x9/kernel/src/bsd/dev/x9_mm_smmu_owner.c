/* Native installed-storage adapter. No private whole-controller template,
 * global reset, secure register write, HVC probe or automatic registration. */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/locks.h>
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <vm/pmap.h>
#include <vm/vm_kern.h>
#include <arm/caches_internal.h>
#include "x9_mm_power.h"
#include "x9_mm_smmu_owner.h"
extern ppnum_t pmap_find_phys(pmap_t, addr64_t);
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
#define OWNER_PAGE 16384u
#define OWNER_ST_ADDR UINT64_C(0x000fffffffffffc0)
#define OWNER_Q_ADDR UINT64_C(0x000fffffffffffe0)
#define OWNER_MAGIC UINT64_C(0x58394d4d4f574e31)
LCK_GRP_DECLARE(mm_owner_group,"x9-mm-smmu-owner");
LCK_MTX_DECLARE(mm_owner_lock,&mm_owner_group);
static const unsigned owner_reg32[]={0,4,0x14,0x20,0x24,0x28,0x2c,
    0x44,0x50,0x54,0x60,0x64,0x88};
static const unsigned owner_reg64[]={0x80,0x90,0xa0};
struct owner_lease {
    struct x9_mm_smmu_owner *owner;
    uint64_t token,epoch;
    uint32_t sid,asid,state;
    bool released;
    const struct x9_mm_smmu_private_mapping *mapping;
};
struct x9_mm_smmu_owner {
    uint64_t magic,power_child,camera_token,camera_epoch;
    struct x9_mm_smmu_owner_adoption adoption;
    vm_offset_t mmio,expected_ste,expected_l1;
    uint64_t ste_pa,l1_pa;
    uint32_t l1_bytes,qshift,eshift,prod,cons,op,active_sid;
    uint32_t reg32[sizeof(owner_reg32)/sizeof(owner_reg32[0])];
    uint64_t reg64[sizeof(owner_reg64)/sizeof(owner_reg64[0])];
    uint32_t fault,staged_count,batch_start;
    uint64_t staged[4][2];
    bool pending,active,authority_held,camera_queue_live;
    struct owner_lease leases[2];
    struct owner_lease *current;
    const struct x9_camera_smmu_export *camera;
};
static struct x9_mm_smmu_owner *owner_namespace;
static uint64_t owner_next_token=1;
static uint32_t owner_rr(struct x9_mm_smmu_owner *o,unsigned off)
{ uint32_t v=*(volatile uint32_t *)(o->mmio+off);__asm__ volatile("dsb sy":::"memory");return v; }
static uint64_t owner_rr64(struct x9_mm_smmu_owner *o,unsigned off)
{ uint64_t v=*(volatile uint64_t *)(o->mmio+off);__asm__ volatile("dsb sy":::"memory");return v; }
static bool owner_overlap(uint64_t a,uint64_t ab,uint64_t b,uint64_t bb)
{ return a<b+bb && b<a+ab; } /* only prevalidated nonwrapping intervals */
static int owner_pin(struct x9_mm_smmu_owner *o)
{ return x9_mm_power_validate_pin(o->adoption.power_token,o->power_child,o->adoption.power_client); }
static int owner_region(const struct x9_mm_smmu_owner_region *r,uint32_t max)
{
    if(PAGE_SHIFT!=14 || !r->va || !r->pa || r->reserved || !r->bytes ||
       r->bytes>max || ((r->va|r->pa|r->bytes)&(OWNER_PAGE-1)) ||
       r->bytes>UINTPTR_MAX-r->va || r->pa>=(UINT64_C(1)<<48) ||
       r->bytes>(UINT64_C(1)<<48)-r->pa)return EINVAL;
    for(uint32_t off=0;off<r->bytes;off+=OWNER_PAGE) {
        ppnum_t first=pmap_find_phys(kernel_pmap,(addr64_t)(r->va+off));
        ppnum_t last=pmap_find_phys(kernel_pmap,(addr64_t)(r->va+off+OWNER_PAGE-1));
        if(!first || first!=last || ptoa_64(first)!=r->pa+off)return ESTALE;
    }
    return 0;
}
static bool owner_empty(const struct x9_mm_smmu_owner_region *r)
{ return !r->va && !r->pa && !r->bytes && !r->reserved; }
static int owner_slice(const struct x9_mm_smmu_owner_region *r,uint64_t pa,
    uint32_t bytes,vm_offset_t *va)
{
    if(!bytes || pa<r->pa || pa-r->pa>=r->bytes || bytes>r->bytes-(pa-r->pa))return ERANGE;
    *va=r->va+(vm_offset_t)(pa-r->pa);return 0;
}
static const struct x9_mm_smmu_owner_region *owner_ste_region(struct x9_mm_smmu_owner *o)
{ return o->l1_bytes?&o->adoption.strtab_l2:&o->adoption.strtab; }
static int owner_storage(struct x9_mm_smmu_owner *o)
{
    const struct x9_mm_smmu_owner_region *r[]={&o->adoption.strtab,
        &o->adoption.cmdq,&o->adoption.evtq,&o->adoption.strtab_l2};
    const uint32_t max[]={32768u,16u<<19,32u<<19,32768u};
    unsigned count=owner_empty(r[3])?3:4;
    for(unsigned n=0;n<count;n++) {
        int e=owner_region(r[n],max[n]);if(e)return e;
        for(unsigned j=0;j<n;j++)
            if(owner_overlap(r[n]->va,r[n]->bytes,r[j]->va,r[j]->bytes) ||
               owner_overlap(r[n]->pa,r[n]->bytes,r[j]->pa,r[j]->bytes))return EINVAL;
    }
    return 0;
}
static int owner_capture(struct x9_mm_smmu_owner *o)
{
    vm_offset_t va=0;
    uint32_t cfg=o->reg32[12],bits=cfg&63u,fmt=(cfg>>16)&3u;
    uint64_t base=o->reg64[0]&OWNER_ST_ADDR;
    int e;
    o->qshift=o->reg64[1]&31u;o->eshift=o->reg64[2]&31u;
    if(o->qshift<2 || o->qshift>19 || o->qshift>((o->reg32[1]>>21)&31u) ||
       !o->eshift || o->eshift>19 || o->eshift>((o->reg32[1]>>16)&31u))return ENOTSUP;
    if(o->reg32[3]!=13u || o->reg32[4]!=13u || (o->reg32[5]&~0xfffu) ||
       (o->reg32[6]&~7u) || (o->reg32[7]&(1u<<31)) ||
       o->reg32[8] || o->reg32[9] ||
       (o->reg64[0]&~(OWNER_ST_ADDR|(UINT64_C(1)<<62))) ||
       (o->reg64[1]&~(OWNER_Q_ADDR|31u|(UINT64_C(1)<<62))) ||
       (o->reg64[2]&~(OWNER_Q_ADDR|31u|(UINT64_C(1)<<62))) ||
       ((o->reg64[1]&OWNER_Q_ADDR)&((16u<<o->qshift)-1u)) ||
       ((o->reg64[2]&OWNER_Q_ADDR)&((32u<<o->eshift)-1u)))return ENOTSUP;
    e=owner_slice(&o->adoption.cmdq,o->reg64[1]&OWNER_Q_ADDR,16u<<o->qshift,&va);
    if(!e)e=owner_slice(&o->adoption.evtq,o->reg64[2]&OWNER_Q_ADDR,32u<<o->eshift,&va);
    if(e)return e;
    if(!fmt) {
        if(cfg!=8 || !owner_empty(&o->adoption.strtab_l2))return ENOTSUP;
        o->ste_pa=base;
    } else {
        if(fmt!=1 || ((cfg>>6)&31u)!=8 || bits<8 || bits>20 ||
           bits>(o->reg32[1]&63u) || ((o->reg32[0]>>27)&3u)!=1 ||
           (cfg&~((3u<<16)|(31u<<6)|63u)))return ENOTSUP;
        o->l1_bytes=8u<<(bits-8);o->l1_pa=base;
        uint32_t align=o->l1_bytes<64?64:o->l1_bytes;
        if(base&(align-1))return ERANGE;
        e=owner_slice(&o->adoption.strtab,base,o->l1_bytes,&va);if(e)return e;
        memcpy((void *)o->expected_l1,(const void *)va,o->l1_bytes);
        const uint64_t *p=(const uint64_t *)o->expected_l1;
        if(!p[0])return ENXIO;
        for(unsigned n=0;n<o->l1_bytes/8;n++) {
            uint64_t d=p[n],pa=d&OWNER_ST_ADDR;
            if(!d)continue;
            if((d&31u)!=9 || (d&~(OWNER_ST_ADDR|UINT64_C(31))) ||
               !pa || (pa&(OWNER_PAGE-1)) || pa>(UINT64_C(1)<<48)-OWNER_PAGE)return ENOTSUP;
            const struct x9_mm_smmu_owner_region *r[]={&o->adoption.strtab,
                &o->adoption.cmdq,&o->adoption.evtq};
            for(unsigned j=0;j<3;j++)if(owner_overlap(pa,OWNER_PAGE,r[j]->pa,r[j]->bytes))return ESTALE;
            if(n && owner_overlap(pa,OWNER_PAGE,o->adoption.strtab_l2.pa,o->adoption.strtab_l2.bytes))return ESTALE;
            for(unsigned j=0;j<n;j++)if(p[j] && (p[j]&OWNER_ST_ADDR)==pa)return ESTALE;
        }
        o->ste_pa=p[0]&OWNER_ST_ADDR;
    }
    if(o->ste_pa&(OWNER_PAGE-1))return ERANGE;
    e=owner_slice(owner_ste_region(o),o->ste_pa,OWNER_PAGE,&va);if(e)return e;
    memcpy((void *)o->expected_ste,(const void *)va,OWNER_PAGE);
    return 0;
}
static int owner_auth(struct x9_mm_smmu_owner *o,bool enter)
{
    struct owner_lease *l=o->current;
    uint32_t asid=l?l->asid:o->active_sid==0x80?1u:0u;
    uint64_t epoch=l?l->epoch:o->camera?o->camera_epoch:o->adoption.epoch;
    const struct x9_mm_smmu_owner_authority *a=&o->adoption.authority;
    if(enter)return a->enter(a->context,o->op,o->active_sid,asid,epoch,l?l->mapping:NULL,o->camera);
    return a->guard(a->context,o->op,o->active_sid,asid,epoch,l?l->mapping:NULL,o->camera);
}
static void owner_auth_leave(struct x9_mm_smmu_owner *o,uint32_t state)
{
    struct owner_lease *l=o->current;
    o->adoption.authority.leave(o->adoption.authority.context,o->op,o->active_sid,
        l?l->asid:o->active_sid==0x80?1u:0u,
        l?l->epoch:o->camera?o->camera_epoch:o->adoption.epoch,state);
    o->authority_held=false;
}
static int owner_live(struct x9_mm_smmu_owner *o)
{
    vm_offset_t va=0;
    int e=owner_pin(o);if(e)return e;
    if(o->fault)return (int)o->fault;
    e=owner_storage(o);if(e)return e;
    for(unsigned n=0;n<sizeof(owner_reg32)/sizeof(owner_reg32[0]);n++)
        if(owner_rr(o,owner_reg32[n])!=o->reg32[n])return ESTALE;
    for(unsigned n=0;n<sizeof(owner_reg64)/sizeof(owner_reg64[0]);n++)
        if(owner_rr64(o,owner_reg64[n])!=o->reg64[n])return ESTALE;
    if(o->reg32[3]!=o->reg32[4] || o->reg32[3]!=13u ||
       o->reg32[10]!=o->reg32[11])return EIO;
    uint32_t ep=owner_rr(o,0x100a8),ec=owner_rr(o,0x100ac),em=(2u<<o->eshift)-1u;
    if((ep&~em) || (ec&~em) || ep!=ec)return EIO;
    if(o->l1_bytes) {
        e=owner_slice(&o->adoption.strtab,o->l1_pa,o->l1_bytes,&va);if(e)return e;
        if(memcmp((const void *)va,(const void *)o->expected_l1,o->l1_bytes))return ESTALE;
    }
    e=owner_slice(owner_ste_region(o),o->ste_pa,OWNER_PAGE,&va);if(e)return e;
    const volatile uint64_t *p=(const volatile uint64_t *)va;
    const uint64_t *x=(const uint64_t *)o->expected_ste;
    for(unsigned sid=0;sid<256;sid++) {
        if(o->camera && sid==0x80)continue; /* camera engine checks OWN words */
        for(unsigned w=0;w<8;w++)if(p[sid*8+w]!=x[sid*8+w])return ESTALE;
    }
    uint32_t prod=owner_rr(o,0x98),cons=owner_rr(o,0x9c),mask=(2u<<o->qshift)-1u;
    if((prod|cons)&~mask)return EIO;
    if(!o->camera_queue_live) {
        if(prod!=o->prod)return ESTALE;
        if(o->pending) {
            if(((cons-o->cons)&mask)>((o->prod-o->cons)&mask))return ESTALE;
            o->cons=cons;
            if(cons==prod)o->pending=false;
        } else if(cons!=o->cons)return ESTALE;
        if(o->staged_count) {
            uint32_t ring=1u<<o->qshift;
            for(unsigned n=0;n<o->staged_count;n++) {
                uint64_t pa=(o->reg64[1]&OWNER_Q_ADDR)+((o->batch_start+n)&(ring-1))*16u;
                e=owner_slice(&o->adoption.cmdq,pa,16,&va);if(e)return e;
                if(memcmp((const void *)va,o->staged[n],16))return ESTALE;
            }
        }
    }
    return 0;
}
static int owner_guard_locked(struct x9_mm_smmu_owner *o)
{
    if(!o->active || !o->authority_held)return ESTALE;
    int e=owner_auth(o,false);if(!e)e=owner_live(o);
    if(e && !o->fault)o->fault=(uint32_t)e;
    return e;
}
static int owner_begin(struct x9_mm_smmu_owner *o)
{
    int e=owner_pin(o);
    if(!e && o->fault)e=(int)o->fault;
    if(!e)e=owner_auth(o,true);
    if(e)return e;
    o->active=true;o->authority_held=true;
    e=owner_live(o);
    /* The camera writes its queue directly DURING the acquired operation;
     * only here, before handing control to it, compare the prior ledger. */
    if(!e && o->camera && (o->pending || owner_rr(o,0x98)!=o->prod ||
       owner_rr(o,0x9c)!=o->cons))e=ESTALE;
    if(!e && owner_rr(o,0x98)!=owner_rr(o,0x9c))e=EBUSY;
    if(e) {owner_auth_leave(o,X9_MM_SMMU_UNEXPOSED);o->active=false;return e;}
    o->prod=o->cons=owner_rr(o,0x98);o->pending=false;o->staged_count=0;
    /* Only now may camera's private command engine mutate the shared queue.
     * The completed preceding codec record was checked before clearing it. */
    o->camera_queue_live=o->camera!=NULL;
    return 0;
}
static uint64_t owner_token(void)
{ return owner_next_token && owner_next_token!=UINT64_MAX?owner_next_token++:0; }
int x9_mm_smmu_owner_adopt(const struct x9_mm_smmu_owner_adoption *a,
    struct x9_mm_smmu_owner **out)
{
    int e;
    struct x9_mm_smmu_owner *o;
    if(!a || !out || *out || !a->epoch || !a->power_token || a->reserved ||
       !a->authority.enter || !a->authority.guard || !a->authority.leave)return EINVAL;
    lck_mtx_lock(&mm_owner_lock);
    if(owner_namespace){lck_mtx_unlock(&mm_owner_lock);return EBUSY;}
    o=kalloc_type(struct x9_mm_smmu_owner,Z_WAITOK|Z_ZERO);
    if(!o){lck_mtx_unlock(&mm_owner_lock);return ENOMEM;}
    o->magic=OWNER_MAGIC;o->adoption=*a;o->op=X9_MM_OWNER_ADOPT;
    e=x9_mm_power_pin(a->power_token,a->power_client,&o->power_child);
    if(!e)e=owner_storage(o);
    if(!e)e=owner_auth(o,true);
    if(!e) {
        o->authority_held=true;
        o->mmio=ml_io_map(0x30800000,0x20000);if(!o->mmio)e=ENOMEM;
    }
    if(!e && kmem_alloc(kernel_map,&o->expected_ste,OWNER_PAGE,KMA_DATA|KMA_ZERO,
        VM_KERN_MEMORY_IOKIT)!=KERN_SUCCESS)e=ENOMEM;
    if(!e && kmem_alloc(kernel_map,&o->expected_l1,32768,KMA_DATA|KMA_ZERO,
        VM_KERN_MEMORY_IOKIT)!=KERN_SUCCESS)e=ENOMEM;
    if(!e) {
        e=owner_pin(o);
        for(unsigned n=0;n<sizeof(owner_reg32)/sizeof(owner_reg32[0]) && !e;n++)o->reg32[n]=owner_rr(o,owner_reg32[n]);
        for(unsigned n=0;n<sizeof(owner_reg64)/sizeof(owner_reg64[0]) && !e;n++)o->reg64[n]=owner_rr64(o,owner_reg64[n]);
        if(!e)e=owner_capture(o);
        o->prod=o->cons=owner_rr(o,0x98);
        if(!e)e=owner_live(o);
        if(!e && o->prod!=owner_rr(o,0x9c))e=EBUSY;
        if(!e)e=owner_auth(o,false);
    }
    if(o->authority_held)owner_auth_leave(o,e?X9_MM_SMMU_UNEXPOSED:X9_MM_SMMU_INSTALLED);
    if(e) {
        if(o->expected_ste)kmem_free(kernel_map,o->expected_ste,OWNER_PAGE);
        if(o->expected_l1)kmem_free(kernel_map,o->expected_l1,32768);
        if(o->power_child) {
            int drop=x9_mm_power_unpin(a->power_token,&o->power_child,a->power_client);
            if(drop) { /* uncertain supplier release: retain opaque failed owner */
                o->fault=(uint32_t)e;owner_namespace=o;
                lck_mtx_unlock(&mm_owner_lock);return e;
            }
        }
        kfree_type(struct x9_mm_smmu_owner,o);
    } else {owner_namespace=o;*out=o;}
    lck_mtx_unlock(&mm_owner_lock);return e;
}
static int owner_identity(struct x9_mm_smmu_owner *o)
{ return o && o==owner_namespace && o->magic==OWNER_MAGIC?0:EINVAL; }
int x9_mm_smmu_owner_lease(struct x9_mm_smmu_owner *o,uint32_t sid,
    uint64_t epoch,uint64_t *token,uint32_t *asid)
{
    int e;
    if(!token || *token || !asid || !epoch || (sid!=0xa0 && sid!=0xb0))return EINVAL;
    lck_mtx_lock(&mm_owner_lock);e=owner_identity(o);
    unsigned index=sid==0xa0?0u:1u;
    if(!e && o->leases[index].token)e=EBUSY;
    if(!e) {
        struct owner_lease *l=&o->leases[index];
        uint32_t candidate=2,limit=(o->reg32[0]&(1u<<12))?65535u:255u;
        while(candidate<=limit && o->leases[1-index].token && candidate==o->leases[1-index].asid)candidate++;
        if(candidate>limit)e=ENOSPC;
        else {
            l->owner=o;l->sid=sid;l->asid=candidate;l->epoch=epoch;
            o->current=l;o->active_sid=sid;o->op=X9_MM_OWNER_LEASE;
            e=owner_begin(o);
            if(!e) {
                uint64_t t=owner_token();
                if(!t)e=EOVERFLOW;else {l->token=t;*token=t;*asid=candidate;}
                owner_auth_leave(o,X9_MM_SMMU_UNEXPOSED);o->active=false;
            }
            o->current=NULL;o->active_sid=0;
            if(e)memset(l,0,sizeof(*l));
        }
    }
    lck_mtx_unlock(&mm_owner_lock);return e;
}
static struct owner_lease *owner_find(struct x9_mm_smmu_owner *o,uint64_t token)
{ for(unsigned n=0;n<2;n++)if(token && !o->leases[n].released && o->leases[n].token==token)return &o->leases[n];return NULL; }
static int owner_private(struct owner_lease *l,const struct x9_mm_smmu_private_mapping *m)
{
    struct x9_mm_smmu_owner *o=l->owner;
    if(!m || m->context.request.sid!=l->sid || m->query.sid!=l->sid ||
       m->context.request.asid!=l->asid || m->context.request.epoch!=l->epoch ||
       !m->pages || !m->spans || m->page_count>X9_MM_SMMU_BUILD_MAX_TABLES ||
       m->span_count>X9_MM_SMMU_MAX_DMA_SPANS)return EINVAL;
    const struct x9_mm_smmu_owner_region *r[]={&o->adoption.strtab,
        &o->adoption.strtab_l2,&o->adoption.cmdq,&o->adoption.evtq};
    for(unsigned n=0;n<4;n++)if(r[n]->bytes) {
        if(owner_overlap(m->context.request.cd_pa,64,r[n]->pa,r[n]->bytes))return EACCES;
        for(unsigned j=0;j<m->page_count;j++)if(owner_overlap(m->pages[j].pa,4096,r[n]->pa,r[n]->bytes))return EACCES;
        for(unsigned j=0;j<m->span_count;j++)if(owner_overlap(m->spans[j].expected_pa,m->spans[j].bytes,r[n]->pa,r[n]->bytes))return EACCES;
    }
    for(unsigned n=0;n<2;n++) {
        const struct owner_lease *other=&o->leases[n];
        if(other==l || !other->mapping)continue;
        const struct x9_mm_smmu_private_mapping *b=other->mapping;
        struct x9_mm_smmu_range ar[1+X9_MM_SMMU_BUILD_MAX_TABLES];
        ar[0]=(struct x9_mm_smmu_range){m->context.request.cd_pa,64};
        for(unsigned j=0;j<m->page_count;j++)ar[j+1]=(struct x9_mm_smmu_range){m->pages[j].pa,4096};
        for(unsigned j=0;j<=m->page_count;j++) {
            if(owner_overlap(ar[j].start,ar[j].bytes,b->context.request.cd_pa,64))return EACCES;
            for(unsigned k=0;k<b->page_count;k++)if(owner_overlap(ar[j].start,ar[j].bytes,b->pages[k].pa,4096))return EACCES;
            for(unsigned k=0;k<b->span_count;k++)if(owner_overlap(ar[j].start,ar[j].bytes,b->spans[k].expected_pa,b->spans[k].bytes))return EACCES;
        }
        for(unsigned j=0;j<m->span_count;j++) {
            const struct x9_mm_smmu_dma_span *s=&m->spans[j];
            if(owner_overlap(s->expected_pa,s->bytes,b->context.request.cd_pa,64))return EACCES;
            for(unsigned k=0;k<b->page_count;k++)if(owner_overlap(s->expected_pa,s->bytes,b->pages[k].pa,4096))return EACCES;
            for(unsigned k=0;k<b->span_count;k++)if(owner_overlap(s->expected_pa,s->bytes,b->spans[k].expected_pa,b->spans[k].bytes))return EACCES;
        }
    }
    return 0;
}
static int owner_io_enter(void *cookie,const struct x9_mm_smmu_private_mapping *m)
{
    struct owner_lease *l=cookie;struct x9_mm_smmu_owner *o=l->owner;
    lck_mtx_lock(&mm_owner_lock);
    int e=owner_identity(o);
    if(!e && (owner_find(o,l->token)!=l || l->state==X9_MM_SMMU_QUARANTINED || l->state==X9_MM_SMMU_RETIRED))e=ESTALE;
    if(!e)e=owner_private(l,m);
    if(!e && l->state==X9_MM_SMMU_INSTALLED && l->mapping!=m)e=ESTALE;
    if(!e) {
        l->mapping=m;o->current=l;o->active_sid=l->sid;
        o->op=l->state==X9_MM_SMMU_INSTALLED?X9_MM_OWNER_RETIRE:X9_MM_OWNER_INSTALL;
        e=owner_begin(o);
    }
    if(e) {
        if(l->state==X9_MM_SMMU_INSTALLED)l->state=X9_MM_SMMU_QUARANTINED;
        else l->mapping=NULL;
        o->current=NULL;o->active_sid=0;lck_mtx_unlock(&mm_owner_lock);
    }
    return e;
}
static int owner_io_guard(void *cookie,uint64_t epoch)
{ struct owner_lease *l=cookie;return epoch==l->epoch && l->owner->current==l?owner_guard_locked(l->owner):ESTALE; }
static void owner_io_leave(void *cookie,uint32_t state)
{
    struct owner_lease *l=cookie;struct x9_mm_smmu_owner *o=l->owner;
    if(o->current!=l || !o->active)panic("X9 MM: mismatched owner leave");
    if(state==X9_MM_SMMU_QUARANTINED && !o->fault)o->fault=EIO;
    l->state=state;
    if(state==X9_MM_SMMU_UNEXPOSED || state==X9_MM_SMMU_RETIRED)l->mapping=NULL;
    owner_auth_leave(o,state);o->active=false;o->current=NULL;o->active_sid=0;
    lck_mtx_unlock(&mm_owner_lock);
}
static int owner_io_regs(void *cookie,struct x9_mm_smmu_regs *r)
{
    struct owner_lease *l=cookie;struct x9_mm_smmu_owner *o=l->owner;
    int e=owner_guard_locked(o);if(e)return e;
    *r=(struct x9_mm_smmu_regs){.idr0=owner_rr(o,0),.idr1=owner_rr(o,4),.idr5=owner_rr(o,0x14),
        .cr0=owner_rr(o,0x20),.cr0ack=owner_rr(o,0x24),.strtab_cfg=owner_rr(o,0x88),
        .cmdq_prod=owner_rr(o,0x98),.cmdq_cons=owner_rr(o,0x9c),
        .gerror=owner_rr(o,0x60),.gerrorn=owner_rr(o,0x64),
        .strtab_base=owner_rr64(o,0x80),.cmdq_base=owner_rr64(o,0x90)};return 0;
}
static int owner_io_read(void *cookie,uint64_t pa,uint8_t *dst,uint32_t bytes)
{
    struct owner_lease *l=cookie;struct x9_mm_smmu_owner *o=l->owner;vm_offset_t va=0;
    if(!dst || (bytes!=8 && bytes!=64))return EINVAL;
    int e=owner_guard_locked(o);if(e)return e;
    e=owner_slice(&o->adoption.strtab,pa,bytes,&va);
    if(e && o->l1_bytes)e=owner_slice(&o->adoption.strtab_l2,pa,bytes,&va);
    if(e)return e;
    memcpy(dst,(const void *)va,bytes);return 0;
}
static int owner_io_write(void *cookie,uint64_t pa,const uint8_t *src,uint32_t bytes)
{
    struct owner_lease *l=cookie;struct x9_mm_smmu_owner *o=l->owner;vm_offset_t va=0;
    if(!src)return EINVAL;
    int e=owner_guard_locked(o);if(e)return e;
    uint64_t ste=o->ste_pa+(uint64_t)l->sid*64u;
    if(pa>=ste && pa-ste<64 && bytes<=64-(pa-ste)) {
        if(pa!=ste+8 || bytes!=56)return EACCES;
        e=owner_slice(owner_ste_region(o),pa,bytes,&va);if(e)return e;
        memcpy((void *)va,src,bytes);
        memcpy((void *)(o->expected_ste+l->sid*64u+8),src,bytes);return 0;
    }
    if(bytes!=16 || o->pending)return EACCES;
    if(o->staged_count==4)o->staged_count=0;
    if(!o->staged_count)o->batch_start=o->prod;
    uint64_t expected=(o->reg64[1]&OWNER_Q_ADDR)+((o->batch_start+o->staged_count)&((1u<<o->qshift)-1))*16u;
    if(pa!=expected)return EACCES;
    e=owner_slice(&o->adoption.cmdq,pa,16,&va);if(e)return e;
    memcpy((void *)va,src,16);memcpy(o->staged[o->staged_count++],src,16);return 0;
}
static int owner_io_store(void *cookie,uint64_t pa,uint64_t value)
{
    struct owner_lease *l=cookie;struct x9_mm_smmu_owner *o=l->owner;vm_offset_t va=0;
    int e=owner_guard_locked(o);if(e)return e;
    uint64_t ste=o->ste_pa+(uint64_t)l->sid*64u;
    if(pa!=ste || (pa&7))return EACCES;
    e=owner_slice(owner_ste_region(o),pa,8,&va);if(e)return e;
    __atomic_store_n((uint64_t *)va,value,__ATOMIC_RELEASE);
    ((uint64_t *)o->expected_ste)[l->sid*8]=value;return 0;
}
static int owner_io_clean(void *cookie,uint64_t pa,uint32_t bytes)
{
    struct owner_lease *l=cookie;struct x9_mm_smmu_owner *o=l->owner;vm_offset_t va=0;
    int e=owner_guard_locked(o);if(e)return e;
    uint64_t ste=o->ste_pa+(uint64_t)l->sid*64u;
    if(pa>=ste && pa-ste<64 && bytes<=64-(pa-ste))e=owner_slice(owner_ste_region(o),pa,bytes,&va);
    else e=owner_slice(&o->adoption.cmdq,pa,bytes,&va);
    if(e)return e;
    clean_dcache64(pa,bytes,TRUE);__asm__ volatile("dsb sy":::"memory");return 0;
}
static int owner_io_barrier(void *cookie)
{ struct owner_lease *l=cookie;int e=owner_guard_locked(l->owner);if(!e)__asm__ volatile("dsb sy":::"memory");return e; }
static int owner_io_publish(void *cookie,uint32_t old,uint32_t next)
{
    struct owner_lease *l=cookie;struct x9_mm_smmu_owner *o=l->owner;
    int e=owner_guard_locked(o);if(e)return e;
    uint32_t mask=(2u<<o->qshift)-1u;
    if(o->pending || o->staged_count!=4 || old!=o->prod || o->cons!=old ||
       next!=((old+4)&mask) || owner_rr(o,0x98)!=old || owner_rr(o,0x9c)!=old)return ESTALE;
    uint64_t expected[4][2]={{3u|((uint64_t)l->sid<<32),1},
        {6u|((uint64_t)l->sid<<32),0},{0x11u|((uint64_t)l->asid<<48),0},
        {0x46u|(UINT64_C(2)<<12)|(UINT64_C(3)<<22)|(UINT64_C(15)<<24),0}};
    if(memcmp(o->staged,expected,sizeof(expected)))return EACCES;
    __asm__ volatile("dsb sy":::"memory");
    o->prod=next;o->pending=true;
    *(volatile uint32_t *)(o->mmio+0x98)=next;
    __asm__ volatile("dsb sy":::"memory");
    return owner_rr(o,0x98)==next?0:EIO;
}
static uint64_t owner_io_clock(void *cookie)
{ uint64_t ns=0;(void)cookie;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);return ns; }
static int owner_io_pause(void *cookie,uint64_t deadline)
{ if(owner_io_clock(cookie)>=deadline)return ETIMEDOUT;delay(1);return owner_io_clock(cookie)>=deadline?ETIMEDOUT:0; }
int x9_mm_smmu_owner_install_io(struct x9_mm_smmu_owner *o,uint64_t token,
    struct x9_mm_smmu_install_io *out)
{
    if(!out)return EINVAL;
    lck_mtx_lock(&mm_owner_lock);int e=owner_identity(o);
    struct owner_lease *l=e?NULL:owner_find(o,token);if(!e && !l)e=ESTALE;
    if(!e) {
        *out=(struct x9_mm_smmu_install_io){.cookie=l,.reader={.cookie=l,.read=owner_io_read},
            .enter=owner_io_enter,.guard=owner_io_guard,.leave=owner_io_leave,.read_regs=owner_io_regs,
            .write=owner_io_write,.store64=owner_io_store,.clean=owner_io_clean,.barrier=owner_io_barrier,
            .publish=owner_io_publish,.clock_ns=owner_io_clock,.pause=owner_io_pause};
        out->reader.range_count=o->l1_bytes?2u:1u;
        out->reader.ranges[0]=(struct x9_mm_smmu_range){o->adoption.strtab.pa,o->adoption.strtab.bytes};
        if(o->l1_bytes)out->reader.ranges[1]=(struct x9_mm_smmu_range){o->adoption.strtab_l2.pa,o->adoption.strtab_l2.bytes};
    }
    lck_mtx_unlock(&mm_owner_lock);return e;
}
int x9_mm_smmu_owner_release_lease(struct x9_mm_smmu_owner *o,uint64_t *token)
{
    if(!token || !*token)return EINVAL;
    lck_mtx_lock(&mm_owner_lock);int e=owner_identity(o);
    struct owner_lease *l=e?NULL:owner_find(o,*token);if(!e && !l)e=ESTALE;
    if(!e && (l->mapping || (l->state!=X9_MM_SMMU_UNEXPOSED && l->state!=X9_MM_SMMU_RETIRED)))e=EBUSY;
    /* Callbacks carry a resident pointer, not a token argument on every call.
     * Keep its tombstone; reusing this slot could give an old callback cookie
     * authority over a new lease. These are the bus's two whole-domain
     * leases, not per-picture/session allocations. No same-boot cold restart. */
    if(!e){l->released=true;*token=0;}
    lck_mtx_unlock(&mm_owner_lock);return e;
}
static int owner_camera_enter(void *cookie,uint64_t token,unsigned op,
    const struct x9_camera_smmu_export *view)
{
    struct x9_mm_smmu_owner *o=cookie;
    lck_mtx_lock(&mm_owner_lock);int e=owner_identity(o);
    if(!e && (!token || token!=o->camera_token || !view || view->asid!=1 || op<1 || op>3))e=EINVAL;
    if(!e) {
        o->camera=view;o->active_sid=0x80;o->op=X9_MM_OWNER_CAMERA_INSTALL+op-1;
        e=owner_begin(o);
    }
    if(e){o->camera=NULL;o->active_sid=0;lck_mtx_unlock(&mm_owner_lock);}
    return e;
}
static int owner_camera_guard(void *cookie,uint64_t token,unsigned op,
    const struct x9_camera_smmu_export *view)
{
    struct x9_mm_smmu_owner *o=cookie;
    if(token!=o->camera_token || view!=o->camera || o->op!=X9_MM_OWNER_CAMERA_INSTALL+op-1)return ESTALE;
    return owner_guard_locked(o);
}
static void owner_camera_leave(void *cookie,uint64_t token,unsigned op)
{
    struct x9_mm_smmu_owner *o=cookie;vm_offset_t va=0;
    if(token!=o->camera_token || !o->camera || o->op!=X9_MM_OWNER_CAMERA_INSTALL+op-1)panic("X9 MM: mismatched camera owner leave");
    int e=owner_guard_locked(o);
    if(!e && owner_rr(o,0x98)!=owner_rr(o,0x9c))e=EBUSY;
    if(!e)e=owner_slice(owner_ste_region(o),o->ste_pa+0x80u*64u,64,&va);
    if(!e)memcpy((void *)(o->expected_ste+0x80u*64u),(const void *)va,64);
    else if(!o->fault)o->fault=(uint32_t)e;
    o->prod=o->cons=owner_rr(o,0x98);
    owner_auth_leave(o,e?X9_MM_SMMU_QUARANTINED:X9_MM_SMMU_INSTALLED);
    o->camera=NULL;o->active=false;o->active_sid=0;o->camera_queue_live=false;
    lck_mtx_unlock(&mm_owner_lock);
}
static void owner_camera_complete(void *cookie,uint64_t token,unsigned op,int result)
{
    struct x9_mm_smmu_owner *o=cookie;
    if(token!=o->camera_token || !o->active || !o->camera ||
       o->op!=X9_MM_OWNER_CAMERA_INSTALL+op-1)panic("X9 MM: mismatched camera result");
    if(result && !o->fault)o->fault=(uint32_t)result;
}
int x9_mm_smmu_owner_camera_host(struct x9_mm_smmu_owner *o,uint64_t epoch,
    struct x9_camera_smmu_host *out)
{
    if(!out || !epoch)return EINVAL;
    lck_mtx_lock(&mm_owner_lock);int e=owner_identity(o);
    if(!e && o->camera_token)e=EBUSY;
    if(!e) {
        uint64_t t=owner_token();if(!t)e=EOVERFLOW;
        else {
            o->camera_token=t;o->camera_epoch=epoch;
            *out=(struct x9_camera_smmu_host){.context=o,.lease=t,.enter=owner_camera_enter,
                .guard=owner_camera_guard,.leave=owner_camera_leave,.stream_guard=x9_mm_smmu_owner_stream_guard,
                .complete=owner_camera_complete};
            out->strtab=(struct x9_camera_smmu_host_region){o->adoption.strtab.va,o->adoption.strtab.pa,o->adoption.strtab.bytes,0};
            out->cmdq=(struct x9_camera_smmu_host_region){o->adoption.cmdq.va,o->adoption.cmdq.pa,o->adoption.cmdq.bytes,0};
            out->strtab_l2=(struct x9_camera_smmu_host_region){o->adoption.strtab_l2.va,o->adoption.strtab_l2.pa,o->adoption.strtab_l2.bytes,0};
        }
    }
    lck_mtx_unlock(&mm_owner_lock);return e;
}
int x9_mm_smmu_owner_stream_guard(void *cookie,uint64_t token,uint32_t sid,
    const uint64_t words[8])
{
    struct x9_mm_smmu_owner *o=cookie;
    if(!o || !words || sid>=256 || token!=o->camera_token || !o->active || !o->camera ||
       !o->authority_held || o->fault)return ESTALE;
    return memcmp(words,(const void *)(o->expected_ste+sid*64u),64)?ESTALE:0;
}
