/* ISP8s PHY3.0 analog-only lifetime, under camera_control_lock.
 * Real A+B banks for unsplit DPHY4/CPHY3, not only A or MAC-top.
 * Initial controls and each confirmed mutation are journaled independently.
 * RGS/RO results are excluded from control guards; ANA9 is NEVER written,
 * despite vendor power_on writing completion=1 after a fixed delay.
 * Source: MT6993 csirx_phyA_init/setting/power_on and mtk-devinfo, not the
 * UniXNU/HakiOS Qualcomm PHY, SMMUv2 or physical-address DMA route.
 */
#include <pexpert/device_tree.h>
#include "x9_camera_phy.h"
#include "x9_camera_calibration.h"
_Static_assert(sizeof(struct x9_camera_calibration)==X9_CAM_CALIBRATION_BYTES,"calibration ABI");
enum { CAM_PHY_A, CAM_PHY_B, CAM_PHY_D, CAM_PHY_C, CAM_PHY_BANKS,
    CAM_PHY_MAX_REGS=96, CAM_PHY_NO_REG=CAM_PHY_MAX_REGS };
#define CAM_PHY_CAL_MASK 0x003f0000u
struct cam_phy_desc { unsigned offset; uint32_t mask, idle_zero; };
/* Named writable controls, no AD/packet/IRQ-result banks. Mixed force words
 * include only RG bits; reads of their adjacent RO fields are not guards. */
static const struct cam_phy_desc cam_phy_analog[]={
    {0x000,0xf7ffffff,0x07008803}, /* CDPHY_RX_ANA_0 */
    {0x004,0x77007777,0x77000000}, /* CDPHY_RX_ANA_1 */
    {0x008,0x77f31f1f,0x00f30000}, /* CDPHY_RX_ANA_2 */
    {0x00c,0x79f31f1f,0x70000000}, /* CDPHY_RX_ANA_3 */
    {0x010,0x77771f1f,0x00000000}, /* CDPHY_RX_ANA_4 */
    {0x014,0xc707ffff,0x00000000}, /* CDPHY_RX_ANA_5 */
    {0x018,0x1f1f1f1f,0x00000000}, /* CDPHY_RX_ANA_6 */
    {0x01c,0x1f1f1f1f,0x00000000}, /* CDPHY_RX_ANA_7 */
    {0x020,0xf73fffff,0xf73f0000}, /* CDPHY_RX_ANA_8 */
    {0x028,0xbfbfbfbf,0x80808080}, /* CDPHY_RX_ANA_10 */
    {0x02c,0x0000bfbf,0x00008080}, /* CDPHY_RX_ANA_11 */
    {0x030,0x3f3f3f3f,0x00000000}, /* CDPHY_RX_ANA_12 */
    {0x034,0x00031f1f,0x00000000}, /* CDPHY_RX_ANA_13 */
    {0x038,0x073f0177,0x003f0000}, /* CDPHY_RX_ANA_14 */
    {0x040,0x00000333,0x00000000}, /* CDPHY_RX_ANA_FORCE_MODE_0 */
    {0x044,0x01330733,0x01330733}, /* CDPHY_RX_ANA_FORCE_MODE_EN_0 */
    {0x050,0x00000111,0x00000000}, /* CDPHY_RX_ANA_FORCE_MODE_1 */
    {0x054,0x00000111,0x00000111}, /* CDPHY_RX_ANA_FORCE_MODE_EN_1 */
    {0x060,0x00000333,0x00000000}, /* CDPHY_RX_ANA_FORCE_MODE_2 */
    {0x064,0x00000333,0x00000333}, /* CDPHY_RX_ANA_FORCE_MODE_EN_2 */
    {0x070,0x00000333,0x00000000}, /* CDPHY_RX_ANA_FORCE_MODE_3 */
    {0x074,0x00000333,0x00000333}, /* CDPHY_RX_ANA_FORCE_MODE_EN_3 */
    {0x080,0x00000133,0x00000000}, /* CDPHY_RX_ANA_FORCE_MODE_4 */
    {0x084,0x00000133,0x00000133}, /* CDPHY_RX_ANA_FORCE_MODE_EN_4 */
    {0x090,0x00000707,0x00000000}, /* CDPHY_RX_ANA_FORCE_MODE_5 */
    {0x094,0x0000f707,0x0000f707}, /* CDPHY_RX_ANA_FORCE_MODE_EN_5 */
    {0x0b0,0x000003ff,0x00000000}, /* CDPHY_RX_ANA_FORCE_MODE_6 */
    {0x0b4,0x00000301,0x00000301}, /* CDPHY_RX_ANA_FORCE_MODE_EN_6 */
    {0x0c0,0x000003ff,0x00000000}, /* CDPHY_RX_ANA_FORCE_MODE_7 */
    {0x0c4,0x00000301,0x00000301}, /* CDPHY_RX_ANA_FORCE_MODE_EN_7 */
    {0x0d0,0x000003ff,0x00000000}, /* CDPHY_RX_ANA_FORCE_MODE_8 */
    {0x0d4,0x00000301,0x00000301}, /* CDPHY_RX_ANA_FORCE_MODE_EN_8 */
    {0x0f0,0xff3ffffb,0xff000003}, /* CDPHY_RX_ANA_SETTING_0 */
    {0x0f4,0x7fff73ff,0x00007300}, /* CDPHY_RX_ANA_SETTING_1 */
    {0x100,0x01f3f3f3,0x00c3c3c3}, /* CDPHY_RX_ASYM_AFIFO_CTRL_0 */
    {0x150,0x0000ffff,0x0000ffff}, /* ULPS IRQ_EN/CLR; never write */
    {0x160,1,1}, {0x168,0x11,0x11} /* PMSR and forced digital CG */
};
struct cam_phy_reg {
    unsigned bank, offset; uint32_t mask, idle_zero, before, expected, owned;
};
static struct {
    vm_offset_t bank[CAM_PHY_BANKS];
    struct cam_phy_reg reg[CAM_PHY_MAX_REGS];
    unsigned count,pending_reg;
    uint32_t pending_next,calibration_pending,calibration_done;
    uint64_t epoch;
    bool held,close_attempted;
    int error;
    struct x9_camera_phy_status status;
} cam_phy;
static void cam_phy_status(void)
{
    cam_phy.status.last_errno=cam_phy.error;cam_phy.status.pending_reg=cam_phy.pending_reg;
    cam_phy.status.calibration_pending=cam_phy.calibration_pending;
    cam_phy.status.calibration_done=cam_phy.calibration_done;cam_phy.status.dirty_count=0;
    for(unsigned n=0;n<cam_phy.count;n++)if(cam_phy.reg[n].owned)cam_phy.status.dirty_count++;
    if(cam_phy.error && cam_phy.held)cam_phy.status.flags|=X9_CAM_PHY_RETAINED;
    else cam_phy.status.flags&=~X9_CAM_PHY_RETAINED;
}
static int cam_phy_fault(int e)
{if(!cam_phy.error)cam_phy.error=e;cam_phy_status();return e;}
static int cam_phy_calibration(unsigned sensor,uint32_t *word)
{
    struct x9_camera_calibration h;
    DTEntry chosen;const void *p=NULL;unsigned int n=0;
    if(SecureDTLookupEntry(NULL,"/chosen",&chosen)!=kSuccess ||
       SecureDTGetProperty(chosen,"x9-camera-calibration",&p,&n)!=kSuccess ||
       !p || n!=sizeof(h))return ENXIO;
    memcpy(&h,p,sizeof(h));
    if(h.version!=1 || h.flags!=X9_CAM_CALIBRATION_LK_DATA || h.port_mask!=9 ||
       h.word_count<=0x1dc/4 || h.word_count>400 || h.reserved[0] || h.reserved[1])return EINVAL;
    *word=sensor==X9_CAMERA_BACK?h.main:h.front;
    /* Do not print devinfo or copy calibration words into persisted logs. */
    return 0;
}
static unsigned cam_phy_add(unsigned bank,unsigned offset,uint32_t mask,uint32_t idle_zero)
{
    unsigned n;
    if(cam_phy.count>=CAM_PHY_MAX_REGS)return CAM_PHY_NO_REG;
    n=cam_phy.count++;cam_phy.reg[n]=(struct cam_phy_reg){bank,offset,mask,idle_zero,0,0,0};return n;
}
static uint32_t cam_phy_read(unsigned n)
{const struct cam_phy_reg *r=&cam_phy.reg[n];return cam_rr(cam_phy.bank[r->bank],r->offset)&r->mask;}
/* Deliberately non-recursive: only the MM base and our own controls. MAC
 * and OUTMUX call this too, keeping PHY conflicts above their teardown. */
static int cam_phy_guard(void)
{
    int e;
    if(!cam_phy.held)return 0;
    if(cam_phy.epoch!=cam_rx_live.sensor_epoch || cam_rx_live.epoch!=cam_mm_live.epoch)return ESTALE;
    e=cam_mm_guard(&cam_mm,true);if(e)return e;
    for(unsigned n=0;n<cam_phy.count;n++) {
        uint32_t v=cam_phy_read(n);
        if(v!=cam_phy.reg[n].expected && !(n==cam_phy.pending_reg && v==cam_phy.pending_next))return ESTALE;
    }
    for(unsigned b=0;b<2;b++)if(cam_phy.calibration_done&(1u<<b)) {
        unsigned n=CAM_PHY_NO_REG;
        for(unsigned k=0;k<cam_phy.count;k++)if(cam_phy.reg[k].bank==b&&cam_phy.reg[k].offset==0x20)n=k;
        if(n==CAM_PHY_NO_REG)return EINVAL;
        /* Completion must remain asserted while CAL_EN is still enabled.
         * During the one confirmed stop it may drop; no ack/reset is used
         * to consume a pending request, and done remains historical only. */
        if((cam_phy_read(n)&CAM_PHY_CAL_MASK)==CAM_PHY_CAL_MASK) {
            cam_phy.status.completion[b]=cam_rr(cam_phy.bank[b],0x24);
            if((cam_phy.status.completion[b]&CAM_PHY_CAL_MASK)!=CAM_PHY_CAL_MASK)return EIO;
        }
    }
    return 0;
}
static int cam_phy_dependencies(void)
{
    int e=cam_phy_guard();if(!e)e=cam_rx_session_guard();
    if(!e)e=cam_mac_guard();if(!e)e=cam_out_guard();return e;
}
static unsigned cam_phy_find(unsigned bank,unsigned offset)
{for(unsigned n=0;n<cam_phy.count;n++)if(cam_phy.reg[n].bank==bank&&cam_phy.reg[n].offset==offset)return n;return CAM_PHY_NO_REG;}
/* Only a single uncertain posted control write. For mixed words, preserve
 * reserved bits but do NOT replay read-only results or command strobes. */
static int cam_phy_write(unsigned bank,unsigned offset,uint32_t mask,uint32_t value)
{
    unsigned n=cam_phy_find(bank,offset);
    struct cam_phy_reg *r;
    uint32_t raw,next,v;
    int e;
    if(n==CAM_PHY_NO_REG)return EINVAL;
    r=&cam_phy.reg[n];if(mask&~r->mask)return EINVAL;
    if(cam_phy.pending_reg!=CAM_PHY_NO_REG)return EBUSY;
    e=cam_phy_dependencies();if(e)return e;
    raw=cam_rr(cam_phy.bank[bank],offset);
    if((raw&r->mask)!=r->expected)return ESTALE;
    next=(raw&~mask)|(value&mask);
    if((next&r->mask)==r->expected)return 0;
    if(offset==0x20 && (mask&CAM_PHY_CAL_MASK) && (value&CAM_PHY_CAL_MASK))
        cam_phy.calibration_pending|=1u<<bank;
    r->owned|=mask;cam_phy.pending_reg=n;cam_phy.pending_next=next&r->mask;
    cam_rw(cam_phy.bank[bank],offset,next);v=cam_phy_read(n);
    if(v!=cam_phy.pending_next)return EIO;
    r->expected=v;cam_phy.pending_reg=CAM_PHY_NO_REG;
    return cam_phy_dependencies();
}
static int cam_phy_pause(unsigned us)
{int e=cam_phy_dependencies();if(e)return e;delay(us);return cam_phy_dependencies();}
/* Configure only the two preview bands. Rates are sensor table-derived
 * nominal settings, NOT hardware measurements or a SENINF throughput vote.
 * Termination defaults occur only for an explicitly present all-zero LK
 * word, as vendor apply_efuse_data; missing data never silently defaults. */
static int cam_phy_settings(uint32_t efuse,bool cphy)
{
    int e=0;
    for(unsigned b=CAM_PHY_A;b<=CAM_PHY_B&&!e;b++) {
        unsigned shift=b==CAM_PHY_A?27:12;
        uint32_t term[3];
        for(unsigned k=0;k<3;k++)term[k]=efuse?(efuse>>(shift-5*k))&31:16;
#define PHY_SET(o,m,v) do { if(!e)e=cam_phy_write(b,(o),(m),(v)); } while(0)
        PHY_SET(4,0x777,0x444); /* LP thresholds/VREF */
        PHY_SET(8,0x77031f1f,0x44000000|term[0]|(term[0]<<8)); /* ALP, selfcal off */
        PHY_SET(0xc,0x09f01f1f,(cphy?0x09400000:0x09000000)|term[1]|(term[1]<<8));
        PHY_SET(0x10,0x1f1f,term[2]|(term[2]<<8));
        PHY_SET(0x14,0xffbf,cphy?0x117:4); /* nominal EQ band, inherited latch preserved */
        PHY_SET(0x38,0x03000100,0x01000000); /* LDO LP1/X26M0, EQ_OS_IS0 */
        PHY_SET(0x18,0x1f1f1f1f,cphy?0x06080808:0x04090909);
        PHY_SET(0x1c,0x1f1f1f1f,cphy?0x06080808:0x04090909);
        if(cphy)PHY_SET(0x34,0x1f1f,0x606);
        PHY_SET(0x20,0xffff,0x400); /* MT6993 reserve, not MT6899 */
        PHY_SET(0x100,0x202020,0x202020); /* three 16-bit AFIFOs; no FLUSH/CLEAR */
        PHY_SET(0,cphy?0x007000c0:0x007077c0,cphy?0x100000:(0x7c0|(b==CAM_PHY_A?0x4000:0)));
        /* set_phya_clk precedes _set_csi. CPHY vendor setting0=322
         * overwrites REF_CK_SEL to0, unlike DPHY's retained A oscillator1.
         * Do not silently turn the CPHY word into32a or claim an osc rate. */
        if(cphy)PHY_SET(0xf0,0xf03ffffb,0x322); /* no SW_RST/DELAY_APPLY */
        else if(b==CAM_PHY_A)PHY_SET(0xf0,0x100000f8,0x28);
        if(b==CAM_PHY_A)PHY_SET(0xf4,0x7fff00fc,cphy?0x416000c4:0x40630044);
#undef PHY_SET
    }
    return e;
}
/* Fresh 0 -> all six1 only. A fixed delay or writing CPLT=1 is never
 * evidence of calibration. ANA9's unknown ack semantics are not guessed.
 * A timeout/error AFTER CAL_EN write leaves the request pending forever
 * for this boot; close must not reset/disable it to manufacture drain. */
static int cam_phy_power_calibrate(unsigned b)
{
    uint64_t span,deadline;
    int e;
    cam_phy.status.bank=b;cam_phy.status.stage=3;
    e=cam_phy_pause(200);if(e)return e; /* admitted CORE/LPF/RSTB/CAL already0 */
    e=cam_phy_write(b,0,1,1);if(!e)e=cam_phy_pause(30);
    if(!e)e=cam_phy_write(b,0,2,2);if(!e)e=cam_phy_pause(5);
    if(!e)e=cam_phy_write(b,0,0x800,0x800);if(!e)e=cam_phy_pause(1);
    if(e)return e;
    cam_phy.status.completion[b]=cam_rr(cam_phy.bank[b],0x24);
    if(cam_phy.status.completion[b]&CAM_PHY_CAL_MASK)return EBUSY;
    /* Mark BEFORE the posted request. No exact readback exception consumes
     * this calibration; only the real status observations below may do so. */
    e=cam_phy_write(b,0x20,CAM_PHY_CAL_MASK,CAM_PHY_CAL_MASK);if(e)return e;
    nanoseconds_to_absolutetime(10000000,&span);deadline=mach_absolute_time()+span;
    e=cam_phy_pause(25);if(e)return e;
    cam_phy.status.stage=4;
    for(unsigned i=0;i<10000;i++) {
        e=cam_phy_dependencies();if(e)return e;
        if(mach_absolute_time()>=deadline)return ETIMEDOUT;
        cam_phy.status.completion[b]=cam_rr(cam_phy.bank[b],0x24);
        if((cam_phy.status.completion[b]&CAM_PHY_CAL_MASK)==CAM_PHY_CAL_MASK) {
            e=cam_phy_pause(1);if(e)return e;
            if(mach_absolute_time()>=deadline)return ETIMEDOUT;
            cam_phy.status.completion[b]=cam_rr(cam_phy.bank[b],0x24);
            if((cam_phy.status.completion[b]&CAM_PHY_CAL_MASK)!=CAM_PHY_CAL_MASK)return EIO;
            cam_phy.calibration_pending&=~(1u<<b);cam_phy.calibration_done|=1u<<b;return 0;
        }
        delay(1);
    }
    return ETIMEDOUT;
}
static int cam_phy_open_locked(uint64_t epoch,struct x9_camera_phy_status *s)
{
    static vm_offset_t maps[2][CAM_PHY_BANKS];
    struct x9_camera_pipeline plan;
    uint32_t again[CAM_PHY_MAX_REGS],efuse;
    unsigned idx=s->sensor==X9_CAMERA_BACK?0:1;
    int e;
    if(cam_phy.held)return EALREADY;
    if(cam_out.held)return EBUSY;
    if(!cam_rx_sensor_held_locked(epoch) || cam_rx_live.error || cam_rx_live.close_attempted || !cam_mac.held || cam_mac.close_attempted ||
       cam_mac.epoch!=epoch || !(cam_mac.status.flags&X9_CAM_MAC_CONFIGURED) || cam_mac.error)return ESTALE;
    e=cam_pipeline_plan(s->sensor,&plan);if(e)return e;
    if(plan.csi_port!=(idx?0u:3u)||plan.cphy!=(idx?0u:1u)||plan.lanes!=(idx?4u:3u)||plan.dt!=0x2b)return ENOTSUP;
    e=cam_phy_calibration(s->sensor,&efuse);if(e)return e;
    e=cam_mac_dependencies();if(e)return e;
    cam_phy.count=0;
    for(unsigned b=0;b<CAM_PHY_BANKS;b++) {
        if(!maps[idx][b])maps[idx][b]=ml_io_map(plan.csi_base+b*0x1000ull,b?0x1000:0xf00);
        cam_phy.bank[b]=maps[idx][b];if(!cam_phy.bank[b])return ENOMEM;
    }
    for(unsigned b=CAM_PHY_A;b<=CAM_PHY_B;b++)
        for(unsigned n=0;n<sizeof(cam_phy_analog)/sizeof(cam_phy_analog[0]);n++) {
            const struct cam_phy_desc *d=&cam_phy_analog[n];cam_phy_add(b,d->offset,d->mask,d->idle_zero);
        }
    cam_phy_add(CAM_PHY_D,0,0x80000f03,0x80000f03); /* LC/LD enable, SW_RST */
    cam_phy_add(CAM_PHY_D,8,0x3ff03,0x3ff03); /* HS lane/PN swap all off */
    cam_phy_add(CAM_PHY_D,0x94,0xc000000f,0xf); /* IRQ EN only */
    cam_phy_add(CAM_PHY_C,0,0xf00ff,0xf00ff); /* LP/HS/BIST lanes */
    cam_phy_add(CAM_PHY_C,0x168,0xf,0xf); /* valid-pipe */
    cam_phy_add(CAM_PHY_C,0xf0,0x8000f1ff,0xf0ff); /* IRQ EN only */
    cam_phy_add(CAM_PHY_D,0x50,0xff3fffff,0xc0000100); /* deskew enable/reset/LP11 off */
    cam_phy_add(CAM_PHY_D,0x100,0x70f,0x10f); /* BIST/lane + PHY2MAC gen off */
    cam_phy_add(CAM_PHY_D,0x41c,0xffffff01,1); /* idle SR BIST off */
    cam_phy_add(CAM_PHY_C,0x100,0xffffff0f,0xf0f); /* state-check and count-init off */
    if(cam_phy.count>=CAM_PHY_MAX_REGS)return EOVERFLOW;
    e=cam_mm_guard(&cam_mm,true);if(e)return e;
    for(unsigned n=0;n<cam_phy.count;n++)cam_phy.reg[n].before=cam_phy_read(n);
    delay(10);e=cam_mm_guard(&cam_mm,true);if(e)return e;
    for(unsigned n=0;n<cam_phy.count;n++)again[n]=cam_phy_read(n);
    for(unsigned n=0;n<cam_phy.count;n++) {
        struct cam_phy_reg *r=&cam_phy.reg[n];
        if(again[n]!=r->before)return ESTALE;
        if(r->before&r->idle_zero)return EBUSY;
        r->expected=r->before;
    }
    /* No inherited calibration history can be acknowledged speculatively. */
    for(unsigned b=0;b<2;b++)if(cam_rr(cam_phy.bank[b],0x24)&CAM_PHY_CAL_MASK)return EBUSY;
    cam_phy.status=*s;cam_phy.status.flags=X9_CAM_PHY_ATTEMPTED|X9_CAM_PHY_LK_EFUSE;
    if(!efuse)cam_phy.status.flags|=X9_CAM_PHY_DEFAULT_TERM;
    cam_phy.status.csi_port=plan.csi_port;cam_phy.status.cphy=plan.cphy;
    cam_phy.status.nominal_data_rate=plan.cphy?3300004166ull:2160000000ull;
    cam_phy.epoch=epoch;cam_phy.held=true;cam_phy.close_attempted=false;
    cam_phy.error=0;cam_phy.pending_reg=CAM_PHY_NO_REG;
    cam_phy.calibration_pending=cam_phy.calibration_done=0;cam_phy.status.stage=2;
    e=cam_phy_settings(efuse,plan.cphy!=0);
    for(unsigned b=0;b<2&&!e;b++)e=cam_phy_power_calibrate(b);
    if(!e) {cam_phy.status.flags|=X9_CAM_PHY_CALIBRATED;cam_phy.status.stage=5;}
    else cam_phy_fault(e);
    for(unsigned b=0;b<2;b++)cam_phy.status.ref_ck[b]=
        (cam_phy.reg[cam_phy_find(b,0xf0)].expected>>3)&1; /* no MMIO on error */
    cam_phy_status();*s=cam_phy.status;return e;
}
/* Before OUTMUX retirement too: do not change a posted command's input
 * environment under an unconsumed analog calibration. No retry/poll here. */
static int cam_phy_retirement_guard(uint64_t epoch)
{
    if(!cam_phy.held)return 0;
    if(epoch!=cam_phy.epoch)return ESTALE;
    if(cam_phy.calibration_pending)return EBUSY;
    return cam_phy_guard();
}
static int cam_phy_close_locked(uint64_t epoch)
{
    int e;
    if(!cam_phy.held)return 0;
    if(epoch!=cam_phy.epoch || cam_phy.close_attempted)return ESTALE;
    cam_phy.close_attempted=true;
    e=cam_phy_retirement_guard(epoch);
    if(!e && cam_phy.pending_reg!=CAM_PHY_NO_REG) {
        struct cam_phy_reg *r=&cam_phy.reg[cam_phy.pending_reg];uint32_t v=cam_phy_read(cam_phy.pending_reg);
        if(v!=r->expected && v!=cam_phy.pending_next)e=ESTALE;
        else {r->expected=v;cam_phy.pending_reg=CAM_PHY_NO_REG;}
    }
    if(!e)e=cam_phy_dependencies();
    /* Reverse bank power, preserving source/async clocks until config is
     * restored. Neither digital lanes nor MAC/OUTMUX DL were enabled. */
    for(unsigned i=2;i&&!e;i--) {
        unsigned b=i-1;
        e=cam_phy_write(b,0x20,CAM_PHY_CAL_MASK,0);
        if(!e)e=cam_phy_write(b,0,0x800,0);
        if(!e)e=cam_phy_write(b,0,2,0);
        if(!e)e=cam_phy_write(b,0,1,0);
        if(!e)e=cam_phy_pause(200);
    }
    for(unsigned i=cam_phy.count;i&&!e;i--) {
        struct cam_phy_reg *r=&cam_phy.reg[i-1];
        if(r->owned)e=cam_phy_write(r->bank,r->offset,r->owned,r->before);
        if(!e)r->owned=0;
    }
    if(!e)e=cam_phy_dependencies();
    if(!e) {cam_phy.held=false;cam_phy.epoch=0;cam_phy.status.flags|=X9_CAM_PHY_RESTORED;}
    else cam_phy_fault(e);
    cam_phy.status.restore_errno=e;cam_phy_status();
    printf("X9 CAMERA PHY CLOSE: sensor=%u epoch=%llu flags=%x errno=%u restore=%d dirty=%u "
        "pending=%u cal=%x/%x (analog config only; NOT IRQ/DMA drain)\n",
        cam_phy.status.sensor,epoch,cam_phy.status.flags,cam_phy.status.last_errno,e,
        cam_phy.status.dirty_count,cam_phy.pending_reg,cam_phy.calibration_pending,cam_phy.calibration_done);
    return e;
}
int x9_camera_sensor_session_prepare_analog(uint64_t epoch,struct x9_camera_phy_status *s)
{
    struct cam_sensor_ap ap;
    uint8_t byte=0;
    int e;
    if(!s)return EINVAL;
    lck_mtx_lock(&camera_control_lock);memset(s,0,sizeof(*s));s->sensor_epoch=epoch;
    e=cam_sensor_session_match(epoch);
    if(!e && cam_sensor_live.error)e=cam_sensor_live.error;
    if(!e && (cam_sensor_live.phase!=CAM_SENSOR_CONFIGURED || session.capture || session.stream))e=EBUSY;
    if(!e && cam_phy.held)e=EALREADY;
    if(!e && cam_out.held)e=EBUSY;
    if(!e && (!cam_rx_sensor_held_locked(epoch)||!cam_mac.held))e=ENXIO;
    if(!e) {
        s->sensor=cam_sensor_live.probe.sensor;
        ap=(struct cam_sensor_ap){&cam_journal.sensor,s->sensor==X9_CAMERA_BACK?0x1a:0x10};
        e=cam_sensor_ap_read(&ap,0x0100,&byte);if(!e && byte)e=EBUSY;
        if(!e)e=cam_sensor_ap_read(&ap,0x0104,&byte);if(!e && byte)e=EBUSY;
        if(e)cam_sensor_session_fault(e);
        if(!e)e=cam_phy_open_locked(epoch,s);
        if(!e)e=cam_sensor_ap_read(&ap,0x0100,&byte);if(!e && byte)e=EBUSY;
        if(!e)e=cam_sensor_ap_read(&ap,0x0104,&byte);if(!e && byte)e=EBUSY;
        if(!e)e=cam_sensor_ap_guard(&ap);
        if(e && cam_phy.held) {cam_phy_fault(e);cam_rx_session_fault(e);cam_sensor_session_fault(e);}
        else if(e && cam_rx_live.error)cam_sensor_session_fault(e);
        if(cam_phy.held)*s=cam_phy.status;
    }
    s->last_errno=e;lck_mtx_unlock(&camera_control_lock);return e;
}
