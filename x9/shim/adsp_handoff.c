/* Derive ADSP common pool from THIS LK FDT, not an Android dump. No MMIO,
 * memory access in the reserve, EMI reprogramming or firmware operations. */
#include "fdt.h"
#include "plog.h"
#include "../kernel/src/bsd/dev/x9_adsp_handoff.h"
#include "../kernel/src/bsd/dev/x9_adsp_audio_irq.h"
_Static_assert(sizeof(struct x9_adsp_handoff)==X9_ADSP_HANDOFF_BYTES,"ADSP handoff ABI");
static uint32_t be(const uint8_t *p)
{return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static uint32_t le(const uint8_t *p)
{return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static int eq(const uint8_t *p,size_t n,const char *s)
{size_t i=0;while(i<n&&s[i]&&p[i]==(uint8_t)s[i])i++;return i<n&&!s[i]&&!p[i];}
static int word(uintptr_t dt,const char *path,const char *key,uint32_t *v,int optional)
{
    const uint8_t *p;size_t n;
    int found=fdt_property_checked(dt,path,key,&p,&n);
    if(found!=1){if(found==0&&optional){*v=0;return 1;}return 0;}
    if(n!=4)return 0;*v=be(p);return 1;
}
static int value(uintptr_t dt,const char *path,const char *key,uint32_t v)
{uint32_t a;return word(dt,path,key,&a,0)&&a==v;}
static int str(uintptr_t dt,const char *path,const char *key,const char *v)
{const uint8_t *p;size_t n;return fdt_property(dt,path,key,&p,&n)&&eq(p,n,v);}
enum { CaptureRoot,CaptureSoc,CaptureSystem,CaptureCoreA,CaptureCoreB,CaptureGIC };
struct adsp_capture {struct fdt_captured_property p[128];unsigned count;};
static int captured_get(const struct adsp_capture *c,unsigned node,const char *key,const uint8_t **p,size_t *n)
{
    *p=0;*n=0;
    for(unsigned i=0;i<c->count;i++)if(c->p[i].node==node) {
        const char *a=c->p[i].key,*b=key;while(*a&&*a==*b){a++;b++;}
        if(*a==*b){*p=c->p[i].value;*n=c->p[i].bytes;return 1;}
    }
    return 0; /* node existence/key uniqueness already proved by full capture */
}
static int captured_word(const struct adsp_capture *c,unsigned node,const char *key,uint32_t *v,int optional)
{
    const uint8_t *p;size_t n;
    if(!captured_get(c,node,key,&p,&n)){if(optional){*v=0;return 1;}return 0;}
    if(n!=4)return 0;*v=be(p);return 1;
}
static int captured_value(const struct adsp_capture *c,unsigned node,const char *key,uint32_t value)
{uint32_t v;return captured_word(c,node,key,&v,0)&&v==value;}
static int captured_str(const struct adsp_capture *c,unsigned node,const char *key,const char *value)
{const uint8_t *p;size_t n;return captured_get(c,node,key,&p,&n)&&eq(p,n,value);}
static int decode_named_reg(const uint8_t *r,size_t rb,const uint8_t *names,size_t nb,
                            const char *name,uint32_t *base,uint32_t *bytes)
{
    if(!rb||rb>4096||rb%16||!nb||nb>2048)return 0;
    size_t at=0;unsigned i=0,found=0;
    while(at<nb) {
        size_t end=at;while(end<nb&&names[end])end++;
        if(end==nb || (size_t)i*16>=rb)return 0;
        if(eq(names+at,end-at+1,name)) {
            if(be(r+i*16)||be(r+i*16+8))return 0;
            *base=be(r+i*16+4);*bytes=be(r+i*16+12);found++;
        }
        at=end+1;i++;
    }
    return (size_t)i*16==rb&&found==1;
}
static int named_reg(uintptr_t dt,const char *path,const char *name,uint32_t *base,uint32_t *bytes)
{
    const uint8_t *r,*names;size_t rb,nb;
    return fdt_property(dt,path,"reg",&r,&rb)&&fdt_property(dt,path,"reg-names",&names,&nb)&&
        decode_named_reg(r,rb,names,nb,name,base,bytes);
}
static int captured_named_reg(const struct adsp_capture *c,unsigned node,const char *name,uint32_t *base,uint32_t *bytes)
{
    const uint8_t *r,*names;size_t rb,nb;
    return captured_get(c,node,"reg",&r,&rb)&&captured_get(c,node,"reg-names",&names,&nb)&&
        decode_named_reg(r,rb,names,nb,name,base,bytes);
}
#include "adsp_i2s4.inc"
#include "adsp_pa_reset.inc"
#include "adsp_pa_vdd.inc"
/* Four cells per interrupt are required by THIS GIC binding. Reject an
 * alternate parent/extended/map instead of silently decoding as 3 cells. */
static int qualify_audio_irq(uintptr_t dt,const struct adsp_capture *c,struct x9_adsp_audio_irq_layout *q)
{
    const uint8_t *p;size_t n;uint32_t ph;
    if(!captured_str(c,CaptureGIC,"compatible","arm,gic-v3") ||
       !captured_value(c,CaptureGIC,"#interrupt-cells",4) ||
       !captured_word(c,CaptureGIC,"phandle",&ph,0) || !ph ||
       fdt_phandle_unique(dt,"/interrupt-controller@6800000",ph)!=FDT_PROPERTY_PRESENT ||
       !captured_get(c,CaptureGIC,"interrupt-controller",&p,&n) || n ||
       !captured_get(c,CaptureGIC,"reg",&p,&n) || n!=32 ||
       be(p) || be(p+4)!=0x06800000 || be(p+8) || be(p+12)!=0x40000 ||
       be(p+16) || be(p+20)!=0x06880000 || be(p+24) || be(p+28)!=0x200000)return 0;
    /* Root and soc use the same parent; a core-local override must agree. */
    for(unsigned i=CaptureRoot;i<=CaptureSoc;i++)
        if(!captured_value(c,i,"interrupt-parent",ph) ||
           captured_get(c,i,"interrupt-map",&p,&n))return 0;
    if(captured_get(c,CaptureCoreA,"interrupt-parent",&p,&n) &&
       (n!=4 || be(p)!=ph))return 0;
    if(captured_get(c,CaptureCoreA,"interrupts-extended",&p,&n) ||
       !captured_get(c,CaptureCoreA,"interrupt-names",&p,&n))return 0;
    const char names[]="sysirq\0wdt\0audioirq";
    if(n!=sizeof(names))return 0;
    for(unsigned i=0;i<n;i++)if(p[i]!=(uint8_t)names[i])return 0;
    if(!captured_get(c,CaptureCoreA,"interrupts",&p,&n) || n!=48)return 0;
    for(unsigned i=0;i<3;i++)if(be(p+i*16) || be(p+i*16+4)!=0x216+i*2 ||
        be(p+i*16+8)!=4 || be(p+i*16+12))return 0;
    *q=(struct x9_adsp_audio_irq_layout){1,0,X9_ADSP_AUDIO_INTID,4,
        0x1a000000,0x06800000,0x40000,0};
    return x9_adsp_audio_irq_layout_valid(q);
}
static int qualify(uintptr_t dt,struct x9_adsp_handoff *h,struct x9_adsp_audio_irq_layout *q)
{
    /* Inline arrays are required: pointer tables create ABS64 relocations
     * which are invalid when LK relocates the shim. */
    const char paths[][FDT_CAPTURE_PATH_BYTES]={"","/soc","/soc/adspsys@1a000000",
        "/soc/adsp-core0@1a200000","/soc/adsp-core1@1a210000","/interrupt-controller@6800000"};
    const char mem[][24]={"adsp-rsv-logger-a","adsp-rsv-dbg-dump-a","adsp-rsv-ipidma-a",
        "adsp-rsv-core-dump-a","adsp-rsv-mbrain-a","adsp-rsv-logger-b",
        "adsp-rsv-dbg-dump-b","adsp-rsv-ipidma-b","adsp-rsv-core-dump-b",
        "adsp-rsv-mbrain-b","adsp-rsv-c2c","adsp-rsv-pcie","adsp-rsv-l2sram",
        "adsp-rsv-xhci","adsp-rsv-audio"};
    const char cfg_names[][8]={"cfg","cfg2"};
    const char core_compat[][40]={"mediatek,mt6993-adsp_core_0","mediatek,mt6993-adsp_core_1"};
    const char mbox_names[][12]={"mbox0_base","mbox0_set","mbox0_clr","mbox1_base","mbox1_set",
        "mbox1_clr","mbox2_base","mbox2_set","mbox2_clr","mbox3_base","mbox3_set","mbox3_clr"};
    const uint8_t *p;size_t n;
    struct adsp_capture c;
    /* One complete-tree pass, not one rescan per scalar or named register.
     * Captured pointers are local to this synchronous call, not a cache. */
    if(fdt_capture_nodes(dt,paths,6,c.p,128,&c.count)!=FDT_PROPERTY_PRESENT)return 0;
    plog_str("X9 ADSP FDT: required nodes captured; validating shared layout\n");
    for(unsigned i=0;i<2;i++)if(!captured_value(&c,i,"#address-cells",2)||
       !captured_value(&c,i,"#size-cells",2))return 0;
    if(!captured_get(&c,CaptureSoc,"ranges",&p,&n)||n||
       !captured_str(&c,CaptureSystem,"compatible","mediatek,mt6993-adspsys")||
       !captured_str(&c,CaptureSystem,"status","okay")||
       !captured_value(&c,CaptureSystem,"core-num",2)||!captured_value(&c,CaptureSystem,"system-l2sram",1)||
       !captured_get(&c,CaptureSystem,"shared_memory",&p,&n)||n!=16)return 0;
    h->reserve_base=(uint64_t)be(p)<<32|be(p+4);
    h->reserve_bytes=(uint64_t)be(p+8)<<32|be(p+12);
    if(!h->reserve_bytes || h->reserve_bytes>0x4000000 || h->reserve_base>UINT64_MAX-h->reserve_bytes)return 0;
    /* Absence from the current allocator map is mandatory. fdt_ram_available
     * is the wrong predicate: this pool SHOULD be reserved, not allocatable.
     * Require the full live-map boot mode; a small/default map grants none. */
    if(fdt_ram_live_bytes()<(8ULL<<30))return 0;
    for(unsigned i=0;i<128;i++) {
        uint64_t lo,hi;if(!fdt_ram_live_tuple(i,&lo,&hi))break;
        if(h->reserve_base<hi && lo<h->reserve_base+h->reserve_bytes)return 0;
    }
    uint32_t at=0;
    for(unsigned i=0;i<X9_ADSP_MEM_COUNT;i++) {
        uint32_t bytes;
        if(!captured_word(&c,CaptureSystem,mem[i],&bytes,i==X9ADSPMemL2)||bytes>0x4000000)return 0;
        h->block[i].bytes=bytes;
        if(!bytes)continue;
        uint32_t padded=(bytes+0xfff)&~0xfffu;
        if(at>h->reserve_bytes || padded>h->reserve_bytes-at)return 0;
        h->block[i].offset=at;at+=padded;
    }
    h->allocated_bytes=at;
    for(unsigned i=0;i<2;i++) {
        if(!captured_named_reg(&c,CaptureSystem,cfg_names[i],&h->cfg_base[i],&h->cfg_bytes[i])||
           !captured_str(&c,CaptureCoreA+i,"compatible",core_compat[i])||
           !captured_str(&c,CaptureCoreA+i,"status","okay")||
           !captured_get(&c,CaptureCoreA+i,"system",&p,&n)||n!=16||be(p)||be(p+8))return 0;
        h->system_base[i]=be(p+4);h->system_bytes[i]=be(p+12);
    }
    for(unsigned i=0;i<4;i++) {
        uint32_t bytes;
        if(!captured_named_reg(&c,CaptureSystem,mbox_names[i*3],&h->mbox[i].base,&bytes)||bytes!=0x100||
           !captured_named_reg(&c,CaptureSystem,mbox_names[i*3+1],&h->mbox[i].set,&bytes)||bytes!=4||
           !captured_named_reg(&c,CaptureSystem,mbox_names[i*3+2],&h->mbox[i].clear,&bytes)||bytes!=4)return 0;
    }
    if(!captured_get(&c,CaptureSystem,"mbox-attr",&p,&n)||n!=16)return 0;
    for(unsigned i=0;i<4;i++)if(be(p+i*4))return 0;
    /* Finish every reader of the required capture before reusing its bounded
     * storage for optional LEFT supply metadata. No extra capture stack. */
    (void)qualify_audio_irq(dt,&c,q);
    h->version=1;h->flags=X9_ADSP_LAYOUT_VERIFIED;h->cores=2;
    if(qualify_i2s4(dt)) {
        h->flags|=X9_ADSP_I2S4_VERIFIED;
        h->i2s4_pins=X9_ADSP_I2S4_PINS;h->i2s4_modes=X9_ADSP_I2S4_MODES;
    }
    if(qualify_pa_reset(dt))h->flags|=X9_ADSP_PA_RESET_VERIFIED;
    if((h->flags&X9_ADSP_PA_RESET_VERIFIED)&&qualify_left_vdd(dt,&c))
        h->flags|=X9_ADSP_PA_LEFT_VDD_VERIFIED;
    if(!x9_adsp_handoff_valid(h))return 0;
    return 1;
}
struct walk {uint8_t *p;size_t size,at,found,irq_found;unsigned nodes,matches,irq_matches;};
static int node(struct walk *w,unsigned depth)
{
    if(depth>32||++w->nodes>1024||w->at>w->size||w->size-w->at<8)return 0;
    uint32_t props=le(w->p+w->at),children=le(w->p+w->at+4);w->at+=8;
    if(props>1024||children>1024)return 0;
    size_t candidate=0;unsigned chosen=0,found=0,irq_found=0;size_t irq_candidate=0;
    for(unsigned i=0;i<props;i++) {
        if(w->size-w->at<36)return 0;
        uint8_t *key=w->p+w->at;uint32_t n=le(key+32)&0x7fffffff;w->at+=36;
        size_t padded=((size_t)n+3)&~(size_t)3;
        if(padded>w->size-w->at)return 0;
        if(eq(key,32,"name"))chosen=depth==1&&eq(w->p+w->at,n,"chosen");
        if(eq(key,32,"x9-adsp-handoff")){if(n!=X9_ADSP_HANDOFF_BYTES)return 0;candidate=w->at;found++;}
        if(eq(key,32,"x9-adsp-audio-irq")){if(n!=sizeof(struct x9_adsp_audio_irq_layout))return 0;irq_candidate=w->at;irq_found++;}
        w->at+=padded;
    }
    if(chosen&&found){w->found=candidate;w->matches+=found;}
    if(chosen&&irq_found){w->irq_found=irq_candidate;w->irq_matches+=irq_found;}
    for(unsigned i=0;i<children;i++)if(!node(w,depth+1))return 0;
    return 1;
}
int fdt_adsp_patch_adt(uintptr_t dt,void *adt,size_t bytes)
{
    plog_str("X9 ADSP FDT: entering handoff slot validation\n");
    struct walk w={.p=(uint8_t *)adt,.size=bytes};
    if(!node(&w,0)||w.at!=bytes||w.matches!=1||w.irq_matches!=1)return -1;
    uint8_t *dst=w.p+w.found;
    for(unsigned i=0;i<X9_ADSP_HANDOFF_BYTES;i++)dst[i]=0;
    uint8_t *irq_dst=w.p+w.irq_found;
    for(unsigned i=0;i<sizeof(struct x9_adsp_audio_irq_layout);i++)irq_dst[i]=0;
    struct x9_adsp_handoff h={0};struct x9_adsp_audio_irq_layout q={0};
    plog_str("X9 ADSP FDT: capturing this-boot nodes\n");
    if(!qualify(dt,&h,&q)){plog_str("X9 ADSP FDT: shared layout refused; no DRAM grant\n");return 0;}
    const uint8_t *src=(const uint8_t *)&h;
    for(unsigned i=0;i<X9_ADSP_HANDOFF_BYTES;i++)dst[i]=src[i];
    src=(const uint8_t *)&q;
    for(unsigned i=0;i<sizeof(q);i++)irq_dst[i]=src[i];
    if(q.version)plog_str("X9 ADSP FDT: core-A AUDIO SPI570 level-high topology; IRQ not armed\n");
    plog_str("X9 ADSP FDT: common pool and mailbox layout qualified; EMI/firmware not yet ready\n");
    if(h.flags&X9_ADSP_I2S4_VERIFIED)plog_str("X9 ADSP FDT: I2SIN4/OUT4 pinctrl metadata qualified; pads not owned\n");
    if(h.flags&X9_ADSP_PA_RESET_VERIFIED)plog_str("X9 ADSP FDT: SID3 dual active-low PA reset topology; no rail/reset lease\n");
    if(h.flags&X9_ADSP_PA_LEFT_VDD_VERIFIED)plog_str("X9 ADSP FDT: LEFT VDD SID5 LNC0_8 topology; RIGHT supply not inferred\n");
    return 1;
}
