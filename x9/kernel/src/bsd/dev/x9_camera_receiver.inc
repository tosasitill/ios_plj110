/* MT6993/ISP8s powered register preflight, camera_control_lock held.
 * Captured DT reg-names, not old unit-test aliases:
 * outmux0 outer 3a200000 / inner 3a208000; CAMSV id0 3a600000,
 * DMA +1000 / CQ +2000 / inner +8000. CSI MAC-top is port base+4000.
 * Only documented configuration CSR reads. Do NOT read interrupt/status
 * banks: upstream enables read-clear on several of them. No receiver CSR
 * writes, no shared async CG/IRQ setup (AOV/SCP races), no eFuse/PHY guesses.
 * DISABLED is not ownership, queue drain or a mapping lease.
 */
_Static_assert(sizeof(struct x9_camera_receiver)==1192,"camera receiver ABI");
static struct x9_camera_receiver cam_rx_last={.version=1};
static bool cam_rx_attempted;
static const unsigned cam_rx_out_offsets[X9_CAM_RX_OUT_WORDS]={
    0x000,0x004,0x008,0x00c,0x010,0x014,0x018,0x020,
    0x02c,0x034,0x038,0x100,0x104,0x108
};
static const unsigned cam_rx_sv_offsets[X9_CAM_RX_SV_WORDS]={
    0x040,0x044,0x140,0x148,
    0x540,0x580,0x5c0,0x600,0x640,0x680,0x6c0,0x700,
    0x1200,0x1204,0x1210,0x2100,0x2140
};
struct cam_rx_maps { vm_offset_t mac,async,out[2],sv[2]; };
static int cam_rx_sample(const struct cam_rx_maps *m,struct x9_camera_receiver_sample *s)
{
    /* Re-prove power/clock effects before EACH sample. No probe on a failed
     * MM take, and no unsafe read after a changed parent or lost PM_ACK. */
    if(!cam_mm.active)return ENXIO;
    for(unsigned n=0;n<X9_CAM_MM_SLOTS;n++) {
        int e=cam_mm_cfg_check(&cam_mm,n);if(e)return e;
        e=cam_mm_effect_on(&cam_mm,n);if(e)return e;
    }
    static const unsigned mac_offsets[4]={0,4,0x40,0x70};
    for(unsigned i=0;i<4;i++)s->mac[i]=cam_rr(m->mac,mac_offsets[i]);
    s->async_cfg=cam_rr(m->async,0);s->async_split=cam_rr(m->async,0xc);
    s->afifo=cam_rr(cam_mm.cam_cg,0x7dc);
    for(unsigned bank=0;bank<2;bank++) {
        for(unsigned i=0;i<X9_CAM_RX_OUT_WORDS;i++)
            s->out[bank][i]=cam_rr(m->out[bank],cam_rx_out_offsets[i]);
        for(unsigned i=0;i<X9_CAM_RX_SV_WORDS;i++)
            s->sv[bank][i]=cam_rr(m->sv[bank],cam_rx_sv_offsets[i]);
    }
    return 0;
}
static uint32_t cam_rx_busy(const struct x9_camera_receiver_sample *s,unsigned port)
{
    uint32_t busy=0;
    /* PHY receiver enable or packet-pattern source, not an analog power
     * assertion. Do not attempt to stop/reset an inherited receiver. */
    if((s->mac[2]&0xf003) || (s->mac[3]&5))busy|=1;
    if(s->async_cfg&(1u<<port))busy|=2; /* test-model source */
    if(s->mac[0]&1)busy|=32;
    for(unsigned bank=0;bank<2;bank++) {
        const uint32_t *out=s->out[bank],*sv=s->sv[bank];
        if((out[7]&(1u<<12)) || (out[8]&1) || (out[10]&1))busy|=4;
        if(out[9]&1)busy|=32;
        if(sv[0] || sv[1] || ((sv[2]|sv[3])&1))busy|=8;
        for(unsigned tag=0;tag<8;tag++)
            if(sv[4+tag]&((1u<<15)|(1u<<12)|(1u<<8)|(1u<<7)))busy|=8;
        if((sv[15]&(1u<<16)) || (sv[16]&1))busy|=16;
    }
    return busy;
}
static uint32_t cam_rx_changed(const struct x9_camera_receiver_sample *a,
    const struct x9_camera_receiver_sample *b,unsigned port)
{
    uint32_t changed=0;
    if(memcmp(a->mac,b->mac,sizeof(a->mac)))changed|=1;
    /* Shared async/AFIFO masks: unrelated AOV ports are not our ownership.
     * AFIFO portA and async split fields both use two-bit slots. */
    if(((a->async_cfg^b->async_cfg)&((1u<<port)|(1u<<(24+port)))) ||
       ((a->async_split^b->async_split)&(3u<<(2*port))) ||
       ((a->afifo^b->afifo)&(3u<<(2*port))))changed|=2;
    if(memcmp(a->out,b->out,sizeof(a->out)))changed|=4;
    if(memcmp(a->sv,b->sv,sizeof(a->sv)))changed|=8;
    return changed;
}
static int cam_rx_inspect(void *arg)
{
    struct x9_camera_receiver *r=arg;struct cam_rx_maps m={0};
    unsigned port=r->sensor==X9_CAMERA_BACK?3:0;
    r->stage=X9_CAM_RX_STAGE_MAP;
    r->mac_base=0x13884000ull+port*0x8000ull;
    r->out_base=0x3a200000;r->out_inner_base=0x3a208000;r->sv_base=0x3a600000;
    /* Permanent Device mappings, at most once per boot, just like the MM
     * journal mappings. ml_io_map is NOT a camera DMA allocator. */
    m.mac=ml_io_map(r->mac_base,0x1000);
    m.async=ml_io_map(0x3a401000,0x1000);
    m.out[0]=ml_io_map(r->out_base,0x1000);
    m.out[1]=ml_io_map(r->out_inner_base,0x1000);
    /* Map only three documented banks per view, not the holes in the DT. */
    m.sv[0]=ml_io_map(r->sv_base,0x3000);
    m.sv[1]=ml_io_map(r->sv_base+0x8000,0x3000);
    if(!m.mac || !m.async || !m.out[0] || !m.out[1] || !m.sv[0] || !m.sv[1])return ENOMEM;
    r->stage=X9_CAM_RX_STAGE_SAMPLE;
    int e=cam_rx_sample(&m,&r->before);if(e)return e;
    delay(10);
    e=cam_rx_sample(&m,&r->after);if(e)return e;
    r->flags|=X9_CAM_RX_SNAPSHOT;r->blockers&=~X9_CAM_RX_NEED_SNAPSHOT;
    r->stage=X9_CAM_RX_STAGE_CHECK;
    r->busy=cam_rx_busy(&r->before,port)|cam_rx_busy(&r->after,port);
    r->changed=cam_rx_changed(&r->before,&r->after,port);
    if(r->changed)return ESTALE;
    r->flags|=X9_CAM_RX_STABLE;
    if(r->busy)return EBUSY;
    r->flags|=X9_CAM_RX_DISABLED;
    return 0;
}
static int cam_rx_probe(unsigned sensor,struct x9_camera_receiver *r)
{
    memset(r,0,sizeof(*r));r->version=1;r->sensor=sensor;
    r->blockers=X9_CAM_RX_NEED_OWNER|X9_CAM_RX_NEED_RATE|X9_CAM_RX_NEED_PHY|
        X9_CAM_RX_NEED_ROUTE|X9_CAM_RX_NEED_SMI|X9_CAM_RX_NEED_DMA_MAP|
        X9_CAM_RX_NEED_DRAIN|X9_CAM_RX_NEED_SNAPSHOT;
    if(cam_rx_attempted || cam_mm_attempted || cam_resources_latched || cam_probe_latched) {
        if(cam_rx_attempted)*r=cam_rx_last;
        r->last_errno=cam_resources_latched||cam_probe_latched?EIO:EALREADY;
        if(cam_resources_latched || cam_probe_latched)r->flags|=X9_CAM_RX_LATCHED;
        return r->last_errno;
    }
    cam_rx_attempted=true;r->flags=X9_CAM_RX_ATTEMPTED;r->stage=X9_CAM_RX_STAGE_MM;
    int e=cam_mm_transaction(sensor,&r->mm,cam_rx_inspect,r);
    r->last_errno=e;
    if(r->mm.flags&X9_CAM_MM_RESTORED)r->flags|=X9_CAM_RX_RESTORED;
    if(r->mm.flags&X9_CAM_MM_LATCHED)r->flags|=X9_CAM_RX_LATCHED;
    if(r->mm.restore_errno && (r->flags&X9_CAM_RX_DISABLED))r->stage=X9_CAM_RX_STAGE_RESTORE;
    if(!e)r->stage=X9_CAM_RX_STAGE_DONE;
    cam_rx_last=*r;
    printf("X9 CAMERA RX: sensor=%u flags=%x stage=%u errno=%u restore=%u retained=%x "
        "busy=%x changed=%x blockers=%x MAC=%llx out=%llx/%llx SV=%llx "
        "(read-only controls, NOT ownership or DMA drain)\n",sensor,r->flags,r->stage,
        r->last_errno,r->mm.restore_errno,r->mm.retained,r->busy,r->changed,r->blockers,
        r->mac_base,r->out_base,r->out_inner_base,r->sv_base);
    return e;
}
