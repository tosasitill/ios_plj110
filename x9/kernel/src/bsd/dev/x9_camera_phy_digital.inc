/* ISP8s PHY3.0 fixed preview settings, called by configure_timing after
 * actual CSI clock measurement/HS timing, before the final sensor CTLE.
 * common_get_csi_param memcpy: hhfront preview trail64 and hhmain settle57,
 * all ULPS/ALP/LRTE/init-deskew/periodic-deskew support flags0. Compile-time
 * support macros do not change these sensor flags. Neither profile enables
 * PHY lanes, AFIFO FLUSH/CLEAR, IRQs or DMA here. */
#define CAM_DSK_RESET 0x80000000u
#define CAM_DSK_ENABLE 0x100u
#define CAM_DSK_LP11_IDLE 0x40000000u
static int cam_phy_deskew_reset(void)
{
    unsigned n=cam_phy_find(CAM_PHY_D,0x50);
    int e;
    if(n==CAM_PHY_NO_REG || cam_phy.status.deskew_reset_pending ||
       cam_phy.status.deskew_reset_released)return EALREADY;
    if(cam_phy_read(n)&(CAM_DSK_RESET|CAM_DSK_ENABLE|CAM_DSK_LP11_IDLE))return EBUSY;
    e=cam_phy_dependencies();if(e)return e;
    /* Dedicated inactive deskew FSM reset, NOT a way to cancel pending
     * calibration/meter/DMA requests. Mark before assertion. A failed
     * assertion, guard or deassertion leaves this reset pending: no close
     * retry, global reset or power-off is allowed to manufacture retirement.
     * This sequence proves only observed reset control levels, never that
     * RO calibration/IRQ history is cleared or a packet was received. */
    cam_phy.status.deskew_reset_pending=1;
    e=cam_phy_write(CAM_PHY_D,0x50,CAM_DSK_RESET,CAM_DSK_RESET);
    if(!e)e=cam_phy_pause(2);
    if(!e)e=cam_phy_write(CAM_PHY_D,0x50,CAM_DSK_RESET,0);
    if(!e)e=cam_phy_pause(2);
    if(e)return e;
    if(cam_phy_read(n)&CAM_DSK_RESET)return EIO;
    cam_phy.status.deskew_reset_pending=0;cam_phy.status.deskew_reset_released=1;
    return 0;
}
static int cam_phy_digital_settings(void)
{
    bool cphy=cam_phy.status.cphy!=0;
    int e=cam_phy_dependencies();if(e)return e;
    cam_phy.status.stage=10;
    /* SPARE0 is explicitly written31 by both vendor paths. Its base is
     * DPHY_TOP+f0, not A/B analog SETTING0 at the same offset. */
    e=cam_phy_write(CAM_PHY_D,0xf0,UINT32_MAX,0x31);
    if(cphy) {
        /* No invented CPHY trio swap: the upstream setting selects physical
         * LP/valid-pipe trio0..2 directly and never programs CPHY_LANE_SWAP.
         * Those lane/pipe enables belong to the future IRQ-owned start.
         * ALP/LRTE support0: retain qualified off, as upstream explicitly sets. */
        if(!e)e=cam_phy_write(CAM_PHY_C,0x130,2,0);
        if(!e)e=cam_phy_write(CAM_PHY_C,0x208,3,0);
        return e;
    }
    /* Unsplit DPHY4: LC0=2, LC1=0, LD0=1, LD1=3, LD2=0, LD3=4.
     * The LAST vendor setter enables CK_DATA_MUX, overriding no earlier
     * field. No LC/LD or HSRX lane enable accompanies this mapping. */
    if(!e)e=cam_phy_write(CAM_PHY_D,4,0x80777777,0x80403102);
    if(!e)e=cam_phy_write(CAM_PHY_D,0x180,0x58,8); /* sync method1, ALP EN/HSRX_SEL0 */
    if(!e)e=cam_phy_deskew_reset();
    /* The >1500Mbps generic deskew branch runs even though init/periodic
     * sensor supports0. DPHY ANA3.16BIT_SEL is explicitly1 in our generic
     * profile. SENINF_BITS masks vendor0x66666666 to HOLD_CNT=0x66. */
    if(!e)e=cam_phy_write(CAM_PHY_D,0x50,0x40f0ff0f,0x40100d00);
    if(!e)e=cam_phy_write(CAM_PHY_D,0x54,0xff7fffff,0x03026601);
    if(!e)e=cam_phy_write(CAM_PHY_D,0x58,0x40000000,0x40000000); /* APPLY_ONLY_1ST_PAT1 */
    for(unsigned k=0;k<4&&!e;k++)e=cam_phy_write(CAM_PHY_D,0x60+4*k,0x10001,0);
    if(!e)e=cam_phy_write(CAM_PHY_D,0x44,UINT32_MAX,0xff); /* patternff/mask0 */
    /* Vendor enables all deskew IRQs here. We do NOT: unique IRQ/history
     * retirement and receive/DMA ownership have not been connected yet. */
    if(!e)cam_phy.status.lane_select=0x80403102;
    return e;
}
static int cam_phy_digital_stop(void)
{
    unsigned n=cam_phy_find(CAM_PHY_D,0x50);
    struct cam_phy_reg *r;
    uint32_t mask;
    int e;
    if(cam_phy.status.deskew_reset_pending)return EBUSY;
    if(n==CAM_PHY_NO_REG)return EINVAL;
    r=&cam_phy.reg[n];mask=r->owned&(CAM_DSK_ENABLE|CAM_DSK_LP11_IDLE);
    if(!mask)return 0;
    if(r->before&(CAM_DSK_ENABLE|CAM_DSK_LP11_IDLE|CAM_DSK_RESET))return EBUSY;
    /* No PHY lane or DELAY_EN/PERIODIC/APPLY was enabled. Stop only the
     * generic deskew configuration before other digital controls change.
     * Neither a late IRQ poll nor a reset is used as a drain assertion. */
    e=cam_phy_write(CAM_PHY_D,0x50,mask,r->before);
    if(!e)e=cam_phy_pause(2);
    return e;
}
