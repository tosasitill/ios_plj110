/* MT6991-compatible speed_ver=1 AP FIFO timing admission.
 * clk-mt6993.c I2C parents: 26M, UNIVPLL/20, MAINPLL/16, UNIVPLL/10.
 * i2c-mt65xx.c::mtk_i2c_set_speed_v2 uses separate sample*step high/low,
 * duplicated 5-bit dividers, LS EXT and one extra LS period count.
 * Admit only our bounded standard-mode profile, never retime a live bus.
 * These are conservative CONFIGURATION bounds, not measured SCL timing,
 * an electrical rise-time/stretch guarantee or an AP/SCP hardware lease.
 */
static int cam_bus_source_hz(const struct cam_probe_bus *b, uint64_t *hz)
{
    *hz=26000000;
    if(b->clock_source) {
        unsigned fixed=b->clock_source==1?20:b->clock_source==2?16:10;
        uint64_t divisor=(uint64_t)16384*(1u<<((b->clock_con1>>24)&7))*fixed;
        /* Round UP once across both dividers; the old second truncation
         * could understate the source used for the period/EXT constraints. */
        *hz=((uint64_t)(b->clock_con1&0x3fffff)*26000000+divisor-1)/divisor;
    }
    return *hz>=1000000 && *hz<=500000000?0:ENOTSUP;
}
static int cam_bus_timing_profile(uint64_t hz, uint32_t timing, uint32_t low,
    uint32_t ext, uint32_t divreg, uint32_t hs, uint32_t sda,
    uint32_t io, uint32_t mis, uint32_t *rate)
{
    *rate=0;
    if((timing&~0x073fu) || (low&~0x01ffu) || (ext&~0xff01u) ||
       (divreg&~0x1f1fu) || hs || sda || io!=3 || mis || !(ext&1))return ENOTSUP;
    unsigned div=(divreg&31)+1;
    if(((divreg>>8)&31)+1!=div)return ENOTSUP;
    unsigned high=(((timing>>8)&7)+1)*((timing&63)+1);
    unsigned l=(((low>>6)&7)+1)*((low&63)+1);
    unsigned total=high+l;
    if(high<2 || l<2)return ENOTSUP;
    /* Charge one count against the full period, matching the upstream
     * LS solver's +1 slack. Charge one against EACH phase for conservative
     * tHIGH>=4us / tLOW>=4.7us. Do not round a >100kHz bound down to 100k. */
    uint64_t period=(uint64_t)div*(total-1);
    if(hz>period*100000 || hz-1<(uint64_t)div*total*20000 ||
       (uint64_t)div*(high-1)*1000000<hz*4 ||
       (uint64_t)div*(l-1)*10000000<hz*47 ||
       (uint64_t)div*(ext>>8)*1000000<hz*5)return ENOTSUP;
    *rate=(uint32_t)((hz+period-1)/period);
    return 0;
}
static int cam_bus_timing_check(struct cam_probe_bus *b, bool initialized)
{
    uint64_t hz=0;
    int e=cam_bus_controls(b,false);
    if(!e)e=cam_bus_source_hz(b,&hz);
    uint32_t rate=0;
    /* expected[] has been freshly checked against every saved CSR. No
     * second journal, no resampling after withdrawing clocks/supplies. */
    if(!e)e=cam_bus_timing_profile(hz,b->expected[6],b->expected[8],
        b->expected[7],b->expected[11],b->expected[9],b->expected[10],
        b->expected[12],b->expected[13],&rate);
    if(!e)b->timing_hz=rate;
    printf("X9 CAMERA AP TIMING: gate=%x initialized=%u source_hz_bound=%llu "
        "timing=%x low=%x ext=%x div=%x hz_bound=%u errno=%d "
        "(standard profile, NOT measured SCL or lease)\n",
        b->gate_bit,initialized,hz,b->expected[6],b->expected[8],
        b->expected[7],b->expected[11],rate,e);
    return e;
}
/* High/low products are sample(1..8)*step(1..64). Prefer 45% duty,
 * [41%,49%), <=98kHz. Check the actual encoded profile BEFORE any write. */
static bool cam_timing_factor(unsigned n, unsigned *sample, unsigned *step)
{
    for(unsigned s=1;s<=8;s++)if(n%s==0 && n/s>=1 && n/s<=64) {
        *sample=s;*step=n/s;return true;
    }
    return false;
}
static int cam_bus_init_timing(struct cam_probe_bus *b)
{
    uint64_t hz=0;
    int e=cam_bus_source_hz(b,&hz);if(e)return e;
    unsigned div,hs=0,ht=0,ls=0,lt=0,ext=0,total=0;
    uint32_t timing=0,low=0,divreg=0,rate=0;
    bool found=false;
    for(div=1;div<=32 && !found;div++) {
        /* Ceil the full expression: EXT must not be shorter than 5us. */
        ext=(unsigned)((hz*5+(uint64_t)div*1000000-1)/((uint64_t)div*1000000));
        if(ext>255)continue;
        total=(unsigned)((hz+(uint64_t)div*98000-1)/((uint64_t)div*98000))+1;
        if(total<4 || total>1024)continue;
        unsigned ideal=total*45/100;
        for(unsigned delta=0;delta<=total/2 && !found;delta++) {
            for(unsigned side=0;side<2 && !found;side++) {
                if(side && delta>ideal)continue;
                unsigned high=side?ideal-delta:ideal+delta;
                if(high<2 || high>=total-1 || high*1000<total*410 || high*1000>=total*490)continue;
                if(!cam_timing_factor(high,&hs,&ht) || !cam_timing_factor(total-high,&ls,&lt))continue;
                timing=((hs-1)<<8)|(ht-1);low=((ls-1)<<6)|(lt-1);
                divreg=((div-1)<<8)|(div-1);
                e=cam_bus_timing_profile(hz,timing,low,(ext<<8)|1,divreg,0,0,3,0,&rate);
                found=!e && rate<=98000;
            }
        }
    }
    if(!found)return ENOTSUP;
    e=cam_bus_controls(b,false);if(!e)e=cam_bus_idle(b);if(e)return e;
    if(b->expected[6])return EBUSY;
    /* Journal was captured/owned before writes. Do not softreset, change
     * MCU interrupt routing, APDMA or shared shadow mode to get admission. */
    const unsigned regs[]={CAM_I2C_TIMING,CAM_I2C_EXT,CAM_I2C_LTIMING,
        CAM_I2C_HS,CAM_I2C_SDA,CAM_I2C_DIV,CAM_I2C_IO,CAM_I2C_MIS};
    const uint32_t values[]={timing,(ext<<8)|1,low,0,0,divreg,3,0};
    for(unsigned i=0;i<sizeof(regs)/sizeof(regs[0]);i++) {
        e=cam_bus_control_write(b,regs[i],values[i],false);if(e)return e;
    }
    return cam_bus_timing_check(b,true);
}
