/* Derive ADSP common pool from THIS LK FDT, not an Android dump. No MMIO,
 * memory access in the reserve, EMI reprogramming or firmware operations.
 * Every field comes from the single audio fdt_collect table (audio_fdt.h);
 * phandle uniqueness is registered here and checked in one later pass. */
#include "audio_fdt.h"
#include "plog.h"
#include "../kernel/src/bsd/dev/x9_adsp_handoff.h"
#include "../kernel/src/bsd/dev/x9_adsp_audio_irq.h"
_Static_assert(sizeof(struct x9_adsp_handoff)==X9_ADSP_HANDOFF_BYTES,"ADSP handoff ABI");
static uint32_t be(const uint8_t *p)
{return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static int eq(const uint8_t *p,size_t n,const char *s)
{size_t i=0;while(i<n&&s[i]&&p[i]==(uint8_t)s[i])i++;return i<n&&!s[i]&&!p[i];}
static int get(const struct audio_fdt *a,unsigned node,const char *key,const uint8_t **p,size_t *n)
{return fdt_collect_get(&a->c,node,key,p,n)==1;}
/* Absent optional words read as 0 only from a present node. */
static int word(const struct audio_fdt *a,unsigned node,const char *key,uint32_t *v,int optional)
{
    const uint8_t *p;size_t n;
    int found=fdt_collect_get(&a->c,node,key,&p,&n);
    if(found!=1){if(found==0&&optional){*v=0;return 1;}return 0;}
    if(n!=4)return 0;*v=be(p);return 1;
}
static int value(const struct audio_fdt *a,unsigned node,const char *key,uint32_t v)
{uint32_t x;return word(a,node,key,&x,0)&&x==v;}
static int str(const struct audio_fdt *a,unsigned node,const char *key,const char *v)
{const uint8_t *p;size_t n;return get(a,node,key,&p,&n)&&eq(p,n,v);}
/* status absent, or "okay"; a missing node is never enabled. */
static int enabled(const struct audio_fdt *a,unsigned node)
{
    const uint8_t *p;size_t n;int r=fdt_collect_get(&a->c,node,"status",&p,&n);
    return r==0 || (r==1&&eq(p,n,"okay"));
}
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
static int named_reg(const struct audio_fdt *a,unsigned node,const char *name,uint32_t *base,uint32_t *bytes)
{
    const uint8_t *r,*names;size_t rb,nb;
    return get(a,node,"reg",&r,&rb)&&get(a,node,"reg-names",&names,&nb)&&
        decode_named_reg(r,rb,names,nb,name,base,bytes);
}
#include "adsp_i2s4.inc"
#include "adsp_pa_reset.inc"
#include "adsp_pa_vdd.inc"
/* Four cells per interrupt are required by THIS GIC binding. Reject an
 * alternate parent/extended/map instead of silently decoding as 3 cells. */
static int qualify_audio_irq(struct audio_fdt *a,struct x9_adsp_audio_irq_layout *q)
{
    const uint8_t *p;size_t n;uint32_t ph;
    if(!str(a,AN_GIC,"compatible","arm,gic-v3") ||
       !value(a,AN_GIC,"#interrupt-cells",4) ||
       !word(a,AN_GIC,"phandle",&ph,0) || !ph ||
       !get(a,AN_GIC,"interrupt-controller",&p,&n) || n ||
       !get(a,AN_GIC,"reg",&p,&n) || n!=32 ||
       be(p) || be(p+4)!=0x06800000 || be(p+8) || be(p+12)!=0x40000 ||
       be(p+16) || be(p+20)!=0x06880000 || be(p+24) || be(p+28)!=0x200000)return 0;
    /* Root and soc use the same parent; a core-local override must agree. */
    for(unsigned i=AN_ROOT;i<=AN_SOC;i++)
        if(!value(a,i,"interrupt-parent",ph) || get(a,i,"interrupt-map",&p,&n))return 0;
    if(get(a,AN_CORE0,"interrupt-parent",&p,&n) && (n!=4 || be(p)!=ph))return 0;
    if(get(a,AN_CORE0,"interrupts-extended",&p,&n) ||
       !get(a,AN_CORE0,"interrupt-names",&p,&n))return 0;
    const char names[]="sysirq\0wdt\0audioirq";
    if(n!=sizeof(names))return 0;
    for(unsigned i=0;i<n;i++)if(p[i]!=(uint8_t)names[i])return 0;
    if(!get(a,AN_CORE0,"interrupts",&p,&n) || n!=48)return 0;
    for(unsigned i=0;i<3;i++)if(be(p+i*16) || be(p+i*16+4)!=0x216+i*2 ||
        be(p+i*16+8)!=4 || be(p+i*16+12))return 0;
    *q=(struct x9_adsp_audio_irq_layout){1,0,X9_ADSP_AUDIO_INTID,4,
        0x1a000000,0x06800000,0x40000,0};
    if(!x9_adsp_audio_irq_layout_valid(q))return 0;
    return audio_fdt_need(a,ph,AN_GIC,AF_IRQ);
}
static int qualify_layout(const struct audio_fdt *a,struct x9_adsp_handoff *h)
{
    static const char mem[][24]={"adsp-rsv-logger-a","adsp-rsv-dbg-dump-a","adsp-rsv-ipidma-a",
        "adsp-rsv-core-dump-a","adsp-rsv-mbrain-a","adsp-rsv-logger-b",
        "adsp-rsv-dbg-dump-b","adsp-rsv-ipidma-b","adsp-rsv-core-dump-b",
        "adsp-rsv-mbrain-b","adsp-rsv-c2c","adsp-rsv-pcie","adsp-rsv-l2sram",
        "adsp-rsv-xhci","adsp-rsv-audio"};
    static const char cfg_names[][8]={"cfg","cfg2"};
    static const char core_compat[][40]={"mediatek,mt6993-adsp_core_0","mediatek,mt6993-adsp_core_1"};
    static const char mbox_names[][12]={"mbox0_base","mbox0_set","mbox0_clr","mbox1_base","mbox1_set",
        "mbox1_clr","mbox2_base","mbox2_set","mbox2_clr","mbox3_base","mbox3_set","mbox3_clr"};
    const uint32_t required=(1u<<AN_ROOT)|(1u<<AN_SOC)|(1u<<AN_ADSPSYS)|
        (1u<<AN_CORE0)|(1u<<AN_CORE1)|(1u<<AN_GIC);
    const uint8_t *p;size_t n;
    if((a->c.present&required)!=required)return 0;
    for(unsigned i=AN_ROOT;i<=AN_SOC;i++)if(!value(a,i,"#address-cells",2)||
       !value(a,i,"#size-cells",2))return 0;
    if(!get(a,AN_SOC,"ranges",&p,&n)||n||
       !str(a,AN_ADSPSYS,"compatible","mediatek,mt6993-adspsys")||
       !str(a,AN_ADSPSYS,"status","okay")||
       !value(a,AN_ADSPSYS,"core-num",2)||!value(a,AN_ADSPSYS,"system-l2sram",1)||
       !get(a,AN_ADSPSYS,"shared_memory",&p,&n)||n!=16)return 0;
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
        if(!word(a,AN_ADSPSYS,mem[i],&bytes,i==X9ADSPMemL2)||bytes>0x4000000)return 0;
        h->block[i].bytes=bytes;
        if(!bytes)continue;
        uint32_t padded=(bytes+0xfff)&~0xfffu;
        if(at>h->reserve_bytes || padded>h->reserve_bytes-at)return 0;
        h->block[i].offset=at;at+=padded;
    }
    h->allocated_bytes=at;
    for(unsigned i=0;i<2;i++) {
        if(!named_reg(a,AN_ADSPSYS,cfg_names[i],&h->cfg_base[i],&h->cfg_bytes[i])||
           !str(a,AN_CORE0+i,"compatible",core_compat[i])||
           !str(a,AN_CORE0+i,"status","okay")||
           !get(a,AN_CORE0+i,"system",&p,&n)||n!=16||be(p)||be(p+8))return 0;
        h->system_base[i]=be(p+4);h->system_bytes[i]=be(p+12);
    }
    for(unsigned i=0;i<4;i++) {
        uint32_t bytes;
        if(!named_reg(a,AN_ADSPSYS,mbox_names[i*3],&h->mbox[i].base,&bytes)||bytes!=0x100||
           !named_reg(a,AN_ADSPSYS,mbox_names[i*3+1],&h->mbox[i].set,&bytes)||bytes!=4||
           !named_reg(a,AN_ADSPSYS,mbox_names[i*3+2],&h->mbox[i].clear,&bytes)||bytes!=4)return 0;
    }
    if(!get(a,AN_ADSPSYS,"mbox-attr",&p,&n)||n!=16)return 0;
    for(unsigned i=0;i<4;i++)if(be(p+i*4))return 0;
    return 1;
}
int adsp_qualify(struct audio_fdt *a,struct x9_adsp_handoff *h,struct x9_adsp_audio_irq_layout *q,unsigned *features)
{
    *features=0;
    if(!qualify_layout(a,h))return 0;
    h->version=1;h->flags=X9_ADSP_LAYOUT_VERIFIED;h->cores=2;
    if(qualify_audio_irq(a,q))*features|=AF_IRQ;
    else *q=(struct x9_adsp_audio_irq_layout){0};
    if(qualify_i2s4(a))*features|=AF_I2S4;
    if(qualify_pa_reset(a)) {
        *features|=AF_PA_RESET;
        if(qualify_left_vdd(a))*features|=AF_LEFT_VDD;
    }
    return 1;
}
