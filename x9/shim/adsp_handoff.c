/* Derive ADSP common pool from THIS LK FDT, not an Android dump. No MMIO,
 * memory access in the reserve, EMI reprogramming or firmware operations. */
#include "fdt.h"
#include "plog.h"
#include "../kernel/src/bsd/dev/x9_adsp_handoff.h"
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
static int named_reg(uintptr_t dt,const char *path,const char *name,uint32_t *base,uint32_t *bytes)
{
    const uint8_t *r,*names;size_t rb,nb;
    if(!fdt_property(dt,path,"reg",&r,&rb)||!rb||rb>4096||rb%16||
       !fdt_property(dt,path,"reg-names",&names,&nb)||!nb||nb>2048)return 0;
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
#include "adsp_i2s4.inc"
static int qualify(uintptr_t dt,struct x9_adsp_handoff *h)
{
    const char *sys="/soc/adspsys@1a000000";
    /* Inline arrays are required: pointer tables create ABS64 relocations
     * which are invalid when LK relocates the shim. */
    const char roots[][24]={"","/soc"};
    const char mem[][24]={"adsp-rsv-logger-a","adsp-rsv-dbg-dump-a","adsp-rsv-ipidma-a",
        "adsp-rsv-core-dump-a","adsp-rsv-mbrain-a","adsp-rsv-logger-b",
        "adsp-rsv-dbg-dump-b","adsp-rsv-ipidma-b","adsp-rsv-core-dump-b",
        "adsp-rsv-mbrain-b","adsp-rsv-c2c","adsp-rsv-pcie","adsp-rsv-l2sram",
        "adsp-rsv-xhci","adsp-rsv-audio"};
    const char cfg_names[][8]={"cfg","cfg2"};
    const char core_paths[][32]={"/soc/adsp-core0@1a200000","/soc/adsp-core1@1a210000"};
    const char core_compat[][40]={"mediatek,mt6993-adsp_core_0","mediatek,mt6993-adsp_core_1"};
    const char mbox_names[][12]={"mbox0_base","mbox0_set","mbox0_clr","mbox1_base","mbox1_set",
        "mbox1_clr","mbox2_base","mbox2_set","mbox2_clr","mbox3_base","mbox3_set","mbox3_clr"};
    const uint8_t *p;size_t n;
    for(unsigned i=0;i<2;i++)if(!value(dt,roots[i],"#address-cells",2)||!value(dt,roots[i],"#size-cells",2))return 0;
    if(!fdt_property(dt,"/soc","ranges",&p,&n)||n||
       !str(dt,sys,"compatible","mediatek,mt6993-adspsys")||!str(dt,sys,"status","okay")||
       !value(dt,sys,"core-num",2)||!value(dt,sys,"system-l2sram",1)||
       !fdt_property(dt,sys,"shared_memory",&p,&n)||n!=16)return 0;
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
        if(!word(dt,sys,mem[i],&bytes,i==X9ADSPMemL2)||bytes>0x4000000)return 0;
        h->block[i].bytes=bytes;
        if(!bytes)continue;
        uint32_t padded=(bytes+0xfff)&~0xfffu;
        if(at>h->reserve_bytes || padded>h->reserve_bytes-at)return 0;
        h->block[i].offset=at;at+=padded;
    }
    h->allocated_bytes=at;
    for(unsigned i=0;i<2;i++) {
        if(!named_reg(dt,sys,cfg_names[i],&h->cfg_base[i],&h->cfg_bytes[i])||
           !str(dt,core_paths[i],"compatible",core_compat[i])||!str(dt,core_paths[i],"status","okay")||
           !fdt_property(dt,core_paths[i],"system",&p,&n)||n!=16||be(p)||be(p+8))return 0;
        h->system_base[i]=be(p+4);h->system_bytes[i]=be(p+12);
    }
    for(unsigned i=0;i<4;i++) {
        uint32_t bytes;
        if(!named_reg(dt,sys,mbox_names[i*3],&h->mbox[i].base,&bytes)||bytes!=0x100||
           !named_reg(dt,sys,mbox_names[i*3+1],&h->mbox[i].set,&bytes)||bytes!=4||
           !named_reg(dt,sys,mbox_names[i*3+2],&h->mbox[i].clear,&bytes)||bytes!=4)return 0;
    }
    if(!fdt_property(dt,sys,"mbox-attr",&p,&n)||n!=16)return 0;
    for(unsigned i=0;i<4;i++)if(be(p+i*4))return 0;
    h->version=1;h->flags=X9_ADSP_LAYOUT_VERIFIED;h->cores=2;
    if(qualify_i2s4(dt)) {
        h->flags|=X9_ADSP_I2S4_VERIFIED;
        h->i2s4_pins=X9_ADSP_I2S4_PINS;h->i2s4_modes=X9_ADSP_I2S4_MODES;
    }
    return x9_adsp_handoff_valid(h);
}
struct walk {uint8_t *p;size_t size,at,found;unsigned nodes,matches;};
static int node(struct walk *w,unsigned depth)
{
    if(depth>32||++w->nodes>1024||w->at>w->size||w->size-w->at<8)return 0;
    uint32_t props=le(w->p+w->at),children=le(w->p+w->at+4);w->at+=8;
    if(props>1024||children>1024)return 0;
    size_t candidate=0;unsigned chosen=0,found=0;
    for(unsigned i=0;i<props;i++) {
        if(w->size-w->at<36)return 0;
        uint8_t *key=w->p+w->at;uint32_t n=le(key+32)&0x7fffffff;w->at+=36;
        size_t padded=((size_t)n+3)&~(size_t)3;
        if(padded>w->size-w->at)return 0;
        if(eq(key,32,"name"))chosen=depth==1&&eq(w->p+w->at,n,"chosen");
        if(eq(key,32,"x9-adsp-handoff")){if(n!=X9_ADSP_HANDOFF_BYTES)return 0;candidate=w->at;found++;}
        w->at+=padded;
    }
    if(chosen&&found){w->found=candidate;w->matches+=found;}
    for(unsigned i=0;i<children;i++)if(!node(w,depth+1))return 0;
    return 1;
}
int fdt_adsp_patch_adt(uintptr_t dt,void *adt,size_t bytes)
{
    struct walk w={(uint8_t *)adt,bytes,0,0,0,0};
    if(!node(&w,0)||w.at!=bytes||w.matches!=1)return -1;
    uint8_t *dst=w.p+w.found;
    for(unsigned i=0;i<X9_ADSP_HANDOFF_BYTES;i++)dst[i]=0;
    struct x9_adsp_handoff h={0};
    if(!qualify(dt,&h)){plog_str("X9 ADSP FDT: shared layout refused; no DRAM grant\n");return 0;}
    const uint8_t *src=(const uint8_t *)&h;
    for(unsigned i=0;i<X9_ADSP_HANDOFF_BYTES;i++)dst[i]=src[i];
    plog_str("X9 ADSP FDT: common pool and mailbox layout qualified; EMI/firmware not yet ready\n");
    if(h.flags&X9_ADSP_I2S4_VERIFIED)plog_str("X9 ADSP FDT: I2SIN4/OUT4 pinctrl metadata qualified; pads not owned\n");
    return 1;
}
