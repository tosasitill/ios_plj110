/* MT6993 CAMSV A/id0: central 3a600000, DMA +1000, SCQ +2000;
 * inner views +8000. Reference: ISP8s mtk_cam-sv-regs.h and the actual
 * mtk_cam_sv_toggle_db / slave_sv_dev_config register writers. The slave
 * stress source, DMA/CMOS/IRQ enables and reset are NOT ported. The normal
 * libccd sv_pipe_composer writes FBC0=8000 (50640..50af4: central+40,
 * tag*40+500) and later 8080 (5325c..535bc). EN initialization precedes GIC;
 * exactly ONE RCNT descriptor is ported after actual GIC publication. Its
 * complete counter/control acceptance is observed, not guessed from a pulse.
 * Normal source IRQ/tag0 DMA receive enables then follow with actual bank
 * consumption; normal CMOS/VF TG gate follows. PHY/OUTMUX DL/sensor
 * source still remain off, so this is not yet a producing capture path.
 * camera_control_lock held. Named ordinary configuration only: no DONE,
 * SOF, ERR, COMMON, CHANNEL, VF_ST, FIFO_STAT or CQ IRQ_STATUS reads.
 * Tag0 FBC1 is sampled separately, as in the normal upstream FBC readers;
 * No reset, implicit credit, write completion or AXI retirement is inferred.
 */
#include "x9_camera_sv_hw.h"
enum { CAM_SV_MAX_REGS=352, CAM_SV_FIELDS=X9_CAM_SV_HW_FIELDS,
    CAM_SV_CONFIG_FIELDS=X9_CAM_SV_HW_CONFIG_FIELDS,
    CAM_SV_ADDR_LO=CAM_SV_CONFIG_FIELDS, CAM_SV_ADDR_HI, CAM_SV_FH, CAM_SV_FBC,
    CAM_SV_DB_IDLE=0, CAM_SV_DB_DOWN, CAM_SV_DB_UP, CAM_SV_DB_POLICY };
#define CAM_SV_DB_EN (1u<<30)
#define CAM_SV_FBC_BUSY ((1u<<7)|(1u<<8)|(1u<<12)|(1u<<14)|(1u<<15))
#define CAM_SV_FBC_EN (1u<<15)
#define CAM_SV_FBC_RCNT (1u<<7)
/* Named writable FBC0 fields only; reserved bits remain at captured prior.
 * Configuration writes NEVER emit RCNT/RESET. The separate RCNT command
 * engine journals one issued store; it cannot be replayed by DB or restore. */
#define CAM_SV_FBC_MASK 0xffffd180u
#define CAM_SV_FBC_COUNT_MASK 0x000f0000u
/* Normal composer 54530..5454c establishes central+40, then 54c48..54d0c
 * writes DMA_IMG/+304 DONE/+30c ERR/+31c SOF. Restrict its multi-tag masks
 * to our immutable tag0/group0 request. Only named SRAM/tag0 faults, not CQ,
 * foreign tags, COMMON/CHANNEL routing or the vendor's all-tag TOP mask.
 * These are ordinary RW enables, not the adjacent read-clear histories. */
static const struct { unsigned offset; uint32_t target; }
cam_sv_receive_fields[X9_CAM_SV_RECEIVE_FIELDS]={
    {0x344,0x10000}, {0x34c,0x701}, {0x35c,4}, {0x040,1}
};
/* Absolute offsets within each three-bank view. In particular global
 * DMA SLC_CTRL is +102c, not the per-tag +123c. No holes/debug/status sweep. */
static const unsigned cam_sv_fixed[]={
    0x040,0x044,0x060,0x07c,0x140,0x148,0x14c,0x150,0x154,0x158,0x15c,
    0x170,0x174,0x18c,0x1b0,0x1b4,0x1b8,0x1bc,0x1c0,0x1c4,0x1c8,0x1cc,
    0x1d0,0x1d4,0x1d8,0x1dc,0x200,0x204,0x208,0x20c,0x210,0x228,
    0x240,0x244,0x248,0x24c,0x250,0x280,0x284,
    0x344,0x34c,0x35c,0x364,0x36c,
    0x1000,0x1004,0x1008,0x100c,0x1010,0x1014,0x1018,0x101c,
    0x1020,0x1024,0x102c,0x1048,0x10c4,0x10c8,0x10cc,
    0x10e0,0x10e4,0x10e8,0x10ec,0x10f0,0x10f4,0x10f8,0x10fc,
    0x1120,0x1124,0x1128,0x112c,0x1130,0x1134,0x1138,0x113c,
    0x11a0,0x11b0,0x11c0,0x11d0,0x11e0,
    0x2004,0x2008,0x2010,0x2014,0x2018,0x2100,0x2108,0x2120,0x2130,
    0x2140,0x214c,0x2150,0x2158,0x2160,
    0x2620,0x2624,0x2628,0x262c,0x2630,
    0x26a0,0x26a4,0x26a8,0x26ac,0x26b0
};
static const unsigned cam_sv_tag[]={0,8,0xc,0x14,0x18,0x1c,0x20,0x24,0x3c};
static const unsigned cam_sv_dma_group[]={0x1200,0x1600,0x1800,0x1900,0x1b00,0x1d00};
/* A includes metadata tags6/7: the actual IRQ/dequeue address readers use
 * them too. B has six image tags; C has three documented image slots.
 * Do not copy the generic vendor clear loop beyond C's legal slots into
 * the LEN aperture. These are configuration reads, never a DMA grant. */
static const unsigned cam_sv_dma_tags[]={8,6,3,8,6,3};
struct cam_sv_field { unsigned offset; uint32_t mask; };
static const struct cam_sv_field cam_sv_fields[CAM_SV_FIELDS]={
    {0x548,UINT32_MAX},{0x54c,UINT32_MAX},{0x554,0xff1fefdf},
    {0x1210,0xffff001f},{0x1b8,0xff},{0x1bc,0xff},{0x1c0,0xff},
    {0x1c4,0xff},{0x1cc,0xff},{0x1c8,0xff},
    /* WDMA address words, not burst/stride or a CPU-PA alias. The vendor
     * writer stores full low/high words; FH_SPARE is a 32-bit request cookie
     * read by the normal SOF/hybrid handlers, never capture_id truncation. */
    {0x1200,UINT32_MAX},{0x1204,UINT32_MAX},{0x57c,UINT32_MAX},
    {0x540,CAM_SV_FBC_MASK}
};
_Static_assert(CAM_SV_FBC+1==CAM_SV_FIELDS,"CAMSV request fields");
_Static_assert(CAM_SV_FIELDS<32,"CAMSV dirty bitmap");
static struct {
    vm_offset_t view[2];
    unsigned count, offset[CAM_SV_MAX_REGS], field[CAM_SV_FIELDS], db_reg;
    uint32_t before[2][CAM_SV_MAX_REGS], expected[2][CAM_SV_MAX_REGS];
    uint32_t target[CAM_SV_FIELDS], dirty, pending_next;
    unsigned pending_slot, db_phase;
    uint64_t epoch, sequence, command, completed;
    bool held, close_attempted, terminal, pending_seen, commit_pending, db_issued;
    bool request_held, request_ready;
    bool fbc_sampled, fbc_ready;
    uint32_t fbc_before[2];
    unsigned credit_phase;
    bool credit_issued;
    uint32_t credit_seen, credit_ack, credit_samples, credit_target[2];
    unsigned receive_phase;
    uint32_t receive_issued, receive_off_issued, receive_seen[2], receive_off_seen[2], receive_samples;
    struct x9_camera_camsv_slot *request_slot;
    struct x9_camera_camsv_request request;
    struct x9_camera_camsv_dma request_dma;
    int error;
    struct x9_camera_sv_hw_status status;
} cam_sv;
static void cam_sv_gate_init(void);
static void cam_sv_gate_capture(void);
static void cam_sv_gate_status(void);
static void cam_sv_gate_fault(int);
static void cam_sv_gate_observed(unsigned,const uint32_t *);
static int cam_sv_gate_observe_word(unsigned,unsigned,uint32_t,bool *);
static bool cam_sv_gate_off_ready(void);
static unsigned cam_sv_find(unsigned offset)
{
    for(unsigned n=0;n<cam_sv.count;n++)if(cam_sv.offset[n]==offset)return n;
    return CAM_SV_MAX_REGS;
}
static int cam_sv_add(unsigned offset)
{
    if(offset>=0x3000 || (offset&3) || cam_sv.count==CAM_SV_MAX_REGS)return EOVERFLOW;
    if(cam_sv_find(offset)!=CAM_SV_MAX_REGS)return EEXIST;
    cam_sv.offset[cam_sv.count++]=offset;return 0;
}
static void cam_sv_status(void)
{
    struct x9_camera_sv_hw_status *s=&cam_sv.status;
    s->sensor_epoch=cam_sv.epoch;s->command=cam_sv.command;s->completed=cam_sv.completed;
    s->last_errno=cam_sv.error;s->dirty=cam_sv.dirty;
    s->pending=cam_sv.pending_slot<=CAM_SV_FIELDS;
    s->commit_pending=cam_sv.commit_pending;s->db_phase=cam_sv.db_phase;
    s->db_issued=cam_sv.db_issued;s->terminal=cam_sv.terminal;
    if(cam_sv.held && cam_sv.error)s->flags|=X9_CAM_SV_HW_RETAINED;
    else s->flags&=~X9_CAM_SV_HW_RETAINED;
    if(cam_sv.request_ready)s->flags|=X9_CAM_SV_HW_REQUEST;
    else s->flags&=~X9_CAM_SV_HW_REQUEST;
    if(cam_sv.fbc_ready && cam_sv.credit_phase==X9_CAM_SV_CREDIT_EMPTY)s->flags|=X9_CAM_SV_HW_FBC_EMPTY;
    else s->flags&=~X9_CAM_SV_HW_FBC_EMPTY;
    if(cam_sv.fbc_ready && cam_sv.credit_phase==X9_CAM_SV_CREDIT_ACCEPTED)s->flags|=X9_CAM_SV_HW_FBC_CREDIT;
    else s->flags&=~X9_CAM_SV_HW_FBC_CREDIT;
    s->credit_phase=cam_sv.credit_phase;s->credit_issued=cam_sv.credit_issued;
    s->credit_seen=cam_sv.credit_seen;s->credit_ack=cam_sv.credit_ack;
    s->credit_samples=cam_sv.credit_samples;
    s->receive_phase=cam_sv.receive_phase;s->receive_issued=cam_sv.receive_issued;
    s->receive_off_issued=cam_sv.receive_off_issued;s->receive_samples=cam_sv.receive_samples;
    memcpy(s->receive_seen,cam_sv.receive_seen,sizeof(s->receive_seen));
    memcpy(s->receive_off_seen,cam_sv.receive_off_seen,sizeof(s->receive_off_seen));
    if(cam_sv.receive_phase==X9_CAM_SV_RECEIVE_ARMED && !cam_sv.error)s->flags|=X9_CAM_SV_HW_RECEIVE_ARMED;
    else s->flags&=~X9_CAM_SV_HW_RECEIVE_ARMED;
    if(cam_sv.receive_phase==X9_CAM_SV_RECEIVE_QUIESCENT && !cam_sv.error)s->flags|=X9_CAM_SV_HW_RECEIVE_QUIESCENT;
    else s->flags&=~X9_CAM_SV_HW_RECEIVE_QUIESCENT;
    cam_sv_gate_status();
    if(cam_sv.request_held) {
        s->request_generation=cam_sv.request.frame.generation;
        s->request_capture_id=cam_sv.request.frame.capture_id;
        s->request_sequence=cam_sv.request.inner_sequence;
    }
}
static void cam_sv_status_copy(uint64_t epoch,struct x9_camera_sv_hw_status *s)
{
    cam_sv_status();
    if(cam_sv.epoch==epoch)*s=cam_sv.status;
    else {memset(s,0,sizeof(*s));s->sensor_epoch=epoch;}
}
static int cam_sv_fault(int e)
{
    if(!cam_sv.error)cam_sv.error=e;
    if(cam_sv.request_held) {
        cam_sv.request_ready=false;
        cam_sv.fbc_ready=false;
        if(!cam_sv.status.request_errno)cam_sv.status.request_errno=e;
        if(cam_sv.fbc_sampled && !cam_sv.status.fbc_errno)cam_sv.status.fbc_errno=e;
        if(cam_sv.credit_issued && !cam_sv.status.credit_errno)cam_sv.status.credit_errno=e;
        if(cam_sv.receive_phase!=X9_CAM_SV_RECEIVE_IDLE && !cam_sv.status.receive_errno)
            cam_sv.status.receive_errno=e;
    }
    if(e==ESTALE || e==EACCES || e==EBUSY || e==EOVERFLOW)cam_sv.terminal=true;
    cam_sv_gate_fault(e);
    cam_sv_status();return e;
}
/* No OUTMUX or receiver recursion here. The real MM and optional SMI
 * journals establish power/clock eligibility BEFORE every bank sample. */
static int cam_sv_power_guard(void)
{
    int e;
    if(cam_sv.epoch!=cam_rx_live.sensor_epoch || cam_rx_live.epoch!=cam_mm_live.epoch)return ESTALE;
    e=cam_mm_guard(&cam_mm,true);if(e)return e;
    return cam_smi_clock_guard();
}
static int cam_sv_inactive(const uint32_t *v)
{
    /* Commands/foreign IRQ/DMA enables remain off; only this request's exact
     * separately issued receive-arm controls may transition below.
     * FBC_SUB_EN is bit14, RESET bit8, EN bit15, RCNT_INC bit7. */
    static const unsigned zero[]={0x040,0x044,0x1d0,0x1d4,0x1d8,0x228,
        0x240,0x244,0x248,0x24c,0x250,0x344,0x34c,0x35c,0x364,0x36c,
        0x1020,0x1048,0x2010,0x2014,0x2018};
    for(unsigned n=0;n<sizeof(zero)/sizeof(zero[0]);n++)
        if(v[cam_sv_find(zero[n])]&~cam_sv_receive_busy_exemption(zero[n]))return EBUSY;
    if((v[cam_sv_find(0x140)]&1&~cam_sv_gate_busy_exemption(0x140)) ||
       (v[cam_sv_find(0x148)]&1&~cam_sv_gate_busy_exemption(0x148)))return EBUSY;
    if(v[cam_sv.db_reg]&((1u<<16)|(1u<<26)|(1u<<31)))return EBUSY;
    if(v[cam_sv_find(0x1024)]&0x11)return EBUSY; /* no pseudo/FPGA DMA */
    if(v[cam_sv_find(0x2140)]&1)return EBUSY; /* CQ SUB_THR0 enable */
    if((v[cam_sv_find(0x2100)]|v[cam_sv_find(0x2130)])&(1u<<16))return EBUSY; /* both CQ resets */
    for(unsigned tag=0;tag<8;tag++) {
        uint32_t mask=CAM_SV_FBC_BUSY;
        /* THIS request's EN and, only during its one issued command, RCNT.
         * Exact control/command values and counters are still checked below.
         * No foreign tag, RESET/LOCK/SUB_EN or unissued RCNT is permitted. */
        if(!tag)mask&=~cam_sv_fbc_busy_exemption();
        if(v[cam_sv_find(0x540+tag*0x40)]&mask)return EBUSY;
    }
    return 0;
}
/* Receiver's cold admission remains strict. Its LIVE busy check can ignore
 * only our tag0 EN and the pending issued command after exact journal checks.
 * Not an ownership grant derived from expected/zero registers. */
static uint32_t cam_sv_fbc_busy_exemption(void)
{
    uint32_t mask=0;
    if(cam_sv.held && cam_sv.request_held && cam_sv.fbc_sampled && !cam_sv.terminal) {
        mask=CAM_SV_FBC_EN;
        if(cam_sv.credit_issued && cam_sv.credit_phase==X9_CAM_SV_CREDIT_PENDING)
            mask|=CAM_SV_FBC_RCNT;
    }
    return mask;
}
/* Canonical comparison only, NOT a published/accepted register value. This
 * one request may momentarily read its exact EN|RCNT command before hardware
 * consumes it. Once a bank has acknowledged the command, reappearance is a
 * conflict even if a later read would again be EN. Cold admission has no
 * issued command and therefore no exception. All other bits remain exact. */
static uint32_t cam_sv_fbc_compare_word(unsigned bank,uint32_t value)
{
    if(bank<2 && cam_sv.credit_issued && cam_sv.credit_phase==X9_CAM_SV_CREDIT_PENDING &&
       !(cam_sv.credit_ack&(1u<<bank)) &&
       value==(cam_sv.target[CAM_SV_FBC]|CAM_SV_FBC_RCNT))return value&~CAM_SV_FBC_RCNT;
    return value;
}
/* Merely an exception to the cold busy classifier, NOT authority. The full
 * snapshot guard below still requires the exact per-bank issued transition.
 * Source/DMA enables do not broaden the EN/RCNT exception or allow CMOS/VF. */
static uint32_t cam_sv_receive_busy_exemption(unsigned offset)
{
    if(!cam_sv.held || !cam_sv.request_held || cam_sv.terminal ||
       cam_sv.credit_phase!=X9_CAM_SV_CREDIT_ACCEPTED)return 0;
    if(cam_sv.receive_phase!=X9_CAM_SV_RECEIVE_PENDING &&
       cam_sv.receive_phase!=X9_CAM_SV_RECEIVE_ARMED &&
       cam_sv.receive_phase!=X9_CAM_SV_RECEIVE_QUIESCING)return 0;
    for(unsigned i=0;i<X9_CAM_SV_RECEIVE_FIELDS;i++) {
        uint32_t bit=1u<<i;
        if(cam_sv_receive_fields[i].offset==offset && (cam_sv.receive_issued&bit))
            return cam_sv_receive_fields[i].target;
    }
    return 0;
}
/* Comparison of HISTORICAL samples may span this one posted transition.
 * Actual samples are qualified/recorded below before any comparison, so a
 * target->prior regression cannot be normalized away. Once no transition
 * is pending, all comparisons are exact again. No raw evidence is changed. */
static uint32_t cam_sv_receive_compare_word(unsigned bank,uint32_t value)
{
    unsigned i=X9_CAM_SV_RECEIVE_FIELDS-1;
    uint32_t bit=1u<<i;
    if(bank<2 && !cam_sv.terminal &&
       cam_sv.credit_phase==X9_CAM_SV_CREDIT_ACCEPTED && (cam_sv.receive_issued&bit) &&
       (cam_sv.receive_phase==X9_CAM_SV_RECEIVE_PENDING ||
        cam_sv.receive_phase==X9_CAM_SV_RECEIVE_QUIESCING) &&
       (value==0 || value==cam_sv_receive_fields[i].target))return 0;
    return value;
}
static int cam_sv_receive_sample_word(unsigned bank,vm_offset_t view,uint32_t value)
{
    unsigned i=X9_CAM_SV_RECEIVE_FIELDS-1;
    uint32_t bit=1u<<i,next,prior,*seen;
    bool off;
    if(!(cam_sv.receive_issued&bit))return 0; /* cold admission stays strict */
    if(bank>=2 || view!=cam_sv.view[bank] || !cam_sv.held || !cam_sv.request_held ||
       cam_sv.terminal || cam_sv.credit_phase!=X9_CAM_SV_CREDIT_ACCEPTED)
        return cam_sv_fault(ESTALE);
    off=(cam_sv.receive_off_issued&bit)!=0;
    next=off?0:cam_sv_receive_fields[i].target;prior=off?cam_sv_receive_fields[i].target:0;
    seen=off?&cam_sv.receive_off_seen[bank]:&cam_sv.receive_seen[bank];
    cam_sv.status.receive_after[bank][i]=value;
    if(value!=next && ((*seen&bit) || value!=prior))return cam_sv_fault(ESTALE);
    if(value==next)*seen|=bit;
    return 0;
}
static int cam_sv_receive_observe_word(unsigned bank,unsigned reg,uint32_t value,bool *allowed)
{
    for(unsigned i=0;i<X9_CAM_SV_RECEIVE_FIELDS;i++) {
        uint32_t bit=1u<<i,next,prior,*seen;
        bool off;
        if(cam_sv.offset[reg]!=cam_sv_receive_fields[i].offset || !(cam_sv.receive_issued&bit))continue;
        if(cam_sv.credit_phase!=X9_CAM_SV_CREDIT_ACCEPTED ||
           cam_sv.receive_phase<X9_CAM_SV_RECEIVE_PENDING ||
           cam_sv.receive_phase>X9_CAM_SV_RECEIVE_QUIESCENT)return ESTALE;
        off=(cam_sv.receive_off_issued&bit)!=0;
        next=off?0:cam_sv_receive_fields[i].target;prior=off?cam_sv_receive_fields[i].target:0;
        seen=off?&cam_sv.receive_off_seen[bank]:&cam_sv.receive_seen[bank];
        if(value!=next && ((*seen&bit) || value!=prior))return ESTALE;
        *allowed=true;
        if(value==next){*seen|=bit;cam_sv.expected[bank][reg]=value;}
    }
    return 0;
}
static int cam_sv_fbc_counters_guard(void)
{
    uint32_t value;
    int e;
    if(!cam_sv.fbc_sampled)return 0;
    for(unsigned bank=0;bank<2;bank++) {
        e=cam_sv_power_guard();if(e)return e;
        value=cam_rr(cam_sv.view[bank],0x544);
        cam_sv.status.fbc_after[bank]=value;
        if(cam_sv.credit_phase==X9_CAM_SV_CREDIT_EMPTY) {
            if(value!=cam_sv.fbc_before[bank])return ESTALE;
        } else if(cam_sv.credit_phase==X9_CAM_SV_CREDIT_PENDING ||
                  cam_sv.credit_phase==X9_CAM_SV_CREDIT_ACCEPTED) {
            bool target=value==cam_sv.credit_target[bank];
            if(!target && (cam_sv.credit_phase!=X9_CAM_SV_CREDIT_PENDING ||
               (cam_sv.credit_seen&(1u<<bank)) || value!=cam_sv.fbc_before[bank]))return ESTALE;
            if(target)cam_sv.credit_seen|=1u<<bank;
        } else return ESTALE;
    }
    return 0;
}
static void cam_sv_observed(unsigned bank,const uint32_t *v)
{
    struct x9_camera_sv_hw_status *s=&cam_sv.status;
    s->sampled|=1u<<bank;s->db_after[bank]=v[cam_sv.db_reg];
    for(unsigned slot=0;slot<CAM_SV_FIELDS;slot++)s->after[bank][slot]=v[cam_sv.field[slot]];
    for(unsigned i=0;i<X9_CAM_SV_RECEIVE_FIELDS;i++)
        s->receive_after[bank][i]=v[cam_sv_find(cam_sv_receive_fields[i].offset)];
    cam_sv_gate_observed(bank,v);
}
/* The original receiver evidence never changes. Only actual bank values
 * accepted by this journal are published, including live-through inner
 * effects with DB_EN=0. We never synthesize inner consumption. */
static void cam_sv_publish(void)
{
    for(unsigned bank=0;bank<2;bank++)for(unsigned k=0;k<X9_CAM_RX_SV_WORDS;k++) {
        unsigned n=cam_sv_find(cam_rx_sv_offsets[k]);
        if(n!=CAM_SV_MAX_REGS) {
            uint32_t actual=cam_sv.expected[bank][n];
            if(!bank && cam_sv.pending_seen && cam_sv.pending_slot<CAM_SV_FIELDS &&
               n==cam_sv.field[cam_sv.pending_slot])actual=cam_sv.pending_next;
            cam_rx_live.expected.sv[bank][k]=actual;
        }
    }
}
static int cam_sv_guard_impl(void)
{
    uint32_t v[CAM_SV_MAX_REGS];
    unsigned pending_reg=CAM_SV_MAX_REGS;
    int e;
    if(!cam_sv.held)return 0;
    if(cam_sv.terminal)return cam_sv.error?cam_sv.error:ESTALE;
    if(cam_sv.pending_slot<CAM_SV_FIELDS)pending_reg=cam_sv.field[cam_sv.pending_slot];
    else if(cam_sv.pending_slot==CAM_SV_FIELDS)pending_reg=cam_sv.db_reg;
    for(unsigned bank=0;bank<2;bank++) {
        e=cam_sv_power_guard();if(e)return e;
        for(unsigned n=0;n<cam_sv.count;n++)v[n]=cam_rr(cam_sv.view[bank],cam_sv.offset[n]);
        cam_sv_observed(bank,v);e=cam_sv_inactive(v);if(e)return e;
        /* Keep raw command observations in status; only stable FBC control
         * is part of expected. The separate issued/seen/ack record accounts
         * for RCNT, including its legitimate hardware self-clear. */
        v[cam_sv.field[CAM_SV_FBC]]=cam_sv_fbc_compare_word(bank,v[cam_sv.field[CAM_SV_FBC]]);
        for(unsigned n=0;n<cam_sv.count;n++) {
            bool allowed=v[n]==cam_sv.expected[bank][n];
            if(n==pending_reg) {
                if(!bank && cam_sv.pending_seen)allowed=v[n]==cam_sv.pending_next;
                if(v[n]==cam_sv.pending_next)allowed=true;
            }
            /* Only our changed fields/policy may propagate to inner. Each
             * observed target becomes its exact expected; regression is
             * then an error, including if a later sample returns to target. */
            if(bank && v[n]==cam_sv.expected[0][n] &&
               !(n==pending_reg && cam_sv.expected[1][n]==cam_sv.pending_next))allowed=true;
            e=cam_sv_receive_observe_word(bank,n,v[n],&allowed);if(e)return e;
            e=cam_sv_gate_observe_word(bank,n,v[n],&allowed);if(e)return e;
            if(!allowed)return ESTALE;
        }
        if(!bank && pending_reg<CAM_SV_MAX_REGS && v[pending_reg]==cam_sv.pending_next)
            cam_sv.pending_seen=true;
        if(bank)memcpy(cam_sv.expected[1],v,cam_sv.count*sizeof(v[0]));
    }
    e=cam_sv_fbc_counters_guard();if(e)return e;
    cam_sv_publish();return cam_sv_power_guard();
}
static int cam_sv_guard(void)
{
    int e=cam_sv_guard_impl();return e?cam_sv_fault(e):0;
}
/* Thread-only controller authority, kept separate from the hardware guard.
 * No owner callback under hardIRQ or camera_s1_lock; this callback cannot
 * re-enter camera APIs. The immutable copy is also used during restoration,
 * when the address words legitimately differ from the completed request.
 * Its lifetime is NOT ended by cache-only RETIRED or GIC retirement. */
static int cam_sv_request_authority(void)
{
    const struct x9_camera_camsv_dma *d=&cam_sv.request_dma;
    if(!cam_sv.request_held)return 0;
    if(!d->context || !d->lease || !d->backing || !d->check)return ESTALE;
    return d->check(d->context,d->lease,&cam_sv.request);
}
static int cam_sv_environment(void)
{
    int e=cam_sv_request_authority();if(e)return cam_sv_fault(e);
    e=cam_sv_guard();if(e)return e;
    e=cam_out_guard();if(e)return cam_sv_fault(e);
    e=cam_rx_session_guard();if(e)return cam_sv_fault(e);
    e=cam_mac_guard();if(e)return cam_sv_fault(e);
    e=cam_phy_guard();if(e)return cam_sv_fault(e);
    e=cam_sv_request_authority();return e?cam_sv_fault(e):0;
}
static int cam_sv_pending_observe(void)
{
    unsigned reg;
    int e;
    if(cam_sv.pending_slot>CAM_SV_FIELDS)return 0;
    reg=cam_sv.pending_slot==CAM_SV_FIELDS?cam_sv.db_reg:cam_sv.field[cam_sv.pending_slot];
    for(unsigned sample=0;sample<2;sample++) {
        e=cam_sv_environment();if(e)return e;
        if(cam_rr(cam_sv.view[0],cam_sv.offset[reg])!=cam_sv.pending_next)
            return cam_sv.pending_seen?cam_sv_fault(ESTALE):EAGAIN;
        /* DB policy is a phase boundary, not just an outer shadow word.
         * Do not issue UP while a late inner DOWN could still arrive. */
        if(cam_sv.pending_slot==CAM_SV_FIELDS &&
           cam_rr(cam_sv.view[1],cam_sv.offset[reg])!=cam_sv.pending_next)
            return cam_sv.expected[1][reg]==cam_sv.pending_next?cam_sv_fault(ESTALE):EAGAIN;
        if(!sample)delay(1);
    }
    cam_sv.expected[0][reg]=cam_sv.pending_next;
    cam_sv.pending_slot=CAM_SV_FIELDS+1;cam_sv.pending_seen=false;
    cam_sv_publish();return cam_sv_guard();
}
static int cam_sv_pending_wait(uint64_t deadline)
{
    int e;
    for(unsigned poll=0;poll<10000;poll++) {
        if(mach_absolute_time()>=deadline)return ETIMEDOUT;
        e=cam_sv_pending_observe();
        if(mach_absolute_time()>=deadline)return ETIMEDOUT;
        if(e!=EAGAIN)return e;
        delay(1);
    }
    return ETIMEDOUT;
}
/* The single writable view is outer. Register and full next value are
 * journaled BEFORE any posted store; a failed write is never repeated. */
static int cam_sv_post(unsigned slot,uint32_t next)
{
    unsigned reg=slot==CAM_SV_FIELDS?cam_sv.db_reg:cam_sv.field[slot];
    int e=cam_sv_environment();if(e)return e;
    /* Credits are NOT ordinary configuration dirty bits. No DB load,
     * field restore or address rearm may cancel/replay an issued credit. */
    if(cam_sv.credit_issued)return EBUSY;
    if(cam_sv.pending_slot<=CAM_SV_FIELDS)return EBUSY;
    if(next==cam_sv.expected[0][reg])return 0;
    cam_sv.pending_slot=slot;cam_sv.pending_next=next;cam_sv.pending_seen=false;
    if(slot<CAM_SV_FIELDS)cam_sv.dirty|=1u<<slot;
    else cam_sv.db_issued=true;
    cam_rw(cam_sv.view[0],cam_sv.offset[reg],next);
    e=cam_sv_pending_observe();return e==EAGAIN?EIO:e;
}
static int cam_sv_field_write(unsigned slot,uint32_t value)
{
    uint32_t mask=cam_sv_fields[slot].mask;
    uint32_t next=(cam_sv.expected[0][cam_sv.field[slot]]&~mask)|(value&mask);
    return cam_sv_post(slot,next);
}
static int cam_sv_target_observe(void)
{
    int e;
    for(unsigned sample=0;sample<2;sample++) {
        e=cam_sv_environment();if(e)return e;
        if(memcmp(cam_sv.expected[0],cam_sv.expected[1],cam_sv.count*sizeof(uint32_t)))return EAGAIN;
        if(!sample)delay(1);
    }
    return 0;
}
static int cam_sv_target_wait(uint64_t deadline)
{
    int e;
    for(unsigned poll=0;poll<10000;poll++) {
        if(mach_absolute_time()>=deadline)return ETIMEDOUT;
        e=cam_sv_target_observe();
        if(mach_absolute_time()>=deadline)return ETIMEDOUT;
        if(e!=EAGAIN)return e;
        delay(1);
    }
    return ETIMEDOUT;
}
/* Ordinary DB_EN policy, not a CFG_DONE or image-completion event. Follow
 * the actual upstream down/up, never FORCE_LOAD/SWITCH_FORCE or inner
 * configuration writes. A down already 0 is skipped (no posted no-op to
 * confuse retirement). A timed-out phase is only observed on sole close;
 * if DOWN finally completes, UP is issued once, not a replay of DOWN. */
static int cam_sv_commit(void)
{
    uint64_t span,deadline;
    uint32_t db;
    int e=cam_sv_environment();if(e)return e;
    nanoseconds_to_absolutetime(10000000,&span);deadline=mach_absolute_time()+span;
    if(!cam_sv.commit_pending) {
        if(cam_sv.pending_slot<=CAM_SV_FIELDS)return EBUSY;
        if(cam_sv.sequence==UINT64_MAX)return cam_sv_fault(EOVERFLOW);
        cam_sv.command=++cam_sv.sequence;cam_sv.commit_pending=true;
        db=cam_sv.expected[0][cam_sv.db_reg];
        cam_sv.db_phase=(db&CAM_SV_DB_EN)?CAM_SV_DB_DOWN:CAM_SV_DB_UP;
        cam_sv.db_issued=false;
    }
    if(!cam_sv.db_issued) {
        db=cam_sv.expected[0][cam_sv.db_reg];
        e=cam_sv_post(CAM_SV_FIELDS,cam_sv.db_phase==CAM_SV_DB_DOWN?db&~CAM_SV_DB_EN:db|CAM_SV_DB_EN);
        if(e && e!=EIO)return e;
        if(!cam_sv.db_issued)return cam_sv_fault(ESTALE);
    }
    e=cam_sv_pending_wait(deadline);if(e)return e;
    if(cam_sv.db_phase==CAM_SV_DB_DOWN) {
        cam_sv.db_phase=CAM_SV_DB_UP;cam_sv.db_issued=false;
        db=cam_sv.expected[0][cam_sv.db_reg];
        e=cam_sv_post(CAM_SV_FIELDS,db|CAM_SV_DB_EN);
        if(e && e!=EIO)return e;
        if(!cam_sv.db_issued)return cam_sv_fault(ESTALE);
        e=cam_sv_pending_wait(deadline);if(e)return e;
    }
    e=cam_sv_target_wait(deadline);if(e)return e;
    cam_sv.completed=cam_sv.command;cam_sv.commit_pending=cam_sv.db_issued=false;
    cam_sv.db_phase=CAM_SV_DB_IDLE;return cam_sv_guard();
}
static int cam_sv_open_locked(uint64_t epoch)
{
    struct x9_camera_pipeline plan;
    uint32_t a[CAM_SV_MAX_REGS],b[CAM_SV_MAX_REGS];
    const struct cam_rx_maps *m;
    unsigned sensor=cam_sensor_live.probe.sensor;
    int e;
    if(cam_sv.held)return EALREADY;
    if(!cam_out.held || cam_out.epoch!=epoch || !cam_out.consumed || cam_out.error ||
       cam_out.commit_pending || !cam_rx_sensor_held_locked(epoch) || cam_rx_live.error ||
       cam_rx_live.close_attempted)return ESTALE;
    e=cam_pipeline_plan(sensor,&plan);if(e)return e;
    e=cam_out_guard();if(e)return e;
    e=cam_rx_session_guard();if(e)return e;
    cam_sv.count=0;
    for(unsigned n=0;n<sizeof(cam_sv_fixed)/sizeof(cam_sv_fixed[0]);n++) {
        e=cam_sv_add(cam_sv_fixed[n]);if(e)return e;
    }
    for(unsigned tag=0;tag<8;tag++)for(unsigned n=0;n<sizeof(cam_sv_tag)/sizeof(cam_sv_tag[0]);n++) {
        e=cam_sv_add(0x540+tag*0x40+cam_sv_tag[n]);if(e)return e;
    }
    for(unsigned group=0;group<6;group++)for(unsigned tag=0;tag<cam_sv_dma_tags[group];tag++) {
        unsigned base=cam_sv_dma_group[group]+tag*0x40;
        e=cam_sv_add(base);if(!e)e=cam_sv_add(base+4);if(!e)e=cam_sv_add(base+0x10);
        if(e)return e;
        if(group<3) {
            e=cam_sv_add(base+0x18);if(!e)e=cam_sv_add(base+0x34);if(!e)e=cam_sv_add(base+0x3c);
            if(e)return e;
        }
    }
    cam_sv.db_reg=cam_sv_find(0x1b4);
    for(unsigned slot=0;slot<CAM_SV_FIELDS;slot++) {
        cam_sv.field[slot]=cam_sv_find(cam_sv_fields[slot].offset);
        if(cam_sv.field[slot]==CAM_SV_MAX_REGS)return EINVAL;
    }
    m=&cam_rx_maps[sensor==X9_CAMERA_BACK?0:1];
    cam_sv.view[0]=m->sv[0];cam_sv.view[1]=m->sv[1];cam_sv.epoch=epoch;
    memset(&cam_sv.status,0,sizeof(cam_sv.status));cam_sv.status.sensor=sensor;
    cam_sv.status.flags=X9_CAM_SV_HW_ATTEMPTED;
    cam_sv.error=0;cam_sv.terminal=false;cam_sv.dirty=0;cam_sv.command=cam_sv.completed=0;
    cam_sv.request_held=cam_sv.request_ready=false;cam_sv.request_slot=NULL;
    cam_sv.fbc_sampled=cam_sv.fbc_ready=false;
    memset(cam_sv.fbc_before,0,sizeof(cam_sv.fbc_before));
    cam_sv.credit_phase=X9_CAM_SV_CREDIT_EMPTY;cam_sv.credit_issued=false;
    cam_sv.credit_seen=cam_sv.credit_ack=cam_sv.credit_samples=0;
    memset(cam_sv.credit_target,0,sizeof(cam_sv.credit_target));
    cam_sv.receive_phase=X9_CAM_SV_RECEIVE_IDLE;
    cam_sv.receive_issued=cam_sv.receive_off_issued=cam_sv.receive_samples=0;
    memset(cam_sv.receive_seen,0,sizeof(cam_sv.receive_seen));
    memset(cam_sv.receive_off_seen,0,sizeof(cam_sv.receive_off_seen));
    cam_sv_gate_init();
    memset(&cam_sv.request,0,sizeof(cam_sv.request));
    memset(&cam_sv.request_dma,0,sizeof(cam_sv.request_dma));
    cam_sv.commit_pending=cam_sv.pending_seen=cam_sv.db_issued=false;cam_sv.db_phase=CAM_SV_DB_IDLE;
    cam_sv.pending_slot=CAM_SV_FIELDS+1;cam_sv.close_attempted=false;
    for(unsigned bank=0;bank<2;bank++) {
        e=cam_sv_power_guard();if(e)return cam_sv_fault(e);
        for(unsigned n=0;n<cam_sv.count;n++)a[n]=cam_rr(cam_sv.view[bank],cam_sv.offset[n]);
        delay(10);e=cam_sv_power_guard();if(e)return cam_sv_fault(e);
        for(unsigned n=0;n<cam_sv.count;n++)b[n]=cam_rr(cam_sv.view[bank],cam_sv.offset[n]);
        cam_sv_observed(bank,b);
        if(memcmp(a,b,cam_sv.count*sizeof(uint32_t)))return cam_sv_fault(ESTALE);
        e=cam_sv_inactive(b);if(e)return cam_sv_fault(e);
        memcpy(cam_sv.before[bank],b,cam_sv.count*sizeof(uint32_t));
    }
    /* DB toggles can transfer more than tag0. Do not publish inherited
     * staged geometry/addresses/other tags/CQ or request policy. */
    if(memcmp(cam_sv.before[0],cam_sv.before[1],cam_sv.count*sizeof(uint32_t)))return cam_sv_fault(EBUSY);
    memcpy(cam_sv.expected,cam_sv.before,sizeof(cam_sv.expected));
    for(unsigned slot=0;slot<CAM_SV_FIELDS;slot++)cam_sv.status.before[slot]=cam_sv.before[0][cam_sv.field[slot]];
    for(unsigned bank=0;bank<2;bank++)cam_sv.status.db_before[bank]=cam_sv.before[bank][cam_sv.db_reg];
    for(unsigned i=0;i<X9_CAM_SV_RECEIVE_FIELDS;i++) {
        cam_sv.status.receive_before[i]=cam_sv.before[0][cam_sv_find(cam_sv_receive_fields[i].offset)];
        cam_sv.status.receive_target[i]=cam_sv_receive_fields[i].target;
    }
    cam_sv_gate_capture();
    cam_sv.status.flags|=X9_CAM_SV_HW_SNAPSHOT;cam_sv.held=true;
    cam_sv.target[0]=plan.grab_pixels;cam_sv.target[1]=plan.grab_lines;
    cam_sv.target[2]=plan.format;cam_sv.target[3]=plan.dma_basic;
    cam_sv.target[4]=1;cam_sv.target[5]=cam_sv.target[6]=cam_sv.target[7]=0;
    cam_sv.target[8]=cam_sv.target[9]=1;
    /* Initial geometry setup never publishes a buffer/cookie. Keep the
     * address/FH/FBC fields exactly at this cold epoch's captured priors. */
    for(unsigned slot=CAM_SV_CONFIG_FIELDS;slot<CAM_SV_FIELDS;slot++)
        cam_sv.target[slot]=cam_sv.before[0][cam_sv.field[slot]];
    for(unsigned slot=0;slot<CAM_SV_FIELDS;slot++) {
        uint32_t mask=cam_sv_fields[slot].mask;
        cam_sv.status.target[slot]=(cam_sv.before[0][cam_sv.field[slot]]&~mask)|(cam_sv.target[slot]&mask);
    }
    e=cam_sv_environment();
    for(unsigned slot=0;slot<CAM_SV_CONFIG_FIELDS && !e;slot++)e=cam_sv_field_write(slot,cam_sv.target[slot]);
    if(!e)e=cam_sv.dirty?cam_sv_commit():cam_sv_target_observe();
    if(!e) {
        cam_sv.status.flags|=X9_CAM_SV_HW_CONFIGURED;
        e=cam_irq_prepare_locked(epoch); /* dormant; no GIC/source enable */
    }
    if(e)cam_sv_fault(e);
    cam_sv_status();return e;
}
/* Normal composer FBC_EN initialization, AFTER the exact address/FH target
 * has reached both banks. This helper never submits the later 8080 command;
 * the separate one-command engine follows actual GIC publication.
 * The owner callback must independently retire prior producer/history;
 * count0 here only selects an EMPTY no-credit programming baseline. It is
 * neither a DMA completion nor authority to release mapping/power/RAW.
 * Actual repeated rearm keeps this original counter evidence, no rebasing. */
static int cam_sv_fbc_prepare_locked(void)
{
    uint32_t observed[2],first,target;
    bool changed;
    int e=cam_sv_environment();if(e)return e;
    if(!cam_sv.fbc_sampled) {
        for(unsigned bank=0;bank<2;bank++) {
            e=cam_sv_environment();if(e)return e;
            first=cam_rr(cam_sv.view[bank],0x544);
            cam_sv.status.fbc_before[bank]=first;
            delay(1);e=cam_sv_environment();if(e)return e;
            observed[bank]=cam_rr(cam_sv.view[bank],0x544);
            cam_sv.status.fbc_sampled|=1u<<bank;
            cam_sv.status.fbc_after[bank]=observed[bank];
            if(first!=observed[bank])return cam_sv_fault(ESTALE);
            if(observed[bank]&CAM_SV_FBC_COUNT_MASK)return cam_sv_fault(EBUSY);
        }
        if(observed[0]!=observed[1])return cam_sv_fault(ESTALE);
        for(unsigned bank=0;bank<2;bank++)
            cam_sv.status.fbc_before[bank]=cam_sv.fbc_before[bank]=observed[bank];
        cam_sv.fbc_sampled=true;
    }
    /* Even reserved bits are immutable; write() masks only named fields.
     * All command bits are zero in the value actually emitted. */
    target=(cam_sv.before[0][cam_sv.field[CAM_SV_FBC]]&~CAM_SV_FBC_MASK)|CAM_SV_FBC_EN;
    changed=cam_sv.expected[0][cam_sv.field[CAM_SV_FBC]]!=target;
    cam_sv.target[CAM_SV_FBC]=cam_sv.status.target[CAM_SV_FBC]=target;
    e=cam_sv_field_write(CAM_SV_FBC,target);
    if(!e)e=changed?cam_sv_commit():cam_sv_target_observe();
    if(!e)e=cam_sv_environment();
    if(!e)for(unsigned bank=0;bank<2;bank++)
        if(cam_sv.expected[bank][cam_sv.field[CAM_SV_FBC]]!=target)e=ESTALE;
    if(e)return cam_sv_fault(e);
    cam_sv.fbc_ready=true;return 0;
}
/* Called only after this invocation's actual x9_camera_camsv_arm has bound
 * the slot and performed per-page CIVAC. camera_control_lock/IRQ mutex held;
 * the IRQ worker is idle and all three GIC lines disabled. No producer is
 * enabled here. FBC_EN is initialized without RCNT/credit only AFTER the
 * actual addresses/cookie. No inactive/zero register is a native grant.
 * A later rearm has already changed the S1 request, so never run the OLD
 * request's owner callback after arm. Validate the new immutable binding
 * first; begin checked the old authority/environment BEFORE that arm. */
static int cam_sv_request_prepare_locked(uint64_t epoch,struct x9_camera_camsv_slot *slot)
{
    struct x9_camera_camsv_request r;
    struct x9_camera_camsv_dma d;
    bool changed=false;
    int e;
    if(!slot || !cam_sv.held || cam_sv.epoch!=epoch || cam_sv.close_attempted ||
       !(cam_sv.status.flags&X9_CAM_SV_HW_CONFIGURED))return ESTALE;
    if(cam_sv.error)return cam_sv.error;
    if(cam_sv.credit_issued)return EBUSY;
    if(cam_sv.commit_pending || cam_sv.pending_slot<=CAM_SV_FIELDS)return EBUSY;
    /* Copy under the actual slot lock; no callback while it is held. IRQ
     * accepting is still false, and controller serialization pins the slot. */
    lck_mtx_lock(&camera_camsv_lock);
    e=slot->state==X9_CAMSV_ARMED?0:ESTALE;
    r=slot->request;d=slot->dma;
    lck_mtx_unlock(&camera_camsv_lock);
    if(e)return e;
    if(r.tag || r.group || r.reserved || r.frame.sensor!=cam_sv.status.sensor ||
       r.frame.time || !r.frame.generation || !r.frame.capture_id || !r.iova ||
       (r.iova&0x3fffULL) || r.iova>=0x400000000ULL ||
       r.frame.bytes>0x400000000ULL-r.iova || d.version!=X9_CAMERA_CAMSV_VERSION ||
       d.reserved || !d.context || !d.lease || !d.backing || !d.check)return EINVAL;
    if(cam_sv.request_held && (cam_sv.request_slot!=slot ||
       cam_sv.request_dma.context!=d.context || cam_sv.request_dma.check!=d.check ||
       cam_sv.request_dma.backing!=d.backing))return EBUSY;
    e=d.check(d.context,d.lease,&r);if(e)return e;
    /* Bind BEFORE the first possible posted store. A partial address/DB
     * effect keeps this request's lease/backing and cannot be reset/retried. */
    cam_sv.request=r;cam_sv.request_dma=d;cam_sv.request_slot=slot;
    cam_sv.request_held=true;cam_sv.request_ready=false;
    cam_sv.fbc_ready=false;
    cam_sv.target[CAM_SV_ADDR_LO]=(uint32_t)r.iova;
    cam_sv.target[CAM_SV_ADDR_HI]=(uint32_t)(r.iova>>32);
    cam_sv.target[CAM_SV_FH]=r.inner_sequence;
    e=cam_sv_environment();
    for(unsigned field=CAM_SV_CONFIG_FIELDS;field<=CAM_SV_FH && !e;field++) {
        uint32_t target=cam_sv.target[field];
        cam_sv.status.target[field]=target;
        if(cam_sv.expected[0][cam_sv.field[field]]!=target)changed=true;
        e=cam_sv_field_write(field,target);
    }
    /* A repeated identical target is not a reason to emit another DB pulse.
     * Existing geometry dirty bits describe history, not a new request. */
    if(!e)e=changed?cam_sv_commit():cam_sv_target_observe();
    if(!e)e=cam_sv_environment();
    if(!e) {
        for(unsigned bank=0;bank<2;bank++)
            for(unsigned field=CAM_SV_CONFIG_FIELDS;field<=CAM_SV_FH;field++)
                if(cam_sv.expected[bank][cam_sv.field[field]]!=cam_sv.target[field])e=ESTALE;
    }
    if(!e) {
        e=cam_sv_fbc_prepare_locked();
        if(e && !cam_sv.status.fbc_errno)cam_sv.status.fbc_errno=e;
    }
    if(e)cam_sv_fault(e);
    else cam_sv.request_ready=true;
    cam_sv_status();
    printf("X9 CAMERA REQUEST: sensor=%u epoch=%llu generation=%llu capture_id=%llu "
        "inner=%x ready=%u errno=%d dirty=%x command=%llu/%llu "
        "fbc_empty=%u counters=%x/%x (actual S1 address/FH/FBC_EN, no RCNT/source/DMA)\n",
        cam_sv.status.sensor,epoch,r.frame.generation,r.frame.capture_id,r.inner_sequence,
        cam_sv.request_ready,e,cam_sv.dirty,cam_sv.command,cam_sv.completed,
        cam_sv.fbc_ready,cam_sv.status.fbc_after[0],cam_sv.status.fbc_after[1]);
    return e;
}
static int cam_sv_request_guard_locked(uint64_t epoch,const struct x9_camera_camsv_slot *slot)
{
    int e;
    if(!cam_sv.request_held || !cam_sv.request_ready || !cam_sv.fbc_ready || epoch!=cam_sv.epoch ||
       slot!=cam_sv.request_slot)return ESTALE;
    lck_mtx_lock(&camera_camsv_lock);
    e=(slot->state!=X9_CAMSV_ARMED && slot->state!=X9_CAMSV_RETIRED) ||
        memcmp(&slot->request,&cam_sv.request,sizeof(cam_sv.request)) ||
        slot->dma.version!=cam_sv.request_dma.version || slot->dma.reserved ||
        slot->dma.context!=cam_sv.request_dma.context || slot->dma.lease!=cam_sv.request_dma.lease ||
        slot->dma.backing!=cam_sv.request_dma.backing || slot->dma.check!=cam_sv.request_dma.check?ESTALE:0;
    lck_mtx_unlock(&camera_camsv_lock);
    if(!e)e=cam_sv_environment();
    if(!e && (cam_sv.commit_pending || cam_sv.pending_slot<=CAM_SV_FIELDS))e=EBUSY;
    if(!e)for(unsigned bank=0;bank<2;bank++) {
        const uint32_t *v=cam_sv.expected[bank];
        if(v[cam_sv.field[CAM_SV_ADDR_LO]]!=(uint32_t)cam_sv.request.iova ||
           v[cam_sv.field[CAM_SV_ADDR_HI]]!=(uint32_t)(cam_sv.request.iova>>32) ||
           v[cam_sv.field[CAM_SV_FH]]!=cam_sv.request.inner_sequence ||
           v[cam_sv.field[CAM_SV_FBC]]!=cam_sv.target[CAM_SV_FBC])e=ESTALE;
    }
    return e?cam_sv_fault(e):0;
}
/* Called ONLY at the actual end of normal GIC begin, with control/IRQ mutex
 * held, a pinned ARMED S1 and all three actual GIC lines published. Sources
 * and DMA/CMOS/VF/CQ remain off. We port one normal 8080 command, never the
 * vendor's optional duplicate descriptor or software fill-to-two loop.
 * The expected counter DELTA is a runtime acceptance criterion, not an
 * emulated counter: RCNT+1(mod256), CNT=1, unchanged WCNT/DROP/reserved.
 * Both banks and cleared command control must be observed twice. If the
 * hardware cannot accept in this state, retain; never enable DMA to hide it.
 */
static int cam_sv_credit_issue_locked(uint64_t epoch,struct x9_camera_camsv_slot *slot)
{
    uint64_t span,end;
    uint32_t control;
    int e;
    if(cam_sv.credit_issued || cam_sv.credit_phase!=X9_CAM_SV_CREDIT_EMPTY)return EBUSY;
    e=cam_sv_request_guard_locked(epoch,slot);if(e)return e;
    lck_mtx_lock(&camera_camsv_lock);e=slot->state==X9_CAMSV_ARMED?0:ESTALE;
    lck_mtx_unlock(&camera_camsv_lock);if(e)return e;
    if(cam_sv.commit_pending || cam_sv.pending_slot<=CAM_SV_FIELDS)return EBUSY;
    control=cam_sv.target[CAM_SV_FBC];
    if((control&CAM_SV_FBC_MASK)!=CAM_SV_FBC_EN)return EINVAL;
    for(unsigned bank=0;bank<2;bank++) {
        uint32_t before=cam_sv.fbc_before[bank];
        if(before&CAM_SV_FBC_COUNT_MASK)return EBUSY;
        cam_sv.status.credit_target[bank]=cam_sv.credit_target[bank]=
            (before&~(CAM_SV_FBC_COUNT_MASK|0xffu))|0x10000u|((before+1)&0xffu);
    }
    e=cam_sv_request_guard_locked(epoch,slot);if(e)return e;
    e=cam_irq_credit_guard_locked(epoch);if(e)return e;
    nanoseconds_to_absolutetime(10000000,&span);end=mach_absolute_time()+span;
    /* Journal BEFORE the single store. Once issued, failure is terminal and
     * leaves the SAME lease/RAW/controller/power pinned, with no cancellation.
     * RCNT is not put in geometry dirty/pending and no DB pulse follows it. */
    cam_sv.credit_issued=true;cam_sv.credit_phase=X9_CAM_SV_CREDIT_PENDING;
    cam_sv.credit_seen=cam_sv.credit_ack=cam_sv.credit_samples=0;
    cam_sv_status();cam_rw(cam_sv.view[0],0x540,control|CAM_SV_FBC_RCNT);
    e=EAGAIN;
    for(unsigned poll=0;poll<10000;poll++) {
        bool target=true;
        if(mach_absolute_time()>=end){e=ETIMEDOUT;break;}
        e=cam_sv_request_guard_locked(epoch,slot);if(e)break;
        e=cam_irq_credit_guard_locked(epoch);if(e)break;
        for(unsigned bank=0;bank<2 && !e;bank++) {
            uint32_t count,actual;
            e=cam_sv_power_guard();if(e)break;
            count=cam_rr(cam_sv.view[bank],0x544);
            actual=cam_rr(cam_sv.view[bank],0x540);
            cam_sv.status.fbc_after[bank]=count;cam_sv.status.credit_control[bank]=actual;
            if(count==cam_sv.credit_target[bank])cam_sv.credit_seen|=1u<<bank;
            else if((cam_sv.credit_seen&(1u<<bank)) || count!=cam_sv.fbc_before[bank])e=ESTALE;
            if(actual!=control && cam_sv_fbc_compare_word(bank,actual)!=control)e=ESTALE;
            if(!e && count==cam_sv.credit_target[bank] && actual==control)
                cam_sv.credit_ack|=1u<<bank;
            else target=false;
        }
        if(e)break;
        if(mach_absolute_time()>=end){e=ETIMEDOUT;break;}
        cam_sv.credit_samples=target?cam_sv.credit_samples+1:0;
        if(cam_sv.credit_samples==2) {
            cam_sv.credit_phase=X9_CAM_SV_CREDIT_ACCEPTED;
            e=cam_sv_request_guard_locked(epoch,slot);
            if(!e)e=cam_irq_credit_guard_locked(epoch);
            break;
        }
        e=EAGAIN;delay(1);
    }
    if(e) {
        if(e==EAGAIN)e=ETIMEDOUT;
        cam_sv.terminal=true;cam_sv_fault(e);
    }
    cam_sv_status();
    printf("X9 CAMERA FBC CREDIT: sensor=%u epoch=%llu capture_id=%llu phase=%u "
        "issued=%u seen=%x ack=%x samples=%u errno=%d control=%x/%x count=%x/%x "
        "target=%x/%x (one actual RCNT, no source/DMA/frame or AXI drain)\n",
        cam_sv.status.sensor,epoch,cam_sv.request.frame.capture_id,cam_sv.credit_phase,
        cam_sv.credit_issued,cam_sv.credit_seen,cam_sv.credit_ack,cam_sv.credit_samples,e,
        cam_sv.status.credit_control[0],cam_sv.status.credit_control[1],
        cam_sv.status.fbc_after[0],cam_sv.status.fbc_after[1],cam_sv.credit_target[0],cam_sv.credit_target[1]);
    return e;
}
#include "x9_camera_sv_gate.inc"
/* Actual post-credit receive arm, before CMOS/VF arm on start and after
 * actual TG stop on close. PHY/OUTMUX DL and sensor source remain OFF.
 * Four single ordinary RW stores, NOT CQ/DB/RCNT commands. This
 * does not turn an unqualified DMA lease or GIC topology into native authority.
 * off is a private control action, not a caller-supplied drained assertion:
 * only the same inactive receive arm can be quiesced here. Accepted credit,
 * backing and history are preserved, so this NEVER authorizes RAW reuse,
 * unmap, dependency release or normal producer retirement after a frame. */
static int cam_sv_receive_transition_locked(uint64_t epoch,bool off)
{
    uint64_t span,end;
    int e;
    if(!cam_sv.held || cam_sv.epoch!=epoch || !cam_sv.request_held ||
       !cam_sv.request_ready || !cam_sv.fbc_ready ||
       cam_sv.credit_phase!=X9_CAM_SV_CREDIT_ACCEPTED)return ESTALE;
    if(cam_sv.receive_phase!=(off?X9_CAM_SV_RECEIVE_ARMED:X9_CAM_SV_RECEIVE_IDLE))return EBUSY;
    if(off && !cam_sv_gate_off_ready())return EBUSY;
    if(cam_sv.commit_pending || cam_sv.pending_slot<=CAM_SV_FIELDS)return EBUSY;
    e=cam_sv_request_guard_locked(epoch,cam_sv.request_slot);if(e)return e;
    e=cam_irq_credit_guard_locked(epoch);if(e)return e;
    lck_mtx_lock(&camera_camsv_lock);
    e=cam_sv.request_slot->state==X9_CAMSV_ARMED?0:ESTALE;
    lck_mtx_unlock(&camera_camsv_lock);if(e)return e;
    for(unsigned i=0;i<X9_CAM_SV_RECEIVE_FIELDS;i++) {
        unsigned reg=cam_sv_find(cam_sv_receive_fields[i].offset);
        uint32_t expected=off?cam_sv_receive_fields[i].target:0;
        if(reg>=cam_sv.count || cam_sv.status.receive_before[i] ||
           cam_sv.expected[0][reg]!=expected || cam_sv.expected[1][reg]!=expected)return ESTALE;
    }
    nanoseconds_to_absolutetime(10000000,&span);end=mach_absolute_time()+span;
    cam_sv.receive_phase=off?X9_CAM_SV_RECEIVE_QUIESCING:X9_CAM_SV_RECEIVE_PENDING;
    /* DONE -> ERR -> SOF -> DMA, matching the normal disable writer's
     * ordering for this subset. CHANNEL/COMMON remain off; no history clear.
     * On arm, all source enables are accepted BEFORE the IMG0A DMA enable. */
    for(unsigned i=0;i<X9_CAM_SV_RECEIVE_FIELDS;i++) {
        uint32_t bit=1u<<i,next=off?0:cam_sv_receive_fields[i].target;
        bool complete=false;
        if(mach_absolute_time()>=end){e=ETIMEDOUT;break;}
        e=cam_sv_request_guard_locked(epoch,cam_sv.request_slot);if(e)break;
        e=cam_irq_credit_guard_locked(epoch);if(e)break;
        if(off) {
            if(cam_sv.receive_off_issued&bit){e=ESTALE;break;}
            cam_sv.receive_off_issued|=bit;
        } else {
            if(cam_sv.receive_issued&bit){e=ESTALE;break;}
            cam_sv.receive_issued|=bit;
        }
        cam_sv.receive_samples=0;cam_sv_status();
        cam_rw(cam_sv.view[0],cam_sv_receive_fields[i].offset,next);
        for(unsigned poll=0;poll<10000;poll++) {
            bool match=true;
            if(mach_absolute_time()>=end){e=ETIMEDOUT;break;}
            e=cam_sv_request_guard_locked(epoch,cam_sv.request_slot);if(e)break;
            e=cam_irq_credit_guard_locked(epoch);if(e)break;
            for(unsigned bank=0;bank<2;bank++)
                if(cam_sv.status.receive_after[bank][i]!=next)match=false;
            if(mach_absolute_time()>=end){e=ETIMEDOUT;break;}
            cam_sv.receive_samples=match?cam_sv.receive_samples+1:0;
            if(cam_sv.receive_samples==2){complete=true;break;}
            delay(1);
        }
        if(e)break;
        if(!complete){e=ETIMEDOUT;break;}
    }
    if(!e) {
        cam_sv.receive_phase=off?X9_CAM_SV_RECEIVE_QUIESCENT:X9_CAM_SV_RECEIVE_ARMED;
        e=cam_sv_request_guard_locked(epoch,cam_sv.request_slot);
        if(!e)e=cam_irq_credit_guard_locked(epoch);
        if(!e && mach_absolute_time()>=end)e=ETIMEDOUT;
    }
    if(e){cam_sv.terminal=true;cam_sv_fault(e);}
    cam_sv_status();
    printf("X9 CAMERA RECEIVE ARM: epoch=%llu capture_id=%llu off=%u phase=%u issued=%x/%x "
        "seen=%x/%x offseen=%x/%x samples=%u errno=%d "
        "outer=%x/%x/%x/%x inner=%x/%x/%x/%x (CMOS/VF/PHY/sensor off; NOT frame/AXI/map drain)\n",
        epoch,cam_sv.request.frame.capture_id,off,cam_sv.receive_phase,
        cam_sv.receive_issued,cam_sv.receive_off_issued,cam_sv.receive_seen[0],cam_sv.receive_seen[1],
        cam_sv.receive_off_seen[0],cam_sv.receive_off_seen[1],cam_sv.receive_samples,e,
        cam_sv.status.receive_after[0][0],cam_sv.status.receive_after[0][1],
        cam_sv.status.receive_after[0][2],cam_sv.status.receive_after[0][3],
        cam_sv.status.receive_after[1][0],cam_sv.status.receive_after[1][1],
        cam_sv.status.receive_after[1][2],cam_sv.status.receive_after[1][3]);
    return e;
}
static int cam_sv_receive_quiesce_locked(uint64_t epoch)
{
    if(cam_sv.receive_phase==X9_CAM_SV_RECEIVE_IDLE)return 0;
    /* Do not finish/cancel a partial failed arm on close. A late target is
     * not permission for retries or to erase the original error evidence. */
    if(cam_sv.receive_phase!=X9_CAM_SV_RECEIVE_ARMED)return cam_sv.error?cam_sv.error:ESTALE;
    return cam_sv_receive_transition_locked(epoch,true);
}
static int cam_sv_close_locked(uint64_t epoch)
{
    uint64_t span,deadline;
    uint32_t db;
    int e;
    if(!cam_sv.held)return 0;
    if(cam_sv.epoch!=epoch || cam_sv.close_attempted)return ESTALE;
    /* No producer retirement exists yet. FBC_EN off/reset is NOT a way to
     * revoke an accepted/pending credit or justify dropping its backing. */
    if(cam_sv.credit_issued)return cam_sv_fault(EBUSY);
    cam_sv.close_attempted=true;
    cam_sv.request_ready=cam_sv.fbc_ready=false;
    nanoseconds_to_absolutetime(10000000,&span);deadline=mach_absolute_time()+span;
    e=cam_sv_environment();
    if(!e)e=cam_sv.commit_pending?cam_sv_commit():cam_sv_pending_wait(deadline);
    /* A shadow-only failure may have live-through effects even without
     * a DB command. Consume its exact partial target once before returning
     * those fields to prior; never pretend an earlier posted effect was
     * cancelled merely because inner still contains the original value. */
    if(!e && cam_sv.dirty && !cam_sv.command)e=cam_sv_commit();
    /* Consume FBC's original control in BOTH banks before changing any
     * address/cookie, not merely post its outer disable before those writes.
     * Source/DMA/CMOS/VF are still off; this is not a producer-drain path. */
    if(!e && (cam_sv.dirty&(1u<<CAM_SV_FBC))) {
        uint32_t prior=cam_sv.before[0][cam_sv.field[CAM_SV_FBC]];
        e=cam_sv_field_write(CAM_SV_FBC,prior);
        if(!e)e=cam_sv_commit();
        if(!e)for(unsigned bank=0;bank<2;bank++)
            if(cam_sv.expected[bank][cam_sv.field[CAM_SV_FBC]]!=prior)e=cam_sv_fault(ESTALE);
    }
    for(unsigned i=CAM_SV_FBC;i && !e;i--) {
        unsigned slot=i-1;
        if(cam_sv.dirty&(1u<<slot))e=cam_sv_field_write(slot,cam_sv.before[0][cam_sv.field[slot]]);
    }
    if(!e)e=cam_sv.dirty?cam_sv_commit():cam_sv_target_observe();
    /* FBC is the last geometry/request field and is restored FIRST, before FH/address and
     * geometry; the original no-credit counter evidence stays pinned through
     * both-bank restore and final policy. Never invoke RCNT or RESET cleanup.
     * Return policy only AFTER complete original geometry/address/controls
     * are actually back in both banks. This inactive journal does not stop
     * a producer; IRQ/RAW retirement must precede it in receiver close. */
    if(!e) {
        db=cam_sv.before[0][cam_sv.db_reg];cam_sv.db_phase=CAM_SV_DB_POLICY;cam_sv.db_issued=false;
        deadline=mach_absolute_time()+span;
        e=cam_sv_post(CAM_SV_FIELDS,db);
        if(e==EIO && cam_sv.pending_slot==CAM_SV_FIELDS)e=cam_sv_pending_wait(deadline);
        if(!e)e=cam_sv_target_wait(deadline);
    }
    if(!e)e=cam_sv_environment();
    if(!e) {
        for(unsigned bank=0;bank<2;bank++)if(memcmp(cam_sv.expected[bank],cam_sv.before[bank],
            cam_sv.count*sizeof(uint32_t)))e=cam_sv_fault(ESTALE);
    }
    if(!e) {
        cam_sv.held=false;cam_sv.dirty=0;cam_sv.db_phase=CAM_SV_DB_IDLE;cam_sv.db_issued=false;
        cam_sv.request_ready=cam_sv.request_held=false;cam_sv.request_slot=NULL;
        cam_sv.fbc_sampled=cam_sv.fbc_ready=false;
        cam_sv.status.flags|=X9_CAM_SV_HW_RESTORED;
    } else cam_sv_fault(e);
    cam_sv.status.restore_errno=e;cam_sv_status();
    printf("X9 CAMERA CAMSV CLOSE: sensor=%u epoch=%llu flags=%x errno=%u restore=%d "
        "dirty=%x pending=%u commit=%u dbphase=%u sequence=%llu/%llu terminal=%u "
        "DB=%x/%x (no-credit FBC/RAW10 geometry/address/FH restore, NOT AXI/map drain)\n",
        cam_sv.status.sensor,epoch,cam_sv.status.flags,cam_sv.status.last_errno,e,cam_sv.status.dirty,
        cam_sv.status.pending,cam_sv.status.commit_pending,cam_sv.status.db_phase,cam_sv.status.command,
        cam_sv.status.completed,cam_sv.status.terminal,cam_sv.status.db_after[0],cam_sv.status.db_after[1]);
    return e;
}
