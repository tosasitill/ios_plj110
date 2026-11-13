/* Pre-XNU audio handoffs from ONE bounded FDT collection.
 *
 * Replaces the per-field complete-tree lookups that stalled BOOT130 between
 * the GPS walker and the ADSP handoff (MMU/D-cache off, ~120 full walks, no
 * watchdog service). Now: one ADT walk for the three placeholders, then
 * exactly two FDT passes (collect + phandle batch), each servicing the
 * watchdog and bounded in time. Every refusal is nonfatal: a placeholder
 * stays zero, so the kernel consumers fail closed without a grant. */
#include "audio_fdt.h"
#include "hw.h"
#include "plog.h"
#include "../kernel/src/bsd/dev/x9_adsp_handoff.h"
#include "../kernel/src/bsd/dev/x9_adsp_audio_irq.h"
#include "../kernel/src/bsd/dev/x9_mic_dma_handoff.h"

static const char audio_paths[AN_COUNT][FDT_CAPTURE_PATH_BYTES]={
    "","/soc","/soc/adspsys@1a000000","/soc/adsp-core0@1a200000","/soc/adsp-core1@1a210000",
    "/interrupt-controller@6800000",
    "/soc/pinctrl","/soc/mt6993-afe-pcm@1a110000","/soc/sound",
    "/soc/pinctrl/aud-gpio-i2sin4-on","/soc/pinctrl/aud-gpio-i2sin4-on/pins-cmd1-dat",
    "/soc/pinctrl/aud-gpio-i2sin4-on/pins-cmd2-dat","/soc/pinctrl/aud-gpio-i2sin4-on/pins-cmd3-dat",
    "/soc/pinctrl/aud-gpio-i2sout4-on","/soc/pinctrl/aud-gpio-i2sout4-on/pins-cmd1-dat",
    "/spmi@1c01a000","/spmi@1c01a000/mt6661-3@3","/spmi@1c01a000/mt6661-3@3/mt6661-3-pinctrl",
    "/soc/i2c@12990000","/soc/i2c@12990000/tfa98xx@34","/soc/i2c@12990000/tfa98xx@35",
    "/spmi@1c01a000/mt6661-3@3/mt6661-3-pinctrl/tfa98xx_gpio_L",
    "/spmi@1c01a000/mt6661-3@3/mt6661-3-pinctrl/tfa98xx_gpio_L/pins_cmd_dat",
    "/spmi@1c01a000/mt6661-3@3/mt6661-3-pinctrl/tfa98xx_gpio_R",
    "/spmi@1c01a000/mt6661-3@3/mt6661-3-pinctrl/tfa98xx_gpio_R/pins_cmd_dat",
    "/spmi@1c01a000/mt6661-5@5","/spmi@1c01a000/mt6661-5@5/regulators",
    "/spmi@1c01a000/mt6661-5@5/regulators/mt6661-5-lnc0-8"};
_Static_assert(AN_COUNT<=FDT_COLLECT_MAX_NODES,"audio node table");

/* ~6 KiB: static, not on the 64 KiB shim stack. Used once per boot. */
static struct audio_fdt audio;

static uint32_t le(const uint8_t *p)
{return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static int exact(const uint8_t *p,size_t n,const char *s)
{size_t i=0;while(i<n&&s[i]&&p[i]==(uint8_t)s[i])i++;return i+1==n&&!s[i]&&!p[i];}
/* ADT property keys are NUL-padded to 32 bytes, unlike value strings. */
static int key_is(const uint8_t *p,const char *s)
{size_t i=0;while(i<32&&s[i]&&p[i]==(uint8_t)s[i])i++;return i<32&&!s[i]&&!p[i];}

enum { SlotADSP, SlotIRQ, SlotMIC, Slots };
static const char slot_key[Slots][24]={"x9-adsp-handoff","x9-adsp-audio-irq","x9-mic-dma-handoff"};
static const uint32_t slot_bytes[Slots]={X9_ADSP_HANDOFF_BYTES,
    sizeof(struct x9_adsp_audio_irq_layout),X9_MIC_DMA_HANDOFF_BYTES};
/* Whole ADT validation, independent of property order. Exactly one
 * unflagged, exact-size property per slot, and only in /chosen. */
struct walk {uint8_t *p;size_t size,at,found[Slots];unsigned nodes,matches[Slots];};
static int node(struct walk *w,unsigned depth)
{
    if(depth>32||++w->nodes>4096||w->at>w->size||w->size-w->at<8)return 0;
    uint32_t props=le(w->p+w->at),children=le(w->p+w->at+4);w->at+=8;
    if(props>1024||children>4096)return 0;
    unsigned names=0,chosen=0,local[Slots]={0};size_t candidate[Slots]={0};
    for(unsigned i=0;i<props;i++) {
        if(w->size-w->at<36)return 0;
        uint8_t *key=w->p+w->at;uint32_t raw=le(key+32),n=raw&0x7fffffff;w->at+=36;
        size_t padded=((size_t)n+3)&~(size_t)3;
        if(padded>w->size-w->at)return 0;
        if(key_is(key,"name")) {
            if(++names!=1||(raw>>31))return 0;
            chosen=depth==1&&exact(w->p+w->at,n,"chosen");
        }
        for(unsigned s=0;s<Slots;s++)if(key_is(key,slot_key[s])) {
            if(++local[s]!=1||raw!=slot_bytes[s])return 0;
            candidate[s]=w->at;
        }
        w->at+=padded;
    }
    for(unsigned s=0;s<Slots;s++)if(local[s]) {
        if(!chosen||++w->matches[s]!=1)return 0;
        w->found[s]=candidate[s];
    }
    for(unsigned i=0;i<children;i++)if(!node(w,depth+1))return 0;
    return 1;
}
static void copy(uint8_t *dst,const void *src,size_t n)
{const uint8_t *s=(const uint8_t *)src;for(size_t i=0;i<n;i++)dst[i]=s[i];}

int fdt_audio_patch_adt(uintptr_t dt,void *adt,size_t size)
{
    if(!adt||size>0x400000)return -1;
    struct walk w={.p=(uint8_t *)adt,.size=size};
    if(!node(&w,0)||w.at!=size)return -1;
    for(unsigned s=0;s<Slots;s++)if(w.matches[s]!=1)return -1;
    uint8_t *slot[Slots];
    for(unsigned s=0;s<Slots;s++){slot[s]=w.p+w.found[s];for(unsigned i=0;i<slot_bytes[s];i++)slot[s][i]=0;}

    struct audio_fdt *a=&audio;
    a->handles=0;
    plog_str("X9 AUDIO FDT: bounded collection pass 1/2\n");
    int r=fdt_collect(&a->c,dt,audio_paths,AN_COUNT,0);
    uint64_t freq=counter_frequency();
    plog_hex("X9 AUDIO FDT pass1 us",freq?a->c.ticks*1000000/freq:0);
    if(r!=FDT_PROPERTY_PRESENT) {
        plog_str("X9 AUDIO FDT: tree refused or over budget; no DMA/shared DRAM grant\n");
        return 0;
    }
    plog_hex("X9 AUDIO FDT nodes present",a->c.present);
    plog_hex("X9 AUDIO FDT properties",a->c.count);

    struct x9_adsp_handoff h={0};struct x9_adsp_audio_irq_layout q={0};
    unsigned features=0;
    int layout=adsp_qualify(a,&h,&q,&features);
    uint8_t mic[X9_MIC_DMA_HANDOFF_BYTES]={0};
    int mic_ok=mic_dma_qualify(a,mic);

    /* Pass 2: every phandle a candidate feature relies on, in one walk. A
     * handle that is duplicated, absent or misplaced withdraws only the
     * features that registered it. */
    uint32_t ok=0;
    if(a->handles) {
        r=fdt_collect_phandles(&a->c,a->handle,a->owner,a->handles,&ok);
        plog_hex("X9 AUDIO FDT pass1+2 us",freq?a->c.ticks*1000000/freq:0);
        if(r!=FDT_PROPERTY_PRESENT){ok=0;features=0;
            plog_str("X9 AUDIO FDT: phandle pass refused; optional topology withdrawn\n");}
    }
    for(unsigned i=0;i<a->handles;i++)if(!(ok&(1u<<i)))features&=~(unsigned)a->feature[i];
    if(!(features&AF_PA_RESET))features&=~AF_LEFT_VDD;
    plog_hex("X9 AUDIO FDT watchdog kicks",a->c.kicks);

    int granted=0;
    if(layout) {
        if(features&AF_I2S4){h.flags|=X9_ADSP_I2S4_VERIFIED;h.i2s4_pins=X9_ADSP_I2S4_PINS;h.i2s4_modes=X9_ADSP_I2S4_MODES;}
        if(features&AF_PA_RESET)h.flags|=X9_ADSP_PA_RESET_VERIFIED;
        if(features&AF_LEFT_VDD)h.flags|=X9_ADSP_PA_LEFT_VDD_VERIFIED;
        if(x9_adsp_handoff_valid(&h)) {
            copy(slot[SlotADSP],&h,sizeof(h));granted=1;
            if((features&AF_IRQ)&&q.version) {
                copy(slot[SlotIRQ],&q,sizeof(q));
                plog_str("X9 ADSP FDT: core-A AUDIO SPI570 level-high topology; IRQ not armed\n");
            }
            plog_str("X9 ADSP FDT: common pool and mailbox layout qualified; EMI/firmware not yet ready\n");
            if(h.flags&X9_ADSP_I2S4_VERIFIED)plog_str("X9 ADSP FDT: I2SIN4/OUT4 pinctrl metadata qualified; pads not owned\n");
            if(h.flags&X9_ADSP_PA_RESET_VERIFIED)plog_str("X9 ADSP FDT: SID3 dual active-low PA reset topology; no rail/reset lease\n");
            if(h.flags&X9_ADSP_PA_LEFT_VDD_VERIFIED)plog_str("X9 ADSP FDT: LEFT VDD SID5 LNC0_8 topology; RIGHT supply not inferred\n");
        } else plog_str("X9 ADSP FDT: handoff failed final ABI validation; no DRAM grant\n");
    } else plog_str("X9 ADSP FDT: shared layout refused; no DRAM grant\n");
    if(mic_ok) {
        copy(slot[SlotMIC],mic,sizeof(mic));granted=1;
        plog_str("X9 MIC DMA FDT: this boot's 64GiB direct bus window qualified; not AIN0 or drain\n");
    } else plog_str("X9 MIC DMA FDT: unknown/invalid topology; native DMA mapping disabled\n");
    return granted;
}
