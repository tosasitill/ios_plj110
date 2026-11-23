/* ISP8s mtk_cam_sv_ddren_qos_coh_config(), shipped SW-mode default:
 * DDR SET/OR_CQ, COH SET/OR_CQ, BW_QOS SET/OR_CQ, in that order.
 * SET/CLEAR are not ordinary saved configuration fields. Preserve every
 * raw RMW input/output and sample; never restore them by replaying a saved
 * full word. SW_R is recorded, not used as a DMA/power acceptance grant.
 * Normal sv_reset at final supplier close must actually return all three
 * controls to this cold baseline before their power references can drop.
 */
enum { CAM_SV_QOS_COUNT=3 };
#define CAM_SV_QOS_SET (1u<<4)
#define CAM_SV_QOS_CLEAR (1u<<5)
#define CAM_SV_QOS_SW_R (1u<<16)
#define CAM_SV_QOS_OR_CQ (1u<<1)
static const unsigned cam_sv_qos_offsets[CAM_SV_QOS_COUNT]={0x150,0x280,0x158};
static struct {
    uint64_t epoch;
    uint32_t before[2][CAM_SV_QOS_COUNT],actual[2][CAM_SV_QOS_COUNT];
    uint32_t rmw[6],written[6],issued,seen[2],sampled[2];
    bool captured,configured,retired;
    int error;
} cam_sv_qos;
static int cam_sv_environment(void);
static int cam_sv_qos_release_guard(uint64_t epoch)
{
    if(!cam_sv_qos.captured || cam_sv_qos.epoch!=epoch || !cam_sv_qos.issued)return 0;
    return cam_sv_qos.error?cam_sv_qos.error:cam_sv_qos.retired?0:EBUSY;
}
static unsigned cam_sv_qos_index(unsigned offset)
{
    for(unsigned n=0;n<CAM_SV_QOS_COUNT;n++)if(offset==cam_sv_qos_offsets[n])return n;
    return CAM_SV_QOS_COUNT;
}
static int cam_sv_qos_capture(void)
{
    bool inherited=false;
    if(cam_sv_qos.captured && !cam_sv_qos.retired)
        printf("X9 CAMERA CAMSV QOS ADOPT: previous epoch=%llu not retired errno=%d issued=%x; recapturing\n",
            cam_sv_qos.epoch,cam_sv_qos.error,cam_sv_qos.issued);
    memset(&cam_sv_qos,0,sizeof(cam_sv_qos));cam_sv_qos.epoch=cam_sv.epoch;
    for(unsigned b=0;b<2;b++)for(unsigned n=0;n<CAM_SV_QOS_COUNT;n++) {
        unsigned reg=cam_sv_find(cam_sv_qos_offsets[n]);
        if(reg>=cam_sv.count)return CAM_FAIL(EINVAL);
        cam_sv_qos.before[b][n]=cam_sv.before[b][reg];
        cam_sv_qos.actual[b][n]=cam_sv.before[b][reg];
        /* No inherited SW request, timer, mode, OR_CQ or command is
         * silently adopted as ours. Reserved/WLA fields stay untouched. */
        if(cam_sv_qos.before[b][n]&0x1003fu)inherited=true;
    }
    if(inherited)printf("X9 CAMERA CAMSV QOS INHERITED: DDR=%x/%x COH=%x/%x BW=%x/%x (Android RMW sets without check)\n",
        cam_sv_qos.before[0][0],cam_sv_qos.before[1][0],cam_sv_qos.before[0][1],
        cam_sv_qos.before[1][1],cam_sv_qos.before[0][2],cam_sv_qos.before[1][2]);
    cam_sv_qos.captured=true;return 0;
}
static int cam_sv_qos_observe(unsigned bank,unsigned offset,uint32_t value,bool *allowed)
{
    unsigned n=cam_sv_qos_index(offset);
    uint32_t mask,base,bit,or_bit;
    if(n==CAM_SV_QOS_COUNT || !cam_sv_qos.captured)return 0;
    if(bank>=2 || cam_sv_qos.epoch!=cam_sv.epoch || cam_sv_qos.retired)return CAM_FAIL(ESTALE);
    cam_sv_qos.actual[bank][n]=value;cam_sv_qos.sampled[bank]|=1u<<n;
    if(cam_sv_qos.error)return cam_sv_qos.error;
    bit=1u<<(2*n);or_bit=bit<<1;base=cam_sv_qos.before[bank][n];
    mask=(cam_sv_qos.issued&bit)?CAM_SV_QOS_SET|CAM_SV_QOS_SW_R:0;
    if(cam_sv_qos.issued&or_bit)mask|=CAM_SV_QOS_OR_CQ;
    if((value&~mask)!=(base&~mask))return CAM_FAIL(ESTALE);
    if(cam_sv_qos.issued&or_bit) {
        if((cam_sv_qos.seen[bank]&(1u<<n)) && !(value&CAM_SV_QOS_OR_CQ))return CAM_FAIL(ESTALE);
        if(value&CAM_SV_QOS_OR_CQ)cam_sv_qos.seen[bank]|=1u<<n;
    }
    /* Only the command/status bits of our issued SET and the ordinary
     * posted OR_CQ field may differ. CLEAR is never emitted or exempted. */
    *allowed=true;return 0;
}
static int cam_sv_qos_start(void)
{
    int e=0;
    if(!cam_sv_qos.captured || cam_sv_qos.epoch!=cam_sv.epoch ||
       cam_sv_qos.issued || cam_sv_qos.error)return CAM_FAIL(ESTALE);
    for(unsigned step=0;step<6;step++) {
        unsigned n=step/2,offset=cam_sv_qos_offsets[n];
        bool allowed=false;
        uint32_t value;
        e=cam_sv_environment();if(e)break;
        e=cam_sv_power_guard();if(e)break;
        value=cam_rr(cam_sv.view[0],offset);
        e=cam_sv_qos_observe(0,offset,value,&allowed);if(e)break;
        cam_sv_qos.rmw[step]=value;
        /* Match the Android bitfield RMW exactly, including the actual
         * read value of SET/SW_R. The prescribed second RMW is its own
         * journaled store; it is not an automatic replay on timeout. */
        value|=(step&1)?CAM_SV_QOS_OR_CQ:CAM_SV_QOS_SET;
        cam_sv_qos.written[step]=value;cam_sv_qos.issued|=1u<<step;
        cam_rw(cam_sv.view[0],offset,value);
        e=cam_sv_environment();if(e)break;
        if(step&1) {
            uint64_t span,end;
            unsigned samples=0;
            nanoseconds_to_absolutetime(1000000000ull /* log 376: guards cost 3-90 ms per poll */,&span);end=mach_absolute_time()+span;
            for(unsigned poll=0;poll<100000;poll++) {
                if(mach_absolute_time()>=end)break;
                e=cam_sv_environment();if(e)break;
                samples=(cam_sv_qos.actual[0][n]&CAM_SV_QOS_OR_CQ)?samples+1:0;
                if(samples==2)break;
                delay(1);
            }
            if(e)break;
            /* Android writes OR_CQ without readback: observe only. */
            if(samples!=2)printf("X9 CAMERA CAMSV QOS ADOPT: step=%u off=%x OR_CQ not read back actual=%x/%x; continuing\n",
                step,offset,cam_sv_qos.actual[0][n],cam_sv_qos.actual[1][n]);
        }
    }
    if(!e && cam_sv_qos.seen[0]!=7)
        printf("X9 CAMERA CAMSV QOS ADOPT: OR_CQ seen=%x/%x (expected 7); continuing like Android\n",
            cam_sv_qos.seen[0],cam_sv_qos.seen[1]);
    if(!e)cam_sv_qos.configured=true;
    else cam_sv_qos.error=e;
    printf("X9 CAMERA CAMSV QOS: epoch=%llu issued=%x seen=%x/%x sampled=%x/%x "
        "DDR=%x/%x COH=%x/%x BW=%x/%x configured=%u errno=%d "
        "(ISP8s SW-mode writes; SW_R is observation, not DMA grant)\n",
        cam_sv.epoch,cam_sv_qos.issued,cam_sv_qos.seen[0],cam_sv_qos.seen[1],
        cam_sv_qos.sampled[0],cam_sv_qos.sampled[1],
        cam_sv_qos.actual[0][0],cam_sv_qos.actual[1][0],
        cam_sv_qos.actual[0][1],cam_sv_qos.actual[1][1],
        cam_sv_qos.actual[0][2],cam_sv_qos.actual[1][2],cam_sv_qos.configured,e);
    return e;
}
