/* ISP8s PHY3.0 inactive MAC/async configuration. Source: MT6993 DT and
 * csirx_mac_csi_setting / csirx_mac_top_setting / set_async, not HakiOS's
 * Qualcomm register map. A CSI port has distinct MAC-top and MAC-A banks:
 * port0 13884000/13885000, port3 1389c000/1389d000. The 7dc AFIFO offset is
 * relative to CAM_MAIN, not the already-offset vendor AFIFO mapping.
 * All entries use camera_control_lock. No PHY/lane enable, counters reset,
 * checker/status ack, BER start, global async CG/IRQ/reset or DMA writes.
 */
#include "x9_camera_mac.h"
enum { CAM_MAC_TOP, CAM_MAC_A, CAM_MAC_B, CAM_MAC_ASYNC, CAM_MAC_CAM,
    CAM_MAC_SENINF, CAM_MAC_BANKS, CAM_MAC_MAX_REGS=192, CAM_MAC_WRITES=9 };
struct cam_mac_reg { unsigned bank, offset; uint32_t mask, before, expected; };
struct cam_mac_field { unsigned reg; uint32_t mask, target; };
static struct {
    vm_offset_t bank[CAM_MAC_BANKS];
    struct cam_mac_reg reg[CAM_MAC_MAX_REGS];
    struct cam_mac_field field[CAM_MAC_WRITES];
    unsigned count, pending_slot, cg_reg;
    uint32_t pending_next, dirty;
    uint64_t epoch;
    bool held, close_attempted, cg_owned, cg_uncertain;
    int error;
    struct x9_camera_mac_status status;
} cam_mac;
static void cam_mac_status(void)
{
    cam_mac.status.last_errno=cam_mac.error;cam_mac.status.dirty=cam_mac.dirty;
    cam_mac.status.uncertain=cam_mac.pending_slot<CAM_MAC_WRITES?1u<<cam_mac.pending_slot:0;
    cam_mac.status.cg_owned=cam_mac.cg_owned;cam_mac.status.cg_uncertain=cam_mac.cg_uncertain;
    if(cam_mac.error && cam_mac.held)cam_mac.status.flags|=X9_CAM_MAC_RETAINED;
    else cam_mac.status.flags&=~X9_CAM_MAC_RETAINED;
}
static int cam_mac_fault(int e)
{
    if(!cam_mac.error)cam_mac.error=e;
    cam_mac_status();return e;
}
static unsigned cam_mac_add(unsigned bank,unsigned offset,uint32_t mask)
{
    unsigned n;
    if(cam_mac.count>=CAM_MAC_MAX_REGS)return CAM_MAC_MAX_REGS;
    n=cam_mac.count++;
    cam_mac.reg[n]=(struct cam_mac_reg){bank,offset,mask,0,0};return n;
}
static uint32_t cam_mac_read(unsigned n)
{
    const struct cam_mac_reg *r=&cam_mac.reg[n];
    return cam_rr(cam_mac.bank[r->bank],r->offset)&r->mask;
}
/* Publication affects only receiver-sampled fields after an EXACT confirmed
 * write. The initial receiver snapshot remains immutable evidence. */
static void cam_mac_publish(void)
{
    static const unsigned top_offsets[4]={0,4,0x40,0x70};
    for(unsigned n=0;n<cam_mac.count;n++) {
        const struct cam_mac_reg *r=&cam_mac.reg[n];
        if(r->bank==CAM_MAC_TOP)for(unsigned k=0;k<4;k++)if(r->offset==top_offsets[k])
            cam_rx_live.expected.mac[k]=(cam_rx_live.expected.mac[k]&~r->mask)|r->expected;
        if(r->bank==CAM_MAC_ASYNC && !r->offset)
            cam_rx_live.expected.async_cfg=(cam_rx_live.expected.async_cfg&~r->mask)|r->expected;
        if(r->bank==CAM_MAC_ASYNC && r->offset==0xc)
            cam_rx_live.expected.async_split=(cam_rx_live.expected.async_split&~r->mask)|r->expected;
        if(r->bank==CAM_MAC_CAM && r->offset==0x7dc)
            cam_rx_live.expected.afifo=(cam_rx_live.expected.afifo&~r->mask)|r->expected;
    }
}
/* No OUTMUX/receiver recursive calls here. Every stored entry is a named
 * control CSR; no packet/fault/read-clear IRQ/debug-result registers. */
static int cam_mac_guard(void)
{
    int e;
    if(!cam_mac.held)return 0;
    if(cam_mac.epoch!=cam_rx_live.sensor_epoch || cam_rx_live.epoch!=cam_mm_live.epoch)return ESTALE;
    e=cam_mm_guard(&cam_mm,true);if(e)return e;
    for(unsigned n=0;n<cam_mac.count;n++) {
        uint32_t v=cam_mac_read(n);
        bool allowed=v==cam_mac.reg[n].expected;
        if(cam_mac.pending_slot<CAM_MAC_WRITES && n==cam_mac.field[cam_mac.pending_slot].reg &&
           v==cam_mac.pending_next)allowed=true;
        if(n==cam_mac.cg_reg && cam_mac.cg_uncertain &&
           (v==cam_mac.reg[n].before || v==(1u<<cam_mac.status.csi_port)))allowed=true;
        e=cam_source_observe(cam_mac.bank[cam_mac.reg[n].bank],cam_mac.reg[n].offset,v,&allowed);
        if(e)return e;
        if(!allowed)return ESTALE;
    }
    return 0;
}
static int cam_mac_dependencies(void)
{
    int e=cam_mac_guard();if(e)return e;
    e=cam_rx_session_guard();if(e)return e;
    e=cam_phy_guard();if(e)return e;
    return cam_out_guard(); /* its MAC guard does NOT recurse into OUTMUX */
}
static int cam_mac_write(unsigned slot,uint32_t value)
{
    const struct cam_mac_field *f=&cam_mac.field[slot];
    struct cam_mac_reg *r=&cam_mac.reg[f->reg];
    uint32_t raw,next,seen;
    int e=cam_mac_dependencies();if(e)return e;
    if(cam_mac.pending_slot<CAM_MAC_WRITES)return EBUSY;
    raw=cam_rr(cam_mac.bank[r->bank],r->offset);
    if((raw&r->mask)!=r->expected)return ESTALE;
    next=(raw&~f->mask)|(value&f->mask);
    if((next&r->mask)==r->expected)return 0;
    cam_mac.pending_slot=slot;cam_mac.pending_next=next&r->mask;cam_mac.dirty|=1u<<slot;
    cam_rw(cam_mac.bank[r->bank],r->offset,next);
    seen=cam_rr(cam_mac.bank[r->bank],r->offset);
    if((seen&r->mask)!=cam_mac.pending_next)return EIO;
    /* Shared words preserve unrelated ports at this observed RMW boundary.
     * This is not a cross-firmware atomic lease, nor an ABA detector. */
    if((seen^raw)&~r->mask)return ESTALE;
    r->expected=seen&r->mask;cam_mac.pending_slot=CAM_MAC_WRITES;cam_mac_publish();
    return cam_mac_dependencies();
}
static int cam_mac_open_locked(uint64_t epoch,struct x9_camera_mac_status *s)
{
    struct x9_camera_pipeline plan;
    uint32_t port,bit,split,irq,again[CAM_MAC_MAX_REGS];
    unsigned ctrl,phy,opt,hdr,resync,resync2,dt,async_cfg,async_split,afifo;
    unsigned idx;
    static vm_offset_t csi[2][2],seninf_top;
    int e;
    memset(s,0,sizeof(*s));s->sensor_epoch=epoch;s->sensor=cam_sensor_live.probe.sensor;
    if(cam_mac.held)return EALREADY;
    /* Configuration order prevents a partial MAC admission from modifying
     * the environment of an already-posted OUTMUX command. */
    if(cam_out.held)return EBUSY;
    if(!cam_rx_sensor_held_locked(epoch) || cam_rx_live.close_attempted || cam_rx_live.error)return ESTALE;
    e=cam_pipeline_plan(s->sensor,&plan);if(e)return e;
    if(plan.csi_port!=(s->sensor==X9_CAMERA_BACK?3u:0u) ||
       plan.cphy!=(s->sensor==X9_CAMERA_BACK?1u:0u) ||
       plan.lanes!=(s->sensor==X9_CAMERA_BACK?3u:4u) || plan.dt!=0x2b)return ENOTSUP;
    e=cam_rx_session_guard();if(e)return cam_rx_session_fault(e);
    port=plan.csi_port;bit=1u<<port;split=3u<<(2*port);irq=bit|(bit<<8);
    cam_mac.bank[CAM_MAC_TOP]=cam_rx_maps[s->sensor==X9_CAMERA_BACK?0:1].mac;
    /* Preserve cached mappings across sessions; physical port changes require
     * selecting the corresponding cached map, not retargeting a live one. */
    idx=s->sensor==X9_CAMERA_BACK?0:1;
    if(!csi[idx][0])csi[idx][0]=ml_io_map(0x13885000ull+port*0x8000ull,0x1000);
    if(!csi[idx][1])csi[idx][1]=ml_io_map(0x13886000ull+port*0x8000ull,0x1000);
    if(!seninf_top)seninf_top=ml_io_map(0x3a400000,0x1000);
    cam_mac.bank[CAM_MAC_A]=csi[idx][0];cam_mac.bank[CAM_MAC_B]=csi[idx][1];
    cam_mac.bank[CAM_MAC_ASYNC]=cam_rx_maps[idx].async;
    cam_mac.bank[CAM_MAC_CAM]=cam_mm.cam_cg;cam_mac.bank[CAM_MAC_SENINF]=seninf_top;
    for(unsigned k=0;k<CAM_MAC_BANKS;k++)if(!cam_mac.bank[k])return ENOMEM;
    cam_mac.count=0;
    ctrl=cam_mac_add(CAM_MAC_TOP,0,UINT32_MAX);
    phy=cam_mac_add(CAM_MAC_TOP,0x40,UINT32_MAX);
    cam_mac_add(CAM_MAC_TOP,0x70,UINT32_MAX);
    cam_mac_add(CAM_MAC_TOP,0x90,UINT32_MAX);
    cam_mac_add(CAM_MAC_TOP,0x94,UINT32_MAX);
    cam_mac_add(CAM_MAC_TOP,0xf0,UINT32_MAX);
    cam_mac_add(CAM_MAC_A,0,UINT32_MAX); /* lane enables remain zero */
    cam_mac_add(CAM_MAC_B,0,UINT32_MAX); /* no inherited split sibling */
    opt=cam_mac_add(CAM_MAC_A,4,UINT32_MAX);
    hdr=cam_mac_add(CAM_MAC_A,8,UINT32_MAX);
    cam_mac_add(CAM_MAC_A,0xc,UINT32_MAX);
    resync=cam_mac_add(CAM_MAC_A,0x10,UINT32_MAX);
    resync2=cam_mac_add(CAM_MAC_A,0x14,UINT32_MAX);
    cam_mac_add(CAM_MAC_A,0x18,UINT32_MAX);
    for(unsigned o=0x20;o<=0x44;o+=4)cam_mac_add(CAM_MAC_A,o,UINT32_MAX);
    cam_mac_add(CAM_MAC_A,0x5c,UINT32_MAX);
    cam_mac_add(CAM_MAC_A,0xb8,UINT32_MAX);cam_mac_add(CAM_MAC_A,0xc0,UINT32_MAX);
    cam_mac_add(CAM_MAC_A,0xe0,UINT32_MAX);
    cam_mac_add(CAM_MAC_A,0xf8,UINT32_MAX);cam_mac_add(CAM_MAC_A,0xfc,UINT32_MAX);
    cam_mac_add(CAM_MAC_A,0x100,UINT32_MAX);
    dt=cam_mac_add(CAM_MAC_A,0x104,UINT32_MAX);
    /* DT/VC transform words are control, not observed packet data. */
    for(unsigned o=0x108;o<=0x1a8;o+=4)cam_mac_add(CAM_MAC_A,o,UINT32_MAX);
    for(unsigned o=0x220;o<=0x2a8;o+=4)cam_mac_add(CAM_MAC_A,o,UINT32_MAX);
    for(unsigned tag=0;tag<6;tag++) {
        cam_mac_add(CAM_MAC_A,0x300+tag*0x10,0x3fffffff); /* exclude RO error bits */
        cam_mac_add(CAM_MAC_A,0x304+tag*0x10,UINT32_MAX);
        cam_mac_add(CAM_MAC_A,0x440+tag*0x10,0x3fffffff);
        cam_mac_add(CAM_MAC_A,0x444+tag*0x10,UINT32_MAX);
        cam_mac_add(CAM_MAC_A,0x408+tag*4,UINT32_MAX); /* IRQ EN, NOT +420 status */
        cam_mac_add(CAM_MAC_A,0x4a0+tag*4,UINT32_MAX);
    }
    cam_mac_add(CAM_MAC_A,0x400,UINT32_MAX);
    async_cfg=cam_mac_add(CAM_MAC_ASYNC,0,bit|(bit<<24));
    async_split=cam_mac_add(CAM_MAC_ASYNC,0xc,split);
    cam_mac_add(CAM_MAC_ASYNC,0x30+port*0x20,3); /* PRBS config only */
    cam_mac_add(CAM_MAC_ASYNC,0x34+port*0x20,UINT32_MAX);
    afifo=cam_mac_add(CAM_MAC_CAM,0x7dc,split);
    cam_mac_add(CAM_MAC_CAM,0x7e4,bit|(bit<<8));
    cam_mac_add(CAM_MAC_CAM,0x7ec,bit|(bit<<8));
    cam_mac_add(CAM_MAC_SENINF,0,bit);
    cam_mac.cg_reg=cam_mac_add(CAM_MAC_SENINF,8,bit);
    cam_mac_add(CAM_MAC_SENINF,0x20,irq);
    if(cam_mac.cg_reg>=CAM_MAC_MAX_REGS || cam_mac.count>=CAM_MAC_MAX_REGS)return EOVERFLOW;
    e=cam_mm_guard(&cam_mm,true);if(e)return e;
    for(unsigned n=0;n<cam_mac.count;n++)cam_mac.reg[n].before=cam_mac_read(n);
    delay(10);
    e=cam_mm_guard(&cam_mm,true);if(e)return e;
    for(unsigned n=0;n<cam_mac.count;n++)again[n]=cam_mac_read(n);
    for(unsigned n=0;n<cam_mac.count;n++) {
        struct cam_mac_reg *r=&cam_mac.reg[n];
        if(again[n]!=r->before)return ESTALE;
        if(r->bank==CAM_MAC_TOP &&
           ((r->offset==0 && (r->before&1)) || (r->offset==0x40 && (r->before&0xf003)) ||
            (r->offset==0x70 && (r->before&5)) || (r->offset==0x90 && (r->before&0x101)) ||
            (r->offset==0xf0 && (r->before&1))))return EBUSY;
        if((r->bank==CAM_MAC_A || r->bank==CAM_MAC_B) && !r->offset && r->before)return EBUSY;
        if(r->bank==CAM_MAC_A) {
            if((r->offset==0xb8 || r->offset==0x400 || (r->offset>=0x408 && r->offset<=0x41c)) && r->before)return EBUSY;
            if(r->offset==0xc0 && (r->before&0x7fffffff))return EBUSY;
            if((r->offset>=0x300 && r->offset<=0x350 && !(r->offset&0xf)) ||
               (r->offset>=0x440 && r->offset<=0x490 && !(r->offset&0xf)))
                if(r->before&0x11)return EBUSY;
            if(r->offset==4 && (r->before&0x40))return ENOTSUP; /* descrambling */
            if(r->offset==0x18 && (r->before&0x20000))return ENOTSUP; /* RAW10 DPCM */
            if(r->offset==0x10c && (r->before&0x30000))return ENOTSUP; /* forced packet type */
            if((r->offset==0x110 || r->offset==0x114) && (r->before&0x10001000))return ENOTSUP;
            if(r->offset==0x118 && (r->before&0xc0c08080))return ENOTSUP;
            if(r->offset==0x11c && (r->before&1))return ENOTSUP;
            if(r->offset==0x10 && (r->before&0xc00))return ENOTSUP; /* inherited bypass/LRTE */
            if(r->offset==0x14 && (r->before&0x8000))return EBUSY; /* reset request */
            if(r->offset==0x44 && (r->before&0x100))return EBUSY; /* BER clear request */
        }
        if(r->bank==CAM_MAC_ASYNC &&
           ((!r->offset && (r->before&bit)) ||
            (r->offset==0x30+port*0x20 && (r->before&1)) ||
            (r->offset==0x34+port*0x20 && (r->before&0x13))))return EBUSY;
        if(r->bank==CAM_MAC_CAM && (r->offset==0x7e4 || r->offset==0x7ec) && (r->before&bit))return EBUSY;
        if(r->bank==CAM_MAC_SENINF && r->before)return EBUSY; /* no borrowed CG, reset or IRQ */
        r->expected=r->before;
    }
    /* Only documented fixed packet fields; CPHY3 header length index3=4,
     * NOT lane-count 3. MAC-top mux enables still off for both PHY types. */
    cam_mac.field[0]=(struct cam_mac_field){ctrl,1u<<16,0};
    cam_mac.field[1]=(struct cam_mac_field){phy,3u<<8,0};
    cam_mac.field[2]=(struct cam_mac_field){opt,0x1107bf,0x1004b6|(plan.cphy?1u:0u)};
    cam_mac.field[3]=(struct cam_mac_field){hdr,0x7ff,plan.cphy?0x402u:0};
    cam_mac.field[4]=(struct cam_mac_field){resync,0xfffff11f,0x11f};
    cam_mac.field[5]=(struct cam_mac_field){resync2,0x10001fff,0};
    cam_mac.field[6]=(struct cam_mac_field){dt,0x3f,0x2b};
    cam_mac.field[7]=(struct cam_mac_field){async_split,split,0};
    cam_mac.field[8]=(struct cam_mac_field){afifo,split,0};
    /* Debug remains as inherited; it is not needed to route real packets.
     * Test-model source was required off and is never enabled. */
    (void)async_cfg;
    cam_mac.status=*s;cam_mac.status.csi_port=port;cam_mac.status.cphy=plan.cphy;
    cam_mac.status.packet_option=cam_mac.field[2].target;cam_mac.status.header_mode=cam_mac.field[3].target;
    cam_mac.epoch=epoch;cam_mac.held=true;cam_mac.close_attempted=false;
    cam_mac.cg_owned=cam_mac.cg_uncertain=false;cam_mac.pending_slot=CAM_MAC_WRITES;
    cam_mac.dirty=0;cam_mac.error=0;cam_mac.status.flags=X9_CAM_MAC_ATTEMPTED;
    e=cam_mac_dependencies();
    if(!e) {
        uint32_t cg=cam_rr(seninf_top,8),v;
        if(cg&bit)e=EBUSY;
        else {
            cam_mac.cg_owned=cam_mac.cg_uncertain=true; /* BEFORE shared RMW */
            cam_rw(seninf_top,8,cg|bit);
            v=cam_rr(seninf_top,8);
            if(!(v&bit))e=EIO;
            else if((v^cg)&~bit)e=ESTALE;
            else {cam_mac.reg[cam_mac.cg_reg].expected=bit;cam_mac.cg_uncertain=false;e=cam_mac_dependencies();}
        }
    }
    for(unsigned i=0;i<CAM_MAC_WRITES && !e;i++)e=cam_mac_write(i,cam_mac.field[i].target);
    if(!e)cam_mac.status.flags|=X9_CAM_MAC_CONFIGURED;
    else cam_mac_fault(e);
    cam_mac_status();*s=cam_mac.status;return e;
}
static int cam_mac_close_locked(uint64_t epoch)
{
    int e;
    uint32_t bit=1u<<cam_mac.status.csi_port;
    if(!cam_mac.held)return 0;
    if(epoch!=cam_mac.epoch || cam_mac.close_attempted)return ESTALE;
    cam_mac.close_attempted=true;
    e=cam_mac_guard();
    if(!e && cam_mac.pending_slot<CAM_MAC_WRITES) {
        struct cam_mac_reg *r=&cam_mac.reg[cam_mac.field[cam_mac.pending_slot].reg];
        uint32_t v=cam_mac_read(cam_mac.field[cam_mac.pending_slot].reg);
        /* Ambiguous writes accepted only exact old/new; observation retires
         * uncertainty, never repeats the failed configuration write. */
        if(v!=r->expected && v!=cam_mac.pending_next)e=ESTALE;
        else {r->expected=v;cam_mac.pending_slot=CAM_MAC_WRITES;cam_mac_publish();}
    }
    if(!e && cam_mac.cg_uncertain) {
        uint32_t v=cam_mac_read(cam_mac.cg_reg);
        cam_mac.reg[cam_mac.cg_reg].expected=v;
        cam_mac.cg_owned=!!v;cam_mac.cg_uncertain=false;
    }
    if(!e)e=cam_mac_dependencies();
    for(unsigned i=CAM_MAC_WRITES;i && !e;i--) {
        unsigned slot=i-1;
        if(cam_mac.dirty&(1u<<slot))e=cam_mac_write(slot,cam_mac.reg[cam_mac.field[slot].reg].before);
    }
    if(!e)e=cam_mac_dependencies();
    if(!e && cam_mac.cg_owned) {
        uint32_t cg=cam_rr(cam_mac.bank[CAM_MAC_SENINF],8),v;
        if(!(cg&bit))e=ESTALE;
        else {
            cam_mac.cg_uncertain=true; /* one attempt; ambiguous CLR retains */
            cam_rw(cam_mac.bank[CAM_MAC_SENINF],8,cg&~bit);
            v=cam_rr(cam_mac.bank[CAM_MAC_SENINF],8);
            if(v&bit)e=EIO;
            else if((v^cg)&~bit)e=ESTALE;
            else {cam_mac.reg[cam_mac.cg_reg].expected=0;cam_mac.cg_owned=cam_mac.cg_uncertain=false;}
        }
    }
    if(!e)e=cam_mac_dependencies();
    if(!e) {
        cam_mac.held=false;cam_mac.epoch=0;cam_mac.dirty=0;
        cam_mac.status.flags|=X9_CAM_MAC_RESTORED;
    } else cam_mac_fault(e);
    cam_mac.status.restore_errno=e;cam_mac_status();
    printf("X9 CAMERA MAC CLOSE: sensor=%u epoch=%llu flags=%x errno=%u restore=%d dirty=%x "
        "uncertain=%x CG=%u/%u (packet configuration, NOT PHY/DMA drain)\n",
        cam_mac.status.sensor,epoch,cam_mac.status.flags,cam_mac.status.last_errno,e,
        cam_mac.status.dirty,cam_mac.status.uncertain,cam_mac.status.cg_owned,cam_mac.status.cg_uncertain);
    return e;
}
int x9_camera_sensor_session_configure_mac(uint64_t epoch,struct x9_camera_mac_status *s)
{
    struct cam_sensor_ap ap;
    uint8_t byte=0;
    int e;
    if(!s)return EINVAL;
    lck_mtx_lock(&camera_control_lock);
    memset(s,0,sizeof(*s));s->sensor_epoch=epoch;
    e=cam_sensor_session_match(epoch);
    if(!e && cam_sensor_live.error)e=cam_sensor_live.error;
    if(!e && (cam_sensor_live.phase!=CAM_SENSOR_CONFIGURED || session.capture || session.stream))e=EBUSY;
    if(!e && cam_mac.held)e=EALREADY;
    if(!e && cam_out.held)e=EBUSY;
    if(!e && !cam_rx_sensor_held_locked(epoch))e=ENXIO;
    if(!e) {
        ap=(struct cam_sensor_ap){&cam_journal.sensor,cam_sensor_live.probe.sensor==X9_CAMERA_BACK?0x1a:0x10};
        e=cam_sensor_ap_read(&ap,0x0100,&byte);if(!e && byte)e=EBUSY;
        if(!e)e=cam_sensor_ap_read(&ap,0x0104,&byte);if(!e && byte)e=EBUSY;
        if(e)cam_sensor_session_fault(e);
        if(!e)e=cam_mac_open_locked(epoch,s);
        if(!e)e=cam_sensor_ap_read(&ap,0x0100,&byte);if(!e && byte)e=EBUSY;
        if(!e)e=cam_sensor_ap_read(&ap,0x0104,&byte);if(!e && byte)e=EBUSY;
        if(!e)e=cam_sensor_ap_guard(&ap);
        if(e && cam_mac.held){cam_mac_fault(e);cam_rx_session_fault(e);cam_sensor_session_fault(e);}
        else if(e && cam_rx_live.error)cam_sensor_session_fault(e);
        if(cam_mac.held)*s=cam_mac.status;
    }
    s->last_errno=e;lck_mtx_unlock(&camera_control_lock);return e;
}
