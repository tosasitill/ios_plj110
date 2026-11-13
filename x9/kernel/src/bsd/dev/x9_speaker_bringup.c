/* One-shot PLJ110 speaker bring-up for the x9_dsp_boot test path (mode2).
 * Composes the existing combined-mode components in their documented lock
 * order instead of the unsupplied production lease providers:
 *   Speaker ticket -> ADSP boot (speaker_control 0) -> AUDIO/APLL2 clock
 *   sublease -> I2S4 pads -> AFE DL24->OUT4 (CPU producer) -> both TFA9865
 *   started per OPPO tfa98xx_v6.14.2 (tfa986x_specific, tfaRunStartup,
 *   tfa_dev_set_state) from the board's own container -> AMPE=1 -> a short
 *   low-level 1 kHz tone -> AMPE=0/PWDN=1 -> reverse teardown.
 * The container bytes come from generated/x9_tfa_container.inc (built from
 * .local, gitignored; an empty stub refuses before any hardware access).
 * Every step logs "X9 SPK:"; the first failure stops forward progress and
 * still runs the teardown of whatever was acquired. Never retried. */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <machine/machine_routines.h>
#include <vm/pmap.h>
#include <vm/vm_kern.h>
#include "x9_audio_resources.h"
#include "x9_adsp_mt6993.h"
#include "x9_adsp_transport.h"
#include "x9_speaker_mt6993.h"
#include "x9_speaker_pads.h"
#include "x9_speaker_afe.h"
#include "x9_speaker_dl24.h"
#include "x9_tfa_plj110.h"
#include "x9_speaker_bringup.h"
#include "../../../generated/x9_tfa_container.inc"
extern void IOSleep(unsigned int);
/* Logs 260/261: the SoC stopped (black screen, keys and USB dead) and the
 * last ~1 KiB of console never left the device; pstore has no console and
 * the UFS sidecar lags further. Sleep after each bring-up step's line so the
 * USB console drains before the next hardware step. Not used after the AFE
 * starts streaming (a 250 ms gap would underrun the 20 ms prefill). */
#define SPKLOG(...) do{printf(__VA_ARGS__);IOSleep(250);}while(0)
extern int sweet_x9_arg(const char *,int);
extern int sweet_early_is_x9(void);
extern ppnum_t pmap_find_phys(pmap_t,addr64_t);

#define DMA_BYTES 65536u
#define RATE 48000u
static struct {
    uint64_t ticket;
    uint32_t generation;
    volatile uint32_t *afe;
    vm_offset_t dma;
    uint64_t dma_bus;
    bool tfa,afe_held,afe_prepared,pads,clock,dsp,pa_on[2];
} b;

/* ---------------- TFA9865 (two slaves, same sequence) ---------------- */
static const uint8_t slaves[2]={0x34,0x35};
static int bf_get(unsigned i,uint16_t field,uint16_t *v)
{
    uint16_t x;int e=x9_tfa_plj110_read16(slaves[i],(uint8_t)(field>>8),&x);if(e)return e;
    unsigned pos=(field>>4)&15,len=(field&15)+1;
    uint32_t mask=len>=16?0xffffu:((1u<<len)-1);
    *v=(uint16_t)((x>>pos)&mask);return 0;
}
static int bf_set(unsigned i,uint16_t field,uint16_t value)
{
    uint8_t reg=(uint8_t)(field>>8);unsigned pos=(field>>4)&15,len=(field&15)+1;
    uint32_t mask=(len>=16?0xffffu:((1u<<len)-1))<<pos;
    uint16_t x;int e=x9_tfa_plj110_read16(slaves[i],reg,&x);if(e)return e;
    uint16_t next=(uint16_t)((x&~mask)|(((uint32_t)value<<pos)&mask));
    return next==x?0:x9_tfa_plj110_write16(slaves[i],reg,next);
}
static int wait_bf(unsigned i,uint16_t field,uint16_t want,unsigned tries)
{
    for(unsigned n=0;n<tries;n++) {
        uint16_t v;int e=bf_get(i,field,&v);if(e)return e;
        if(v==want)return 0;
        delay(1000);
    }
    return ETIMEDOUT;
}
/* Minimal container walk (format checked by x9_tfa98xx.c's rules plus the
 * v6.14.2 enum: 29..32 reserved, 33 revid). Returns item descriptors. */
static uint32_t le32(const uint8_t *p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static uint16_t le16(const uint8_t *p){return (uint16_t)(p[0]|(p[1]<<8));}
static int cnt_ok(uint32_t off,uint32_t n){return off>=54 && off<=x9_tfa_container_bytes && n<=x9_tfa_container_bytes-off;}
static int cnt_device(unsigned slave,uint32_t *dev)
{
    const uint8_t *p=x9_tfa_container;
    if(x9_tfa_container_bytes<54 || p[0]!='P' || p[1]!='M' || le32(p+6)!=x9_tfa_container_bytes || le16(p+40)!=2)return EINVAL;
    for(unsigned k=0;k<2;k++) {
        uint32_t d=le32(p+46+k*4),o=d&0xffffff;
        if((d>>24) || !cnt_ok(o,12))return EINVAL;
        if(p[o+2]==slave && !p[o+1]){if(!cnt_ok(o+12,p[o]*4u))return EINVAL;*dev=o;return 0;}
    }
    return ENOENT;
}
static int write_item(unsigned i,uint32_t d)
{
    const uint8_t *p=x9_tfa_container;uint32_t t=d>>24,o=d&0xffffff;
    if(t==16) {
        if(!cnt_ok(o,4))return EINVAL;
        return bf_set(i,le16(p+o+2),le16(p+o));
    }
    if(t==2) { /* reg8, value LE16, mask LE16 */
        if(!cnt_ok(o,5))return EINVAL;
        uint16_t x;int e=x9_tfa_plj110_read16(slaves[i],p[o],&x);if(e)return e;
        uint16_t m=le16(p+o+3),v=le16(p+o+1);
        return x9_tfa_plj110_write16(slaves[i],p[o],(uint16_t)((x&~m)|(v&m)));
    }
    return 0;
}
static int write_device_regs(unsigned i,unsigned *count)
{
    uint32_t dev;int e=cnt_device(slaves[i],&dev);if(e)return e;
    const uint8_t *p=x9_tfa_container;*count=0;
    for(unsigned k=0;k<p[dev];k++) {
        uint32_t d=le32(p+dev+12+k*4),t=d>>24;
        if(t==5 || t==4 || t==1)break; /* patch, file, profile */
        if(t==16 || t==2){e=write_item(i,d);if(e)return e;(*count)++;}
    }
    return 0;
}
/* OPPO/NXP select a profile by INDEX, the same index on every device (log
 * 259: profile 0 is "speaker" on 0x34 but "speakerR" on 0x35). */
static int write_profile_regs(unsigned i,unsigned index,unsigned *count)
{
    uint32_t dev;int e=cnt_device(slaves[i],&dev);if(e)return e;
    const uint8_t *p=x9_tfa_container;*count=0;unsigned seen=0;
    for(unsigned k=0;k<p[dev];k++) {
        uint32_t d=le32(p+dev+12+k*4),po=d&0xffffff;
        if((d>>24)!=1)continue;
        if(seen++!=index)continue;
        if(!cnt_ok(po,8) || le16(p+po+2)!=0x1234 || !p[po])return EINVAL;
        char name[33]={0};uint32_t no=le32(p+po+4)&0xffffff;
        for(unsigned c=0;c<32 && cnt_ok(no+c,1) && p[no+c];c++)name[c]=(char)p[no+c];
        printf("X9 SPK: PA%x profile %u = %s\n",slaves[i],index,name);
        unsigned items=p[po]-1u;if(!cnt_ok(po+8,items*4))return EINVAL;
        for(unsigned n=0;n<items;n++) {
            uint32_t pd=le32(p+po+8+n*4),t=pd>>24;
            if(t==17)break; /* default section */
            if(t==16 || t==2){e=write_item(i,pd);if(e)return e;(*count)++;}
        }
        return 0;
    }
    return ENOENT;
}
static const uint8_t init_reg[]={0x00,0x02,0x50,0x5a,0x5b,0x5c,0x5f,0x62,0x63,0x67,0x68,0x74,0x75,0x78,0x7c,0xcf,0xd7,0xdd};
static const uint16_t init_val[]={0xf241,0x0628,0xc000,0x5f4c,0x74e2,0x302b,0x00a0,0x05c4,0x80d4,0x0066,0x0820,0x60f0,0x0e00,0x0001,0x10f2,0x2cd7,0x1000,0x0036};
_Static_assert(sizeof(init_reg)==sizeof(init_val)/2,"TFA9865 MN1A3 table");
/* tfa986x_specific + tfaRunStartup up to (not including) INIT_CF. */
static int tfa_init_one(unsigned i)
{
    uint16_t rev=0,audfs=0,fract=0,x;int e=x9_tfa_plj110_read16(slaves[i],3,&rev);
    if(!e && (rev&0xff)!=0x66)e=ENOTSUP;
    if(!e)e=bf_get(i,0x0203,&audfs);
    if(!e)e=bf_get(i,0x0256,&fract);
    SPKLOG("X9 SPK: PA%x I2CR + leave powerdown\n",slaves[i]);
    if(!e)e=x9_tfa_plj110_write16(slaves[i],0x00,0x0002); /* I2CR */
    if(!e){delay(2000);e=bf_set(i,0x0000,0);}             /* PWDN=0 */
    if(!e)e=bf_set(i,0x0460,0);                           /* MANAOOSC=0 */
    if(!e)e=wait_bf(i,0x1203,1,50);                       /* MANSTATE=1 */
    uint32_t revid=rev;
    if(!e) {
        uint16_t ts=0,lpm=0,dcdc=0,dev=0;
        e=bf_get(i,0xf3b1,&ts);if(!e)e=bf_get(i,0xf090,&lpm);
        if(!e)e=bf_get(i,0xf4f0,&dcdc);if(!e)e=bf_get(i,0x0687,&dev);
        if(!e) {
            if(ts) {
                revid=rev-1u;
                if(lpm)revid=dcdc?revid:((1u<<20)|revid);
                else revid=(1u<<16)|revid;
                if(dcdc)revid-=1;
            }
            revid+=(uint32_t)(dev&3)*0x100000u;
        }
        SPKLOG("X9 SPK: PA%x rev=%04x revid=%x type_sel=%u lpm=%u dcdc=%u devrev=%u audfs=%u fractdel=%u\n",
               slaves[i],rev,revid,ts,lpm,dcdc,dev&3,audfs,fract);
        /* Log 258: both PAs read reg03=0x9866 -> revid 0x19865, absent from
         * OPPO tfa986x_specific's table: the vendor driver takes `default`
         * (warning only, NO chip table) and Android starts it that way.
         * 0x13a65 keeps its generated table; anything else is refused. */
        if(!e && revid!=0x13a65 && revid!=0x19865)e=ENOTSUP;
    }
    /* Log 260: the whole system stopped right after the rev line (screen
     * black, USB gone). Mark each key-unlock access so the next stop shows
     * which transfer it was. */
    if(!e){SPKLOG("X9 SPK: PA%x unlock 0f\n",slaves[i]);e=x9_tfa_plj110_write16(slaves[i],0x0f,0x5a6b);} /* unhide keys */
    if(!e){SPKLOG("X9 SPK: PA%x unlock read fb\n",slaves[i]);e=x9_tfa_plj110_read16(slaves[i],0xfb,&x);}
    if(!e){SPKLOG("X9 SPK: PA%x unlock a0 (fb=%04x)\n",slaves[i],x);e=x9_tfa_plj110_write16(slaves[i],0xa0,(uint16_t)(x^0x005a));}
    if(!e)e=x9_tfa_plj110_write16(slaves[i],0x0f,0x5a6b);
    if(!e){SPKLOG("X9 SPK: PA%x unlock a1\n",slaves[i]);e=x9_tfa_plj110_write16(slaves[i],0xa1,0x005a);} /* key2 unlock */
    if(!e && revid==0x13a65)
        for(unsigned k=0;!e && k<sizeof(init_reg);k++)e=x9_tfa_plj110_write16(slaves[i],init_reg[k],init_val[k]);
    else if(!e)SPKLOG("X9 SPK: PA%x revid %x: OPPO default branch, no chip table\n",slaves[i],revid);
    if(!e)SPKLOG("X9 SPK: PA%x powerdown + container\n",slaves[i]);
    if(!e)e=bf_set(i,0x0000,1);                           /* PWDN=1 */
    if(!e)e=wait_bf(i,0x1203,0,50);                       /* MANSTATE=0 */
    if(!e)e=bf_set(i,0x0460,1);                           /* MANAOOSC=1 */
    if(!e)e=bf_set(i,0x0203,audfs);
    if(!e)e=bf_set(i,0x0256,fract);
    unsigned nd=0,np=0;
    if(!e)e=write_device_regs(i,&nd);
    if(!e)e=write_profile_regs(i,0,&np);
    SPKLOG("X9 SPK: PA%x init result=%d device_items=%u profile_items=%u\n",slaves[i],e,nd,np);
    return e;
}
static int pa_mute(void *ctx,uint64_t ticket)
{
    (void)ctx;(void)ticket;
    if(!b.tfa)return EIO; /* no real PA access -> no mute proof */
    int first=0;
    for(unsigned i=0;i<2;i++) {
        int e=bf_set(i,0x0030,0);                         /* AMPE=0 */
        uint16_t v=1;if(!e)e=bf_get(i,0x0030,&v);
        if(!e && v)e=EIO;
        if(e && !first)first=e;
    }
    return first;
}
static void pa_report(const char *when)
{
    for(unsigned i=0;i<2;i++) {
        uint16_t r0=0,r10=0,r11=0,r12=0,r13=0;
        int e=x9_tfa_plj110_read16(slaves[i],0x00,&r0);
        if(!e)e=x9_tfa_plj110_read16(slaves[i],0x10,&r10);
        if(!e)e=x9_tfa_plj110_read16(slaves[i],0x11,&r11);
        if(!e)e=x9_tfa_plj110_read16(slaves[i],0x12,&r12);
        if(!e)e=x9_tfa_plj110_read16(slaves[i],0x13,&r13);
        printf("X9 SPK: PA%x %s err=%d sysctrl=%04x flags10=%04x stat11=%04x stat12=%04x flags13=%04x\n",
               slaves[i],when,e,r0,r10,r11,r12,r13);
    }
}

/* ---------------- AFE platform (CPU producer) ---------------- */
static int dl24_drained(void *ctx,uint64_t ticket,uint64_t bus,uint32_t bytes)
{
    (void)ctx;(void)ticket;(void)bus;(void)bytes;
    if(!b.afe)return ENXIO;
    uint32_t con=b.afe[0x46d0/4],out4=b.afe[0x1580/4];
    if((con&(1u<<28)) || (out4&1))return EBUSY;
    uint32_t c0=b.afe[0x46bc/4];delay(500);
    return b.afe[0x46bc/4]==c0?0:EBUSY;
}
static int afe_acquire(void *ctx){b.afe_held=true;return pa_mute(ctx,b.ticket);}
static int afe_release(void *ctx){(void)ctx;b.afe_held=false;return 0;}
static int afe_mute(void *ctx){return pa_mute(ctx,b.ticket);}
static int afe_quiesce(void *ctx){return dl24_drained(ctx,b.ticket,0,0);}
static int afe_sync(void *ctx,uint32_t off,uint32_t bytes)
{
    (void)ctx;
    if(off>DMA_BYTES || bytes>DMA_BYTES-off)return EINVAL;
    uint64_t ctr;__asm__ volatile("mrs %0, ctr_el0":"=r"(ctr));
    const uint64_t line=4ULL<<((ctr>>16)&15);
    uint64_t va=(b.dma+off)&~(line-1),end=b.dma+off+bytes;
    for(;va<end;va+=line)__asm__ volatile("dc cvac, %0"::"r"(va):"memory");
    __asm__ volatile("dsb sy":::"memory");return 0;
}
static bool afe_held(void *ctx){(void)ctx;return b.afe_held;}
static int afe_validate(void *ctx)
{
    (void)ctx;
    int e=x9_speaker_mt6993_clock_validate(b.ticket);
    if(!e)e=x9_speaker_pads_validate(b.ticket);
    return e;
}
static int afe_halt(void *ctx){(void)ctx;return 0;} /* CPU producer */

/* ---------------- tone ---------------- */
static int16_t sine_q15(uint32_t phase) /* 48 samples per 1 kHz cycle */
{
    static const int16_t q[13]={0,4277,8481,12539,16384,19947,23170,25996,28378,30273,31651,32487,32767};
    uint32_t k=phase%48;
    if(k<=12)return q[k];
    if(k<=24)return q[24-k];
    if(k<=36)return (int16_t)-q[k-24];
    return (int16_t)-q[48-k];
}
static void fill(int16_t *pcm,uint32_t frames,uint32_t *phase,int shift)
{
    for(uint32_t f=0;f<frames;f++) {
        int16_t v=shift<0?0:(int16_t)(sine_q15((*phase)++)>>shift);
        pcm[f*2]=v;pcm[f*2+1]=v;
    }
}

static void teardown(void)
{
    int e;
    if(b.tfa) {
        for(unsigned i=0;i<2;i++) {
            e=bf_set(i,0x0030,0);if(!e)e=bf_set(i,0x0000,1);
            printf("X9 SPK: PA%x off (AMPE=0 PWDN=1) -> %d\n",slaves[i],e);
        }
    }
    if(b.afe_prepared){e=x9_speaker_afe_stop();printf("X9 SPK: afe stop -> %d\n",e);if(!e)b.afe_prepared=false;}
    if(b.dsp){e=x9_adsp_mt6993_speaker_control(1,b.ticket);printf("X9 SPK: dsp halt -> %d\n",e);}
    if(b.pads){e=x9_speaker_pads_release(b.ticket);printf("X9 SPK: pads release -> %d\n",e);if(!e)b.pads=false;}
    if(b.clock && !b.pads){e=x9_speaker_mt6993_clock_release(b.ticket);printf("X9 SPK: clock release -> %d\n",e);if(!e)b.clock=false;}
    if(b.dsp && !b.clock){e=x9_adsp_mt6993_speaker_control(2,b.ticket);printf("X9 SPK: dsp release -> %d\n",e);if(!e)b.dsp=false;}
    if(b.tfa){e=x9_tfa_plj110_session_close();printf("X9 SPK: i2c6 session close -> %d\n",e);b.tfa=false;}
    if(b.ticket && !b.dsp && !b.clock && !b.pads && !b.afe_prepared) {
        e=x9_audio_resources_release(X9AudioResourceSpeaker,b.ticket);
        printf("X9 SPK: ticket release -> %d\n",e);if(!e)b.ticket=0;
    }
}

int x9_speaker_bringup_run(void)
{
    if(!sweet_early_is_x9() || sweet_x9_arg("x9_audio",0)!=2)return ENXIO;
    SPKLOG("X9 SPK: start container=%u bytes\n",x9_tfa_container_bytes);
    uint32_t probe;int e=x9_tfa_container_bytes?cnt_device(0x34,&probe):ENOENT;
    if(!e)e=cnt_device(0x35,&probe);
    if(e){SPKLOG("X9 SPK: container refused %d (no hardware touched)\n",e);return e;}
    e=kmem_alloc_contig(kernel_map,&b.dma,DMA_BYTES,PAGE_MASK,0,0,
        KMA_DATA|KMA_ZERO|KMA_PERMANENT,VM_KERN_MEMORY_IOKIT)==KERN_SUCCESS?0:ENOMEM;
    if(!e) {
        ppnum_t first=pmap_find_phys(kernel_pmap,(addr64_t)b.dma);
        b.dma_bus=ptoa_64(first);if(!first)e=ENOMEM;
        for(uint32_t off=0;!e && off<DMA_BYTES;off+=PAGE_SIZE)
            if(ptoa_64(pmap_find_phys(kernel_pmap,(addr64_t)(b.dma+off)))!=b.dma_bus+off)e=ENOMEM;
        if(!e)afe_sync(NULL,0,DMA_BYTES);
    }
    SPKLOG("X9 SPK: dma va=%lx bus=%llx -> %d\n",(unsigned long)b.dma,b.dma_bus,e);
    if(!e){e=x9_audio_resources_claim(X9AudioResourceSpeaker,&b.ticket);SPKLOG("X9 SPK: ticket=%llu -> %d\n",b.ticket,e);}
    if(!e){e=x9_tfa_plj110_session_open();b.tfa=!e;SPKLOG("X9 SPK: i2c6 session -> %d\n",e);}
    if(!e)pa_report("inherited");
    for(unsigned i=0;!e && i<2;i++)e=tfa_init_one(i); /* ends in PWDN=1: amplifier off */
    if(!e)SPKLOG("X9 SPK: dsp boot begin (warm takeover)\n");
    if(!e){e=x9_adsp_mt6993_speaker_control(0,b.ticket);b.dsp=true;SPKLOG("X9 SPK: dsp boot -> %d\n",e);}
    X9AudioDSPStatus ds={0};
    if(!e){e=x9_adsp_status(0,&ds);if(!e && ds.state!=X9DSPOnline)e=ESTALE;b.generation=ds.generation;
           SPKLOG("X9 SPK: dsp state=%u gen=%u -> %d\n",ds.state,ds.generation,e);}
    if(!e) {
        struct x9_speaker_clock_policy cp={X9_SPEAKER_CLOCK_POLICY_VERSION,b.generation,b.ticket,NULL,dl24_drained};
        SPKLOG("X9 SPK: clock acquire begin\n");
        e=x9_speaker_mt6993_clock_acquire(&cp);b.clock=true;
        X9AudioClockStatus cs;bzero(&cs,sizeof(cs));x9_speaker_mt6993_clock_status(&cs);
        SPKLOG("X9 SPK: clock acquire -> %d flags=%x stage=%u error=%u reg=%x val=%x\n",
               e,cs.flags,cs.stage,cs.error,cs.last_register,cs.last_value);
    }
    if(!e){e=x9_speaker_mt6993_clock_afe(b.ticket,&b.afe);SPKLOG("X9 SPK: clock afe -> %d\n",e);}
    if(!e) {
        struct x9_speaker_pads_policy pp={X9_SPEAKER_PADS_POLICY_VERSION,b.generation,b.ticket,NULL,pa_mute,dl24_drained};
        SPKLOG("X9 SPK: pads acquire begin\n");
        e=x9_speaker_pads_acquire(&pp);b.pads=true;SPKLOG("X9 SPK: pads acquire -> %d\n",e);
    }
    if(!e) {
        struct x9_speaker_afe_platform ap={0};
        ap.version=X9_SPEAKER_AFE_PLATFORM_VERSION;ap.verified=X9_SPEAKER_AFE_RESOURCES;
        ap.dma_bytes=DMA_BYTES;ap.memif=X9_SPEAKER_DL24_MEMIF;ap.afe=b.afe;
        ap.dma=(void *)b.dma;ap.dma_bus=b.dma_bus;
        ap.acquire=afe_acquire;ap.release=afe_release;ap.mute=afe_mute;ap.quiesce=afe_quiesce;
        ap.sync_device=afe_sync;ap.held=afe_held;ap.validate=afe_validate;ap.halt_producer=afe_halt;
        ap.producer=X9AFEProducerCPU;ap.generation=0;
        e=x9_speaker_afe_register(&ap);SPKLOG("X9 SPK: afe register -> %d\n",e);
        if(!e){e=x9_speaker_afe_prepare_on_ticket(b.ticket);b.afe_prepared=true;SPKLOG("X9 SPK: afe prepare -> %d\n",e);}
    }
    static int16_t pcm[960*2];uint32_t phase=0;uint64_t anchor=0;
    if(!e)SPKLOG("X9 SPK: afe start begin\n");
    if(!e){fill(pcm,960,&phase,-1);e=x9_speaker_afe_start(pcm,960,&anchor);printf("X9 SPK: afe start (silence) -> %d\n",e);}
    /* Keep ~100 ms of silence queued while both PAs leave powerdown: their
     * PLL locks to BCK/WS (CLKS). */
    uint64_t played=0,submitted=960;
    if(!e) {
        for(unsigned n=0;!e && n<5;n++){fill(pcm,960,&phase,-1);e=x9_speaker_afe_submit(pcm,960);if(!e)submitted+=960;}
        x9_speaker_afe_position(&played);
        printf("X9 SPK: silence queued submitted=%llu played=%llu -> %d\n",submitted,played,e);
    }
    for(unsigned i=0;!e && i<2;i++) {
        e=bf_set(i,0x0120,1);if(!e)e=bf_set(i,0x0000,0);  /* INIT_CF: MANSCONF=1 PWDN=0 */
        if(!e)e=wait_bf(i,0x1100,1,50);                   /* CLKS */
        printf("X9 SPK: PA%x clocks -> %d\n",slaves[i],e);
    }
    /* Keep the ring fed while waiting. */
    if(!e){for(unsigned n=0;!e && n<5;n++){fill(pcm,960,&phase,-1);e=x9_speaker_afe_submit(pcm,960);if(!e)submitted+=960;}}
    if(!e)pa_report("powered");
    /* User request (2026-11-13): no audible output. Stop at INIT_CF with
     * the amplifier OFF (AMPE stays 0); the AFE streams silence only. */
    printf("X9 SPK: quiet run: AMPE stays 0, silence only\n");
    /* -24 dB (>>4) 1 kHz for ~2 s, refilled from the hardware cursor. */
    phase=0;unsigned tone_frames=2u*RATE,sent=0;
    uint64_t start=mach_absolute_time(),limit;nanoseconds_to_absolutetime(4000000000ULL,&limit);
    while(!e && sent<tone_frames) {
        e=x9_speaker_afe_position(&played);if(e)break;
        if(submitted-played<9600) {
            fill(pcm,960,&phase,-1);e=x9_speaker_afe_submit(pcm,960);
            if(!e){submitted+=960;sent+=960;}else if(e==EAGAIN)e=0;
        } else delay(5000);
        if(mach_absolute_time()-start>limit){e=ETIMEDOUT;break;}
    }
    printf("X9 SPK: tone sent=%u submitted=%llu played=%llu -> %d\n",sent,submitted,played,e);
    if(b.tfa)pa_report("after-tone");
    /* Mute first, then let silence drain the ring before stopping. */
    if(b.pa_on[0]||b.pa_on[1]){int m=pa_mute(NULL,b.ticket);printf("X9 SPK: mute -> %d\n",m);}
    teardown();
    X9AudioAmpStatus as;bzero(&as,sizeof(as));x9_tfa_plj110_status(&as);
    printf("X9 SPK: done result=%d i2c flags=%x stage=%u error=%u restore=%u writes=%u req=%x\n",
           e,as.flags,as.stage,as.error,as.restore_error,as.revision_read,as.request_status);
    return e;
}
