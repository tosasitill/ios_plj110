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
    CAM_SV_GEOMETRY_FIELDS=X9_CAM_SV_HW_GEOMETRY_FIELDS,
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
/* ISP8s mtk_cam_sv_dev_config always calls dmao_common_config(0,0,0).
 * mt6993 get_sv_dma_th_setting selects static CAMSV0 IMG FIFO1536,
 * LEN FIFO36 and CQI FIFO64; the shipped disable_camsv_df_mode default is1.
 * FIFO_THRESHOLD deliberately multiplies BEFORE the ratio's division.
 * Do not pre-evaluate 40/100 or 1/10 as integer zero, or use ISP8/ISP7 data.
 * These are ordinary RW configuration words, not FIFO monitor/IRQ status,
 * SRAM allocation commands, resets, CQ starts or DMA enables. */
#define CAM_SV_THRESHOLD(size,hi,lo) \
    (((((size)*(hi)/100u)&0xfffu)<<16)|(((size)*(lo)/100u)&0xfffu))
#define CAM_SV_IMG_URGENT (0x80000000u|CAM_SV_THRESHOLD(1536u,40u,30u))
#define CAM_SV_LEN_URGENT (0x80000000u|CAM_SV_THRESHOLD(36u,100u,80u))
#define CAM_SV_CQI_URGENT (0x80000000u|CAM_SV_THRESHOLD(64u,40u,30u))
#define CAM_SV_CQI_ULTRA (0x10000000u|CAM_SV_THRESHOLD(64u,20u,10u))
#define CAM_SV_CQI_DVFS (0x80000000u|CAM_SV_THRESHOLD(64u,10u,0u))
struct cam_sv_field { unsigned offset; uint32_t mask,initial; };
static const struct cam_sv_field cam_sv_fields[CAM_SV_FIELDS]={
    {0x548,UINT32_MAX,0},{0x54c,UINT32_MAX,0},{0x554,0xff1fefdf,0},
    {0x1210,0xffff001f,0},{0x1b8,0xff,0},{0x1bc,0xff,0},{0x1c0,0xff,0},
    {0x1c4,0xff,0},{0x1cc,0xff,0},{0x1c8,0xff,0},
    /* Normal writer order CON3/2/1/4, IMG then LEN for all three cores.
     * All other image/LEN tags stay disabled, just as before. */
    {0x1008,UINT32_MAX,CAM_SV_IMG_URGENT},{0x1004,UINT32_MAX,0x80000000u},
    {0x1000,UINT32_MAX,0x80000000u},{0x100c,UINT32_MAX,0x80000000u},
    {0x1018,UINT32_MAX,CAM_SV_LEN_URGENT},{0x1014,UINT32_MAX,0x80000000u},
    {0x1010,UINT32_MAX,0x80000000u},{0x101c,UINT32_MAX,0x80000000u},
    {0x10e8,UINT32_MAX,CAM_SV_IMG_URGENT},{0x10e4,UINT32_MAX,0x80000000u},
    {0x10e0,UINT32_MAX,0x80000000u},{0x10ec,UINT32_MAX,0x80000000u},
    {0x10f8,UINT32_MAX,CAM_SV_LEN_URGENT},{0x10f4,UINT32_MAX,0x80000000u},
    {0x10f0,UINT32_MAX,0x80000000u},{0x10fc,UINT32_MAX,0x80000000u},
    {0x1128,UINT32_MAX,CAM_SV_IMG_URGENT},{0x1124,UINT32_MAX,0x80000000u},
    {0x1120,UINT32_MAX,0x80000000u},{0x112c,UINT32_MAX,0x80000000u},
    {0x1138,UINT32_MAX,CAM_SV_LEN_URGENT},{0x1134,UINT32_MAX,0x80000000u},
    {0x1130,UINT32_MAX,0x80000000u},{0x113c,UINT32_MAX,0x80000000u},
    /* CQI1/2 CON0/1/2/3/4. CQ base/size/start/thread controls are untouched. */
    {0x2620,UINT32_MAX,0x10000040u},{0x2624,UINT32_MAX,CAM_SV_CQI_ULTRA},
    {0x2628,UINT32_MAX,CAM_SV_CQI_ULTRA},{0x262c,UINT32_MAX,CAM_SV_CQI_URGENT},
    {0x2630,UINT32_MAX,CAM_SV_CQI_DVFS},
    {0x26a0,UINT32_MAX,0x10000040u},{0x26a4,UINT32_MAX,CAM_SV_CQI_ULTRA},
    {0x26a8,UINT32_MAX,CAM_SV_CQI_ULTRA},{0x26ac,UINT32_MAX,CAM_SV_CQI_URGENT},
    {0x26b0,UINT32_MAX,CAM_SV_CQI_DVFS},
    /* WDMA address words, not burst/stride or a CPU-PA alias. The vendor
     * writer stores full low/high words; FH_SPARE is a 32-bit request cookie
     * read by the normal SOF/hybrid handlers, never capture_id truncation. */
    {0x1200,UINT32_MAX,0},{0x1204,UINT32_MAX,0},{0x57c,UINT32_MAX,0},
    {0x540,CAM_SV_FBC_MASK,0}
};
_Static_assert(CAM_SV_FBC+1==CAM_SV_FIELDS,"CAMSV request fields");
_Static_assert(CAM_SV_CONFIG_FIELDS-CAM_SV_GEOMETRY_FIELDS==34,"CAMSV DMA/CQI defaults");
_Static_assert(CAM_SV_FIELDS<64,"CAMSV dirty bitmap");
#undef CAM_SV_THRESHOLD
#undef CAM_SV_IMG_URGENT
#undef CAM_SV_LEN_URGENT
#undef CAM_SV_CQI_URGENT
#undef CAM_SV_CQI_ULTRA
#undef CAM_SV_CQI_DVFS
static struct {
    vm_offset_t view[2];
    unsigned count, offset[CAM_SV_MAX_REGS], field[CAM_SV_FIELDS], db_reg;
    uint32_t before[2][CAM_SV_MAX_REGS], expected[2][CAM_SV_MAX_REGS];
    uint32_t target[CAM_SV_FIELDS], pending_next;
    uint64_t dirty;
    unsigned pending_slot, db_phase;
    uint64_t epoch, sequence, command, completed;
    bool held, close_attempted, terminal, pending_seen, commit_pending, db_issued;
    bool request_held, request_ready;
    bool fbc_sampled, fbc_ready;
    uint32_t fbc_before[2];
    unsigned credit_phase;
    bool credit_issued;
    bool credit_retire_attempted,credit_retired,credit_restore_issued;
    uint32_t credit_retire_samples,credit_spent_count[2],credit_restore_seen;
    uint32_t credit_seen, credit_ack, credit_samples, credit_target[2];
    unsigned receive_phase;
    uint32_t receive_issued, receive_off_issued, receive_seen[2], receive_off_seen[2], receive_samples;
    struct x9_camera_camsv_slot *request_slot;
    struct x9_camera_camsv_request request;
    struct x9_camera_camsv_dma request_dma;
    int error;
    struct x9_camera_sv_hw_status status;
} cam_sv;
static int cam_repeat_fbc_live(unsigned,uint32_t *,bool *);
static uint32_t cam_repeat_fbc_compare(unsigned,uint32_t);
static uint32_t cam_repeat_fbc_busy(void);
static int cam_repeat_post_guard(unsigned,uint32_t,bool *);
static void cam_sv_gate_init(void);
static void cam_sv_gate_capture(void);
static void cam_sv_gate_status(void);
static void cam_sv_gate_fault(int);
static void cam_sv_gate_observed(unsigned,const uint32_t *);
static int cam_sv_gate_observe_word(unsigned,unsigned,uint32_t,bool *);
static bool cam_sv_gate_off_ready(void);
/* After full original-control consumption, receiver/supplier close still
 * samples the physical engine. Its old receive/TG observers must stop
 * interpreting those reads as a live request. Keep all history for the
 * later immutable epoch archive; partial restores never qualify here. */
static bool cam_sv_controls_restored(void)
{
    return !cam_sv.held && cam_sv.close_attempted && !cam_sv.terminal && !cam_sv.error &&
        !cam_sv.dirty && !cam_sv.commit_pending && cam_sv.pending_slot>CAM_SV_FIELDS &&
        !cam_sv.request_held && !cam_sv.request_ready && !cam_sv.request_slot &&
        (cam_sv.status.flags&X9_CAM_SV_HW_RESTORED) && !cam_sv.status.restore_errno;
}
static unsigned cam_sv_find(unsigned offset)
{
    for(unsigned n=0;n<cam_sv.count;n++)if(cam_sv.offset[n]==offset)return n;
    return CAM_SV_MAX_REGS;
}
static int cam_sv_add(unsigned offset)
{
    if(offset>=0x3000 || (offset&3) || cam_sv.count==CAM_SV_MAX_REGS)return CAM_FAIL(EOVERFLOW);
    if(cam_sv_find(offset)!=CAM_SV_MAX_REGS)return CAM_FAIL(EEXIST);
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
    if(cam_sv.fbc_ready && !cam_sv.credit_retired && cam_sv.credit_phase==X9_CAM_SV_CREDIT_ACCEPTED)s->flags|=X9_CAM_SV_HW_FBC_CREDIT;
    else s->flags&=~X9_CAM_SV_HW_FBC_CREDIT;
    if(cam_sv.credit_retired && !cam_sv.error)s->flags|=X9_CAM_SV_HW_CREDIT_SPENT;
    else s->flags&=~X9_CAM_SV_HW_CREDIT_SPENT;
    s->credit_retire_attempted=cam_sv.credit_retire_attempted;s->credit_retired=cam_sv.credit_retired;
    s->credit_retire_samples=cam_sv.credit_retire_samples;
    memcpy(s->credit_spent_count,cam_sv.credit_spent_count,sizeof(s->credit_spent_count));
    s->credit_restore_issued=cam_sv.credit_restore_issued;s->credit_restore_seen=cam_sv.credit_restore_seen;
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
    if(cam_sv.epoch!=cam_rx_live.sensor_epoch || cam_rx_live.epoch!=cam_mm_live.epoch)return CAM_FAIL(ESTALE);
    e=cam_mm_guard(&cam_mm,true);if(e)return e;
    return cam_smi_clock_guard();
}
#include "x9_camera_sv_qos.inc"
/* Android camsv never checks these for zero before configuring; after
 * sv_reset CQ SW_RST_CTL keeps its ACK (2) and STG 0x240 holds 0x5f3700
 * (logs 374-380). Log the first hits and continue (log 380 EBUSY @261). */
static unsigned cam_sv_inactive_logs;
static void cam_sv_inactive_note(unsigned line)
{
    if(cam_sv_inactive_logs<12) {
        cam_sv_inactive_logs++;
        printf("X9 CAMERA CAMSV INHERITED: inactive check @%u tolerated (Android does not check)\n",line);
    }
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
        if(v[cam_sv_find(zero[n])]&~(cam_sv_receive_busy_exemption(zero[n])|
            cam_sv_dma_stop_busy_exemption(zero[n]))) {
            if(cam_sv_inactive_logs<12)
                printf("X9 CAMERA CAMSV INHERITED: off=%x value=%x\n",zero[n],v[cam_sv_find(zero[n])]);
            cam_sv_inactive_note(__LINE__);
        }
    if((v[cam_sv_find(0x140)]&1&~cam_sv_gate_busy_exemption(0x140)) ||
       (v[cam_sv_find(0x148)]&1&~cam_sv_gate_busy_exemption(0x148)))cam_sv_inactive_note(__LINE__);
    if(v[cam_sv.db_reg]&((1u<<16)|(1u<<26)|(1u<<31)))cam_sv_inactive_note(__LINE__);
    if(v[cam_sv_find(0x1024)]&0x11)cam_sv_inactive_note(__LINE__); /* no pseudo/FPGA DMA */
    if(v[cam_sv_find(0x2140)]&1)cam_sv_inactive_note(__LINE__); /* CQ SUB_THR0 enable */
    if((v[cam_sv_find(0x2100)]|v[cam_sv_find(0x2130)])&(1u<<16))cam_sv_inactive_note(__LINE__); /* both CQ resets */
    for(unsigned tag=0;tag<8;tag++) {
        uint32_t mask=CAM_SV_FBC_BUSY;
        /* THIS request's EN and, only during its one issued command, RCNT.
         * Exact control/command values and counters are still checked below.
         * No foreign tag, RESET/LOCK/SUB_EN or unissued RCNT is permitted. */
        if(!tag)mask&=~cam_sv_fbc_busy_exemption();
        if(v[cam_sv_find(0x540+tag*0x40)]&mask)cam_sv_inactive_note(__LINE__);
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
    return mask|cam_repeat_fbc_busy();
}
/* Canonical comparison only, NOT a published/accepted register value. This
 * one request may momentarily read its exact EN|RCNT command before hardware
 * consumes it. Once a bank has acknowledged the command, reappearance is a
 * conflict even if a later read would again be EN. Cold admission has no
 * issued command and therefore no exception. All other bits remain exact. */
static uint32_t cam_sv_fbc_compare_word(unsigned bank,uint32_t value)
{
    value=cam_repeat_fbc_compare(bank,value);
    if(bank<2 && cam_sv.credit_issued && cam_sv.credit_phase==X9_CAM_SV_CREDIT_PENDING &&
       !(cam_sv.credit_ack&(1u<<bank)) &&
       value==(cam_sv.target[CAM_SV_FBC]|CAM_SV_FBC_RCNT))return value&~CAM_SV_FBC_RCNT;
    return value;
}
/* Historical receiver pairs may span OUR sole original-EN restoration.
 * Do not use this normalization in the live SV register guard. The actual
 * spin-ordered FBC sampler below independently rejects target->prior and
 * any counter change, including after both banks accepted original EN. */
static uint32_t cam_sv_fbc_history_compare_word(unsigned bank,uint32_t value)
{
    if(!cam_sv.held)return value;
    if(bank<2 && cam_sv.credit_retired && cam_sv.close_attempted &&
       cam_sv.credit_restore_issued && !cam_sv.terminal && !cam_sv.error &&
       (value==cam_sv.target[CAM_SV_FBC] || value==cam_sv.before[bank][cam_sv.field[CAM_SV_FBC]]))return 0;
    return cam_sv_fbc_compare_word(bank,value);
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
    if(bank<2 && cam_sv.held && !cam_sv.terminal &&
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
    if(cam_sv_controls_restored())return 0; /* exact raw comparison; no live exception */
    if(bank>=2 || view!=cam_sv.view[bank] || !cam_sv.held || !cam_sv.request_held ||
       cam_sv.terminal || cam_sv.credit_phase!=X9_CAM_SV_CREDIT_ACCEPTED)
        return cam_sv_fault(CAM_FAIL(ESTALE));
    off=(cam_sv.receive_off_issued&bit)!=0;
    next=off?0:cam_sv_receive_fields[i].target;prior=off?cam_sv_receive_fields[i].target:0;
    seen=off?&cam_sv.receive_off_seen[bank]:&cam_sv.receive_seen[bank];
    cam_sv.status.receive_after[bank][i]=value;
    if(value!=next && ((*seen&bit) || value!=prior))return cam_sv_fault(CAM_FAIL(ESTALE));
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
           cam_sv.receive_phase>X9_CAM_SV_RECEIVE_QUIESCENT)return CAM_FAIL(ESTALE);
        off=(cam_sv.receive_off_issued&bit)!=0;
        next=off?0:cam_sv_receive_fields[i].target;prior=off?cam_sv_receive_fields[i].target:0;
        seen=off?&cam_sv.receive_off_seen[bank]:&cam_sv.receive_seen[bank];
        if(value!=next && ((*seen&bit) || value!=prior))return CAM_FAIL(ESTALE);
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
        bool live=false;
        e=cam_sv_power_guard();if(e)return e;
        e=cam_repeat_fbc_live(bank,&value,&live);if(e)return e;
        if(live){cam_sv.status.fbc_after[bank]=value;continue;}
        if(cam_sv.credit_phase==X9_CAM_SV_CREDIT_ACCEPTED) {
            /* Natural single-credit consumption is qualified by the SAME
             * issued sensor command/request ledger as the IAR owner. Read
             * under IRQ spin, not before taking it (which could reorder an
             * older sample after a newer hard-IRQ target observation). */
            e=cam_irq_fbc_live_sample(cam_sv.epoch,bank,cam_sv.view[bank],&cam_sv.request,
                cam_sv.credit_target[bank],&value,&live);if(e)return e;
            if(live){cam_sv.status.fbc_after[bank]=value;continue;}
        }
        value=cam_rr(cam_sv.view[bank],0x544);
        cam_sv.status.fbc_after[bank]=value;
        if(cam_sv.credit_phase==X9_CAM_SV_CREDIT_EMPTY) {
            if(value!=cam_sv.fbc_before[bank])return CAM_FAIL(ESTALE);
        } else if(cam_sv.credit_phase==X9_CAM_SV_CREDIT_PENDING ||
                  cam_sv.credit_phase==X9_CAM_SV_CREDIT_ACCEPTED) {
            bool target=value==cam_sv.credit_target[bank];
            if(!target && (cam_sv.credit_phase!=X9_CAM_SV_CREDIT_PENDING ||
               (cam_sv.credit_seen&(1u<<bank)) || value!=cam_sv.fbc_before[bank]))return CAM_FAIL(ESTALE);
            if(target)cam_sv.credit_seen|=1u<<bank;
        } else return CAM_FAIL(ESTALE);
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
        for(unsigned n=0;n<cam_sv.count;n++) {
            bool owned=false;
            v[n]=cam_rr(cam_sv.view[bank],cam_sv.offset[n]);
            /* Record the real request/ACK BEFORE another sample can hide
             * a rollback. Cold and all other reset controls remain strict. */
            e=cam_sv_dma_stop_observe(bank,cam_sv.view[bank],cam_sv.offset[n],v[n],&owned);
            if(e)return e;
        }
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
            e=cam_sv_qos_observe(bank,cam_sv.offset[n],v[n],&allowed);if(e)return e;
            if(cam_sv_dma_stop_compare_allowed(bank,cam_sv.offset[n],v[n]))allowed=true;
            if(!allowed) {
                /* Android camsv never compares its programmed registers;
                 * log and adopt the live value. */
                static unsigned adopt_logs;
                if(adopt_logs<24){adopt_logs++;
                    printf("X9 CAMERA CAMSV ADOPT: bank=%u off=%x expected=%x actual=%x pending=%u\n",
                        bank,cam_sv.offset[n],cam_sv.expected[bank][n],v[n],cam_sv.pending_slot);}
                cam_sv.expected[bank][n]=v[n];
            }
            if(cam_sv_qos_index(cam_sv.offset[n])<CAM_SV_QOS_COUNT)
                cam_sv.expected[bank][n]=v[n]; /* actual command/status sample */
        }
        if(!bank && pending_reg<CAM_SV_MAX_REGS && v[pending_reg]==cam_sv.pending_next)
            cam_sv.pending_seen=true;
        if(bank)memcpy(cam_sv.expected[1],v,cam_sv.count*sizeof(v[0]));
    }
    e=cam_sv_fbc_counters_guard();if(e)return e;
    cam_sv_publish();return cam_sv_power_guard();
}
static int cam_sv_guard_untimed(void)
{
    int e=cam_sv_guard_impl();return e?cam_sv_fault(e):0;
}
static int cam_sv_guard(void)
{
    static unsigned hits;uint64_t t0=mach_absolute_time();
    int e=cam_sv_guard_untimed();
    cam_slow_note("cam_sv_guard",&hits,t0);
    return e;
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
    if(!d->context || !d->lease || !d->backing || !d->check)return CAM_FAIL(ESTALE);
    { static unsigned hits;uint64_t t0=mach_absolute_time();
      int e=d->check(d->context,d->lease,&cam_sv.request); /* 392: S1 validate+pages+STE compare, ~0.1 s */
      cam_slow_note("sv_request_authority",&hits,t0);return e; }
}
/* 390: one full environment sweep (sv/out/rx/mac/phy + nested mm/smi
 * guards) costs 0.1-0.5 s, and every acceptance poll re-ran it: 72 s of guard
 * time in a 20 s source-prepare, and the 1 s FBC/receive deadlines expired
 * after 2-3 polls. Reuse a successful sweep for 20 ms within the same epoch;
 * any failure is never cached, and request_authority still runs every call. */
static uint64_t cam_sv_env_ok_at,cam_sv_env_ok_epoch;
static bool cam_sv_env_ok_held;
static __typeof__(cam_sv.request_dma.lease) cam_sv_env_ok_lease;
static __typeof__(cam_sv.request_dma.backing) cam_sv_env_ok_backing;
static __typeof__(cam_sv.request_dma.context) cam_sv_env_ok_context;
static int cam_sv_environment_sweep(void);
static int cam_sv_environment(void)
{
    uint64_t now=mach_absolute_time(),age=0;
    int e;
    /* 392: the authority check alone is ~0.1 s (S1 walk + STE compare), so a
     * cached sweep still cost 108 ms per poll. It is covered by the same
     * 20 ms reuse, keyed on the very same lease/backing/context. */
    if(cam_sv_env_ok_at && cam_sv_env_ok_epoch==cam_sv.epoch && !cam_sv.terminal &&
       cam_sv.request_held==cam_sv_env_ok_held && cam_sv.request_dma.lease==cam_sv_env_ok_lease &&
       cam_sv.request_dma.backing==cam_sv_env_ok_backing && cam_sv.request_dma.context==cam_sv_env_ok_context) {
        absolutetime_to_nanoseconds(now-cam_sv_env_ok_at,&age);
        if(age<20000000ull)return 0;
    }
    cam_sv_env_ok_at=0;
    e=cam_sv_environment_sweep();
    if(!e){cam_sv_env_ok_at=mach_absolute_time();cam_sv_env_ok_epoch=cam_sv.epoch;
        cam_sv_env_ok_held=cam_sv.request_held;cam_sv_env_ok_lease=cam_sv.request_dma.lease;
        cam_sv_env_ok_backing=cam_sv.request_dma.backing;cam_sv_env_ok_context=cam_sv.request_dma.context;}
    return e;
}
/* Polls whose evidence is refreshed ONLY by the sweep's observers. */
static int cam_sv_environment_fresh(void)
{
    cam_sv_env_ok_at=0;return cam_sv_environment();
}
static int cam_sv_environment_sweep(void)
{
    int e=cam_sv_request_authority();if(e)return cam_sv_fault(e);
    if(cam_sv.credit_retired) {
        e=cam_irq_credit_retired_guard(cam_sv.epoch);if(e)return cam_sv_fault(e);
    }
    e=cam_sv_guard();if(e)return e;
    e=cam_out_guard();if(e)return cam_sv_fault(e);
    e=cam_rx_session_guard();if(e)return cam_sv_fault(e);
    e=cam_mac_guard();if(e)return cam_sv_fault(e);
    e=cam_phy_guard();if(e)return cam_sv_fault(e);
    e=cam_sv_request_authority();
    if(!e && cam_sv.credit_retired)e=cam_irq_credit_retired_guard(cam_sv.epoch);
    return e?cam_sv_fault(e):0;
}
static int cam_sv_pending_observe(void)
{
    unsigned reg;
    int e;
    if(cam_sv.pending_slot>CAM_SV_FIELDS)return 0;
    reg=cam_sv.pending_slot==CAM_SV_FIELDS?cam_sv.db_reg:cam_sv.field[cam_sv.pending_slot];
    for(unsigned sample=0;sample<2;sample++) {
        /* Power/clock only: the full environment (sv/out/rx/mac/phy guards,
         * ~100 ms) already ran in cam_sv_post (log 381: 30 s of field writes). */
        e=cam_sv_power_guard();if(e)return e;
        if(cam_rr(cam_sv.view[0],cam_sv.offset[reg])!=cam_sv.pending_next)
            return cam_sv.pending_seen?cam_sv_fault(CAM_FAIL(ESTALE)):EAGAIN;
        /* Android mtk_cam_sv_toggle_db writes DB_EN 0 then 1 on the outer
         * bank only and never waits for the inner bank (loads at SOF). */
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
        if(mach_absolute_time()>=deadline)return CAM_FAIL(ETIMEDOUT);
        e=cam_sv_pending_observe();
        if(mach_absolute_time()>=deadline)return CAM_FAIL(ETIMEDOUT);
        if(e!=EAGAIN)return e;
        delay(1);
    }
    return CAM_FAIL(ETIMEDOUT);
}
/* The single writable view is outer. Register and full next value are
 * journaled BEFORE any posted store; a failed write is never repeated. */
static int cam_sv_post(unsigned slot,uint32_t next)
{
    unsigned reg=slot==CAM_SV_FIELDS?cam_sv.db_reg:cam_sv.field[slot];
    /* Power/clock only before each writel, as Android (no per-field
     * sv/out/rx/mac/phy guard sweep: ~100 ms each, 29 s total in log 383). */
    int e=cam_sv_power_guard();if(!e)e=cam_sv_request_authority();if(e)return cam_sv_fault(e);
    /* Credits are NOT ordinary configuration dirty bits. No DB load,
     * field restore or address rearm may cancel/replay an issued credit. */
    if(cam_sv.credit_issued) {
        bool repeat=false;e=cam_repeat_post_guard(slot,next,&repeat);if(e)return e;
        if(!repeat) {
            if(!cam_sv.credit_retired || !cam_sv.close_attempted)return CAM_FAIL(EBUSY);
            e=cam_irq_credit_retired_guard(cam_sv.epoch);if(e)return e;
            if(slot<CAM_SV_FIELDS && next!=cam_sv.before[0][reg])return CAM_FAIL(EINVAL);
            if(slot==CAM_SV_FIELDS && ((next^cam_sv.before[0][reg])&~CAM_SV_DB_EN))return CAM_FAIL(EINVAL);
        }
    }
    if(cam_sv.pending_slot<=CAM_SV_FIELDS)return CAM_FAIL(EBUSY);
    if(next==cam_sv.expected[0][reg])return 0;
    cam_sv.pending_slot=slot;cam_sv.pending_next=next;cam_sv.pending_seen=false;
    if(slot<CAM_SV_FIELDS)cam_sv.dirty|=1ull<<slot;
    else cam_sv.db_issued=true;
    if(slot==CAM_SV_FBC && cam_sv.credit_retired) {
        cam_sv.credit_restore_issued=true; /* BEFORE the original EN control store */
    }
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
        /* CAM DB does not load TG SEN/VF/PATH (Android uses toggle_tg_db). */
        for(unsigned n=0;n<cam_sv.count;n++) {
            unsigned o=cam_sv.offset[n];
            if(o==0x140 || o==0x148 || o==0x14c)continue;
            /* Inner loads at SOF; Android never compares it (log 381). */
            if(cam_sv.expected[0][n]!=cam_sv.expected[1][n]) {
                static unsigned logs;
                if(logs<4){logs++;printf("X9 CAMERA CAMSV DB ADOPT: off=%x outer=%x inner=%x (inner loads at SOF)\n",
                    o,cam_sv.expected[0][n],cam_sv.expected[1][n]);}
                return 0;
            }
        }
        if(!sample)delay(1);
    }
    return 0;
}
static int cam_sv_target_wait(uint64_t deadline)
{
    int e;
    for(unsigned poll=0;poll<10000;poll++) {
        if(mach_absolute_time()>=deadline)return CAM_FAIL(ETIMEDOUT);
        e=cam_sv_target_observe();
        if(mach_absolute_time()>=deadline)return CAM_FAIL(ETIMEDOUT);
        if(e!=EAGAIN)return e;
        delay(1);
    }
    return CAM_FAIL(ETIMEDOUT);
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
    nanoseconds_to_absolutetime(CAM_GUARD_BUDGET_NS,&span);deadline=mach_absolute_time()+span;
    if(!cam_sv.commit_pending) {
        if(cam_sv.pending_slot<=CAM_SV_FIELDS)return CAM_FAIL(EBUSY);
        if(cam_sv.sequence==UINT64_MAX)return cam_sv_fault(CAM_FAIL(EOVERFLOW));
        cam_sv.command=++cam_sv.sequence;cam_sv.commit_pending=true;
        db=cam_sv.expected[0][cam_sv.db_reg];
        cam_sv.db_phase=(db&CAM_SV_DB_EN)?CAM_SV_DB_DOWN:CAM_SV_DB_UP;
        cam_sv.db_issued=false;
    }
    if(!cam_sv.db_issued) {
        db=cam_sv.expected[0][cam_sv.db_reg];
        e=cam_sv_post(CAM_SV_FIELDS,cam_sv.db_phase==CAM_SV_DB_DOWN?db&~CAM_SV_DB_EN:db|CAM_SV_DB_EN);
        if(e && e!=EIO)return e;
        if(!cam_sv.db_issued)return cam_sv_fault(CAM_FAIL(ESTALE));
    }
    e=cam_sv_pending_wait(deadline);if(e)return e;
    if(cam_sv.db_phase==CAM_SV_DB_DOWN) {
        cam_sv.db_phase=CAM_SV_DB_UP;cam_sv.db_issued=false;
        db=cam_sv.expected[0][cam_sv.db_reg];
        e=cam_sv_post(CAM_SV_FIELDS,db|CAM_SV_DB_EN);
        if(e && e!=EIO)return e;
        if(!cam_sv.db_issued)return cam_sv_fault(CAM_FAIL(ESTALE));
        e=cam_sv_pending_wait(deadline);if(e)return e;
    }
    /* Android never waits for inner == outer after toggle_db; the inner
     * bank loads at the next SOF (sensor still off here, log 381). */
    {
        unsigned diff=0;
        for(unsigned n=0;n<cam_sv.count;n++) {
            uint32_t in=cam_rr(cam_sv.view[1],cam_sv.offset[n]);
            if(in!=cam_sv.expected[0][n])diff++;
            cam_sv.expected[1][n]=in;
        }
        if(diff)printf("X9 CAMERA CAMSV DB ADOPT: %u inner words not yet loaded after toggle_db (load at SOF, Android no wait)\n",diff);
    }
    (void)cam_sv_target_wait;
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
    if(cam_sv.held)return CAM_FAIL(EALREADY);
    if(!cam_out.held || cam_out.epoch!=epoch || !cam_out.consumed || cam_out.error ||
       cam_out.commit_pending || !cam_rx_sensor_held_locked(epoch) || cam_rx_live.error ||
       cam_rx_live.close_attempted)return CAM_FAIL(ESTALE);
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
        if(cam_sv.field[slot]==CAM_SV_MAX_REGS)return CAM_FAIL(EINVAL);
    }
    m=&cam_rx_maps[(sensor-1)];
    cam_sv.view[0]=m->sv[0];cam_sv.view[1]=m->sv[1];cam_sv.epoch=epoch;
    memset(&cam_sv.status,0,sizeof(cam_sv.status));cam_sv.status.sensor=sensor;
    cam_sv.status.flags=X9_CAM_SV_HW_ATTEMPTED;
    cam_sv.error=0;cam_sv.terminal=false;cam_sv.dirty=0;cam_sv.command=cam_sv.completed=0;
    cam_sv.request_held=cam_sv.request_ready=false;cam_sv.request_slot=NULL;
    cam_sv.fbc_sampled=cam_sv.fbc_ready=false;
    memset(cam_sv.fbc_before,0,sizeof(cam_sv.fbc_before));
    cam_sv.credit_phase=X9_CAM_SV_CREDIT_EMPTY;cam_sv.credit_issued=false;
    cam_sv.credit_retire_attempted=cam_sv.credit_retired=cam_sv.credit_restore_issued=false;
    cam_sv.credit_retire_samples=cam_sv.credit_restore_seen=0;
    memset(cam_sv.credit_spent_count,0,sizeof(cam_sv.credit_spent_count));
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
        if(memcmp(a,b,cam_sv.count*sizeof(uint32_t)))
            for(unsigned n=0;n<cam_sv.count;n++)if(a[n]!=b[n])
                printf("X9 CAMERA CAMSV INHERITED: bank=%u off=%x moving %x->%x (using second)\n",
                    bank,cam_sv.offset[n],a[n],b[n]);
        e=cam_sv_inactive(b);if(e)return cam_sv_fault(e);
        memcpy(cam_sv.before[bank],b,cam_sv.count*sizeof(uint32_t));
    }
    /* DB toggles can transfer more than tag0. Do not publish inherited
     * staged geometry/addresses/other tags/CQ or request policy. */
    if(memcmp(cam_sv.before[0],cam_sv.before[1],cam_sv.count*sizeof(uint32_t))) {
        unsigned logged=0;
        for(unsigned n=0;n<cam_sv.count && logged<12;n++)if(cam_sv.before[0][n]!=cam_sv.before[1][n]) {
            logged++;
            printf("X9 CAMERA CAMSV INHERITED: off=%x outer=%x inner=%x (Android writes outer, DB loads inner)\n",
                cam_sv.offset[n],cam_sv.before[0][n],cam_sv.before[1][n]);
        }
    }
    memcpy(cam_sv.expected,cam_sv.before,sizeof(cam_sv.expected));
    e=cam_sv_qos_capture();if(e)return cam_sv_fault(e);
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
    for(unsigned slot=CAM_SV_GEOMETRY_FIELDS;slot<CAM_SV_CONFIG_FIELDS;slot++)
        cam_sv.target[slot]=cam_sv_fields[slot].initial;
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
    if(!e)e=cam_sv_qos_start();
    if(!e)e=cam_sv.dirty?cam_sv_commit():cam_sv_target_observe();
    if(!e) {
        cam_sv.status.flags|=X9_CAM_SV_HW_CONFIGURED;
        e=cam_irq_prepare_locked(epoch); /* dormant; no GIC/source enable */
    }
    if(e)cam_sv_fault(e);
    cam_sv_status();
    printf("X9 CAMERA CAMSV CONFIG: sensor=%u epoch=%llu flags=%x dirty=%llx "
        "geometry=%u dma_cqi=%u errno=%d (Android ISP8s static FIFO; source off)\n",
        sensor,epoch,cam_sv.status.flags,cam_sv.dirty,CAM_SV_GEOMETRY_FIELDS,
        CAM_SV_CONFIG_FIELDS-CAM_SV_GEOMETRY_FIELDS,e);
    return e;
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
            if(first!=observed[bank])return cam_sv_fault(CAM_FAIL(ESTALE));
            if(observed[bank]&CAM_SV_FBC_COUNT_MASK)return cam_sv_fault(CAM_FAIL(EBUSY));
        }
        if(observed[0]!=observed[1])return cam_sv_fault(CAM_FAIL(ESTALE));
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
        if(cam_sv.expected[bank][cam_sv.field[CAM_SV_FBC]]!=target)e=CAM_FAIL(ESTALE);
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
       !(cam_sv.status.flags&X9_CAM_SV_HW_CONFIGURED))return CAM_FAIL(ESTALE);
    if(cam_sv.error)return cam_sv.error;
    if(cam_sv.credit_issued)return CAM_FAIL(EBUSY);
    if(cam_sv.commit_pending || cam_sv.pending_slot<=CAM_SV_FIELDS)return CAM_FAIL(EBUSY);
    /* Copy under the actual slot lock; no callback while it is held. IRQ
     * accepting is still false, and controller serialization pins the slot. */
    lck_mtx_lock(&camera_camsv_lock);
    e=slot->state==X9_CAMSV_ARMED?0:ESTALE;
    r=slot->request;d=slot->dma;
    lck_mtx_unlock(&camera_camsv_lock);
    if(e)return e;
    if(r.tag || r.group || r.reserved || r.frame.sensor!=cam_sv.status.sensor ||
       r.frame.time || r.frame.sof_time || !r.frame.generation || !r.frame.capture_id || !r.iova ||
       (r.iova&0x3fffULL) || r.iova>=0x400000000ULL ||
       r.frame.bytes>0x400000000ULL-r.iova || d.version!=X9_CAMERA_CAMSV_VERSION ||
       d.reserved || !d.context || !d.lease || !d.backing || !d.check)return CAM_FAIL(EINVAL);
    if(cam_sv.request_held && (cam_sv.request_slot!=slot ||
       cam_sv.request_dma.context!=d.context || cam_sv.request_dma.check!=d.check ||
       cam_sv.request_dma.backing!=d.backing))return CAM_FAIL(EBUSY);
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
                if(cam_sv.expected[bank][cam_sv.field[field]]!=cam_sv.target[field])e=CAM_FAIL(ESTALE);
    }
    if(!e) {
        e=cam_sv_fbc_prepare_locked();
        if(e && !cam_sv.status.fbc_errno)cam_sv.status.fbc_errno=e;
    }
    if(e)cam_sv_fault(e);
    else cam_sv.request_ready=true;
    cam_sv_status();
    printf("X9 CAMERA REQUEST: sensor=%u epoch=%llu generation=%llu capture_id=%llu "
        "inner=%x ready=%u errno=%d dirty=%llx command=%llu/%llu "
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
       slot!=cam_sv.request_slot)return CAM_FAIL(ESTALE);
    lck_mtx_lock(&camera_camsv_lock);
    e=(slot->state!=X9_CAMSV_ARMED && slot->state!=X9_CAMSV_RETIRED &&
         (slot->state!=X9_CAMSV_CANCELLED ||
          cam_repeat_cancelled_check_locked(epoch,&slot->request,slot->dma.backing))) ||
        memcmp(&slot->request,&cam_sv.request,sizeof(cam_sv.request)) ||
        slot->dma.version!=cam_sv.request_dma.version || slot->dma.reserved ||
        slot->dma.context!=cam_sv.request_dma.context || slot->dma.lease!=cam_sv.request_dma.lease ||
        slot->dma.backing!=cam_sv.request_dma.backing || slot->dma.check!=cam_sv.request_dma.check?CAM_FAIL(ESTALE):0;
    lck_mtx_unlock(&camera_camsv_lock);
    if(!e)e=cam_sv_environment();
    if(!e && (cam_sv.commit_pending || cam_sv.pending_slot<=CAM_SV_FIELDS))e=CAM_FAIL(EBUSY);
    if(!e)for(unsigned bank=0;bank<2;bank++) {
        const uint32_t *v=cam_sv.expected[bank];
        if(v[cam_sv.field[CAM_SV_ADDR_LO]]!=(uint32_t)cam_sv.request.iova ||
           v[cam_sv.field[CAM_SV_ADDR_HI]]!=(uint32_t)(cam_sv.request.iova>>32) ||
           v[cam_sv.field[CAM_SV_FH]]!=cam_sv.request.inner_sequence ||
           v[cam_sv.field[CAM_SV_FBC]]!=cam_sv.target[CAM_SV_FBC])e=CAM_FAIL(ESTALE);
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
/* 389: FBC credit reached count==target/control on both banks (seen=ack=3,
 * samples=1) but the 1 s deadline expired before the confirming second sample,
 * ~7.7 s after the request: the guards inside this loop dominate. Time them. */
#define CAM_SV_TIMED(name,expr) ({ static unsigned _h;uint64_t _t0=mach_absolute_time();int _e=(expr);cam_slow_note(name,&_h,_t0);_e; })
static int cam_sv_credit_issue_locked(uint64_t epoch,struct x9_camera_camsv_slot *slot)
{
    uint64_t span,end;
    uint32_t control;
    int e;
    if(cam_sv.credit_issued || cam_sv.credit_phase!=X9_CAM_SV_CREDIT_EMPTY)return CAM_FAIL(EBUSY);
    e=CAM_SV_TIMED("credit:sv_request_guard",cam_sv_request_guard_locked(epoch,slot));if(e)return e;
    lck_mtx_lock(&camera_camsv_lock);e=slot->state==X9_CAMSV_ARMED?0:ESTALE;
    lck_mtx_unlock(&camera_camsv_lock);if(e)return e;
    if(cam_sv.commit_pending || cam_sv.pending_slot<=CAM_SV_FIELDS)return CAM_FAIL(EBUSY);
    control=cam_sv.target[CAM_SV_FBC];
    if((control&CAM_SV_FBC_MASK)!=CAM_SV_FBC_EN)return CAM_FAIL(EINVAL);
    for(unsigned bank=0;bank<2;bank++) {
        uint32_t before=cam_sv.fbc_before[bank];
        if(before&CAM_SV_FBC_COUNT_MASK)return CAM_FAIL(EBUSY);
        cam_sv.status.credit_target[bank]=cam_sv.credit_target[bank]=
            (before&~(CAM_SV_FBC_COUNT_MASK|0xffu))|0x10000u|((before+1)&0xffu);
    }
    e=CAM_SV_TIMED("credit:sv_request_guard",cam_sv_request_guard_locked(epoch,slot));if(e)return e;
    e=CAM_SV_TIMED("credit:irq_credit_guard",cam_irq_credit_guard_locked(epoch));if(e)return e;
    nanoseconds_to_absolutetime(CAM_GUARD_BUDGET_NS,&span);end=mach_absolute_time()+span;
    /* Journal BEFORE the single store. Once issued, failure is terminal and
     * leaves the SAME lease/RAW/controller/power pinned, with no cancellation.
     * RCNT is not put in geometry dirty/pending and no DB pulse follows it. */
    cam_sv.credit_issued=true;cam_sv.credit_phase=X9_CAM_SV_CREDIT_PENDING;
    cam_sv.credit_seen=cam_sv.credit_ack=cam_sv.credit_samples=0;
    cam_sv_status();cam_rw(cam_sv.view[0],0x540,control|CAM_SV_FBC_RCNT);
    e=CAM_FAIL(EAGAIN);
    for(unsigned poll=0;poll<10000;poll++) {
        bool target=true;
        if(mach_absolute_time()>=end && !cam_sv.credit_samples){e=CAM_FAIL(ETIMEDOUT);break;}
        e=CAM_SV_TIMED("credit:sv_request_guard",cam_sv_request_guard_locked(epoch,slot));if(e)break;
        e=CAM_SV_TIMED("credit:irq_credit_guard",cam_irq_credit_guard_locked(epoch));if(e)break;
        for(unsigned bank=0;bank<2 && !e;bank++) {
            uint32_t count,actual;
            e=CAM_SV_TIMED("credit:sv_power_guard",cam_sv_power_guard());if(e)break;
            count=cam_rr(cam_sv.view[bank],0x544);
            actual=cam_rr(cam_sv.view[bank],0x540);
            cam_sv.status.fbc_after[bank]=count;cam_sv.status.credit_control[bank]=actual;
            if(count==cam_sv.credit_target[bank])cam_sv.credit_seen|=1u<<bank;
            else if((cam_sv.credit_seen&(1u<<bank)) || count!=cam_sv.fbc_before[bank])e=CAM_FAIL(ESTALE);
            if(actual!=control && cam_sv_fbc_compare_word(bank,actual)!=control)e=CAM_FAIL(ESTALE);
            if(!e && count==cam_sv.credit_target[bank] && actual==control)
                cam_sv.credit_ack|=1u<<bank;
            else target=false;
        }
        if(e)break;
        cam_sv.credit_samples=target?cam_sv.credit_samples+1:0;
        if(mach_absolute_time()>=end && cam_sv.credit_samples!=1 && cam_sv.credit_samples!=2){e=CAM_FAIL(ETIMEDOUT);break;}
        if(cam_sv.credit_samples==2) {
            cam_sv.credit_phase=X9_CAM_SV_CREDIT_ACCEPTED;
            e=CAM_SV_TIMED("credit:sv_request_guard",cam_sv_request_guard_locked(epoch,slot));
            if(!e)e=CAM_SV_TIMED("credit:irq_credit_guard",cam_irq_credit_guard_locked(epoch));
            break;
        }
        e=CAM_FAIL(EAGAIN);delay(1);
    }
    if(e) {
        if(e==EAGAIN)e=CAM_FAIL(ETIMEDOUT);
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
 * only the same inactive arm, or a streamed arm with the private actual
 * OFF/DONE/CPU/worker/GIC receipt, can be quiesced here. Accepted credit,
 * backing and history are preserved, so this NEVER authorizes RAW reuse,
 * unmap, dependency release or normal producer retirement after a frame. */
static int cam_sv_receive_transition_locked(uint64_t epoch,bool off)
{
    uint64_t span,end;
    int e;
    if(!cam_sv.held || cam_sv.epoch!=epoch || !cam_sv.request_held ||
       !cam_sv.request_ready || !cam_sv.fbc_ready ||
       cam_sv.credit_phase!=X9_CAM_SV_CREDIT_ACCEPTED)return CAM_FAIL(ESTALE);
    if(cam_sv.receive_phase!=(off?X9_CAM_SV_RECEIVE_ARMED:X9_CAM_SV_RECEIVE_IDLE))return CAM_FAIL(EBUSY);
    if(off && !cam_sv_gate_off_ready())return CAM_FAIL(EBUSY);
    if(cam_sv.commit_pending || cam_sv.pending_slot<=CAM_SV_FIELDS)return CAM_FAIL(EBUSY);
    e=cam_sv_request_guard_locked(epoch,cam_sv.request_slot);if(e)return e;
    e=cam_irq_control_guard_locked(epoch,off && session.stream);if(e)return e;
    e=cam_irq_slot_guard_locked(off && session.stream);if(e)return e;
    for(unsigned i=0;i<X9_CAM_SV_RECEIVE_FIELDS;i++) {
        unsigned reg=cam_sv_find(cam_sv_receive_fields[i].offset);
        uint32_t expected=off?cam_sv_receive_fields[i].target:0;
        if(reg>=cam_sv.count || cam_sv.status.receive_before[i] ||
           cam_sv.expected[0][reg]!=expected ||
           (cam_sv.expected[1][reg]!=expected && cam_sv.expected[1][reg]!=0))return CAM_FAIL(ESTALE);
    }
    nanoseconds_to_absolutetime(CAM_GUARD_BUDGET_NS,&span);end=mach_absolute_time()+span;
    cam_sv.receive_phase=off?X9_CAM_SV_RECEIVE_QUIESCING:X9_CAM_SV_RECEIVE_PENDING;
    /* DONE -> ERR -> SOF -> DMA, matching the normal disable writer's
     * ordering for this subset. CHANNEL/COMMON remain off; no history clear.
     * On arm, all source enables are accepted BEFORE the IMG0A DMA enable. */
    for(unsigned i=0;i<X9_CAM_SV_RECEIVE_FIELDS;i++) {
        uint32_t bit=1u<<i,next=off?0:cam_sv_receive_fields[i].target;
        bool complete=false;
        /* 391: field 0 was accepted (samples=2) but the single 1 s budget for
         * all four fields was gone before field 1 was issued: the guards, not
         * the hardware, spend it. Each field gets its own 1 s budget. */
        if(i)end=mach_absolute_time()+span;
        e=CAM_SV_TIMED("receive:sv_request_guard",cam_sv_request_guard_locked(epoch,cam_sv.request_slot));if(e)break;
        e=cam_irq_control_guard_locked(epoch,off && session.stream);if(e)break;
        if(off) {
            if(cam_sv.receive_off_issued&bit){e=CAM_FAIL(ESTALE);break;}
            cam_sv.receive_off_issued|=bit;
        } else {
            if(cam_sv.receive_issued&bit){e=CAM_FAIL(ESTALE);break;}
            cam_sv.receive_issued|=bit;
        }
        cam_sv.receive_samples=0;cam_sv_status();
        cam_rw(cam_sv.view[0],cam_sv_receive_fields[i].offset,next);
        for(unsigned poll=0;poll<10000;poll++) {
            bool match=true;
            if(mach_absolute_time()>=end && !cam_sv.receive_samples){e=CAM_FAIL(ETIMEDOUT);break;}
            e=CAM_SV_TIMED("receive:sv_request_guard",cam_sv_request_guard_locked(epoch,cam_sv.request_slot));if(e)break;
            e=CAM_SV_TIMED("receive:irq_control_guard",cam_irq_control_guard_locked(epoch,off && session.stream));if(e)break;
            /* Outer only: Android writes these without readback and inner
             * loads at the next CAM DB (SOF / toggle_db). */
            /* Fresh outer read each poll: the environment sweep that used to
             * refresh receive_after may now be reused for 20 ms. */
            e=cam_sv_power_guard();if(e)break;
            cam_sv.status.receive_after[0][i]=cam_rr(cam_sv.view[0],cam_sv_receive_fields[i].offset);
            for(unsigned bank=0;bank<1;bank++)
                if(cam_sv.status.receive_after[bank][i]!=next)match=false;
            cam_sv.receive_samples=match?cam_sv.receive_samples+1:0;
            if(cam_sv.receive_samples==2){complete=true;break;}
            if(mach_absolute_time()>=end && !cam_sv.receive_samples){e=CAM_FAIL(ETIMEDOUT);break;}
            delay(1);
        }
        if(e)break;
        if(!complete){e=CAM_FAIL(ETIMEDOUT);break;}
    }
    if(!e) {
        cam_sv.receive_phase=off?X9_CAM_SV_RECEIVE_QUIESCENT:X9_CAM_SV_RECEIVE_ARMED;
        e=cam_sv_request_guard_locked(epoch,cam_sv.request_slot);
        if(!e)e=cam_irq_control_guard_locked(epoch,off && session.stream);
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
    unsigned inner_diff=0,inner_first=0;
    int e;
    if(!cam_sv.held)return 0;
    if(cam_sv.epoch!=epoch || cam_sv.close_attempted)return CAM_FAIL(ESTALE);
    /* Only the actual natural spent receipt may restore ordinary FBC EN.
     * EN off/reset cannot revoke pending credit or release its backing. */
    if(cam_sv.credit_issued && !cam_sv.credit_retired)return cam_sv_fault(CAM_FAIL(EBUSY));
    cam_sv.close_attempted=true;
    cam_sv.request_ready=cam_sv.fbc_ready=false;
    nanoseconds_to_absolutetime(CAM_GUARD_BUDGET_NS,&span);deadline=mach_absolute_time()+span;
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
    if(!e && (cam_sv.dirty&(1ull<<CAM_SV_FBC))) {
        uint32_t prior=cam_sv.before[0][cam_sv.field[CAM_SV_FBC]];
        e=cam_sv_field_write(CAM_SV_FBC,prior);
        if(!e)e=cam_sv_commit();
        if(!e)for(unsigned bank=0;bank<2;bank++)
            if(cam_sv.expected[bank][cam_sv.field[CAM_SV_FBC]]!=prior)e=cam_sv_fault(CAM_FAIL(ESTALE));
    }
    /* Return the stopped TG's OFF/FLUSH profile only after original FBC EN
     * is actually off in both banks. CMOS/VF stay off, source stays stopped,
     * original natural counters and permanent mapping remain unchanged. */
    if(!e && cam_sv.credit_retired)e=cam_sv_gate_restore_locked(epoch);
    for(unsigned i=CAM_SV_FBC;i && !e;i--) {
        unsigned slot=i-1;
        if(cam_sv.dirty&(1ull<<slot))e=cam_sv_field_write(slot,cam_sv.before[0][cam_sv.field[slot]]);
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
        for(unsigned bank=0;bank<2;bank++)for(unsigned n=0;n<cam_sv.count;n++) {
            /* SW requests stay asserted through natural frame/translation
             * retirement. Final normal reset owns their independent release;
             * ordinary config restoration must never replay saved SET words. */
            if(cam_sv_qos_index(cam_sv.offset[n])<CAM_SV_QOS_COUNT)continue;
            if(cam_sv.expected[bank][n]!=cam_sv.before[bank][n]) {
                if(!bank)e=cam_sv_fault(CAM_FAIL(ESTALE));
                else if(!inner_diff++)inner_first=cam_sv.offset[n];
            }
        }
        if(inner_diff)printf("X9 CAMERA CAMSV CLOSE ADOPT: %u inner words differ from cold (first off=%x); outer restored\n",
            inner_diff,inner_first);
    }
    if(!e) {
        cam_sv.held=false;cam_sv.dirty=0;cam_sv.db_phase=CAM_SV_DB_IDLE;cam_sv.db_issued=false;
        cam_sv.request_ready=cam_sv.request_held=false;cam_sv.request_slot=NULL;
        cam_sv.fbc_sampled=cam_sv.fbc_ready=false;
        cam_sv.status.flags|=X9_CAM_SV_HW_RESTORED;
    } else cam_sv_fault(e);
    cam_sv.status.restore_errno=e;cam_sv_status();
    printf("X9 CAMERA CAMSV CLOSE: sensor=%u epoch=%llu flags=%x errno=%u restore=%d "
        "dirty=%llx pending=%u commit=%u dbphase=%u sequence=%llu/%llu terminal=%u "
        "DB=%x/%x (inactive/spent FBC geometry/address/FH restore, NOT AXI/map drain)\n",
        cam_sv.status.sensor,epoch,cam_sv.status.flags,cam_sv.status.last_errno,e,cam_sv.status.dirty,
        cam_sv.status.pending,cam_sv.status.commit_pending,cam_sv.status.db_phase,cam_sv.status.command,
        cam_sv.status.completed,cam_sv.status.terminal,cam_sv.status.db_after[0],cam_sv.status.db_after[1]);
    return e;
}
