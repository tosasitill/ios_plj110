/* X9 kernel audio/PCM boundary. UniXNU-sheng supplies the HAL/shared-ring
 * architecture, not MT6993 MMIO or Apple AudioDevice user-client selectors.
 * The physical backend is deliberately absent until ADSP/AFE/PA ownership
 * is implemented. Publishing this service must never imply audible output. */
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include "../../../bsd/dev/x9_audio.h"
#include "../../../bsd/dev/x9_adsp_transport.h"
#include "../../../bsd/dev/x9_adsp_mt6993.h"
#include "../../../bsd/dev/x9_speaker_afe.h"
#include "../../../bsd/dev/x9_tfa_plj110.h"
#include "../../../bsd/dev/x9_speaker_bringup.h"
#include "../../../bsd/dev/x9_mic_bringup.h"
#include "../../../bsd/dev/x9_i2c6.h"
#include "../../../bsd/dev/x9_pa_vdd_left.h"
#include "../../../bsd/dev/x9_audio_resources.h"
#include "../../../bsd/dev/x9_speaker_mt6993.h"
#include "../../../bsd/dev/x9_speaker_pll.h"
#include "../../../bsd/dev/x9_speaker_path.h"
#if defined(ARM64_BOARD_CONFIG_SWEET)
extern "C" int sweet_early_is_x9(void);
extern "C" int sweet_x9_arg(const char *, int);
static LCK_GRP_DECLARE(audioGroup, "x9_audio");
static LCK_MTX_DECLARE(audioLock, &audioGroup);
static IOBufferMemoryDescriptor *memory[3];
static X9AudioControl *control;
static X9AudioProducer *producer;
static int16_t *output;
static x9_audio_backend backend;
static bool haveBackend, haveWorker;
static bool backendHeld;
static int stopError;
static uint64_t physicalPlayed, physicalQueued, physicalAnchor, lastPosition, lastProgress;
static uint32_t prefillFrames, positionRetries;
static bool prefillCommitted;
/* Never pass user-writable output pages as the initial DMA prefill. */
static int16_t prefill[X9_AUDIO_PREFILL_MAX * X9_AUDIO_CHANNELS];
static int16_t period[X9_AUDIO_PERIOD * X9_AUDIO_CHANNELS];
static_assert(sizeof(X9AudioControl) == 112, "PCM ABI v1 size");
static_assert(sizeof(X9AudioStreamStatus) == 48, "stream status v2 size");
static_assert(sizeof(X9AudioClockStatus) == 216, "clock status v1 size");
static_assert(sizeof(X9AudioI2C6Status) == 128, "native I2C6 cached status v1 size");
static_assert(sizeof(X9AudioLeftVDDStatus) == 64, "LEFT VDD cached status v1 size");
static_assert(sizeof(X9AudioPLLStatus) == 264, "PLL status v1 size");
static_assert(sizeof(X9AudioPathStatus) == 272, "path status v1 size");
static void beginUpdate() { __atomic_add_fetch(&control->seq, 1, __ATOMIC_SEQ_CST); }
static void endUpdate() { __atomic_add_fetch(&control->seq, 1, __ATOMIC_SEQ_CST); }
static int stopState(X9AudioControl *s, int error)
{
    int stopped = 0;
    /* Keep ownership independent of UI Running. A failed stop must still be
     * called on explicit Stop/close; it cannot become a successful no-op. */
    if (backendHeld) {
        stopped = backend.stop(backend.context); s->stops++; stopError = stopped;
        if (!stopped) backendHeld = false;
    }
    s->state &= ~X9AudioRunning;
    if (error) s->error = error;
    else if (!s->error) s->error = stopped;
    if (s->error) s->state = X9AudioFault;
    return stopped;
}
/* Do not leave seq odd across mailbox/DMA operations: the realtime HAL must
 * never wait for a physical driver. Only the bounded publication is bracketed. */
static void publish(const X9AudioControl &s)
{
    beginUpdate();
    /* Never memcpy an even source seq over the odd live seq, even briefly. */
    memcpy(control, &s, offsetof(X9AudioControl, seq));
    /* Start from the enclosing object, not a 4-byte field: fortified XNU
     * memcpy otherwise treats this tail copy as a field overflow. */
    const size_t tail = offsetof(X9AudioControl, generation);
    memcpy((uint8_t *)control + tail, (const uint8_t *)&s + tail, sizeof(s) - tail);
    endUpdate();
}
static void stopLocked(int error)
{
    X9AudioControl s = *control; stopState(&s, error); publish(s);
}
/* A transient unstable CUR is not a fatal error by itself. Never extend the
 * budget from wall-clock "progress", nor accept EAGAIN forever. Physical PA
 * mute/quiesce is still the backend's independent responsibility. */
static bool positionBudgetExpired()
{
    if (physicalPlayed > physicalQueued) return true;
    uint64_t pending = physicalQueued - physicalPlayed;
    if (pending <= X9_AUDIO_PERIOD) return true;
    /* Clamp in frames BEFORE multiplication: submitted is a lifetime cursor,
     * not a bounded DMA-ring size. One period is the 10 ms observation cap. */
    uint64_t safe = pending - X9_AUDIO_PERIOD;
    if (safe > X9_AUDIO_PERIOD) safe = X9_AUDIO_PERIOD;
    uint64_t budget = safe * 1000000000ULL / X9_AUDIO_RATE;
    uint64_t now = mach_absolute_time(), elapsed, stalled;
    absolutetime_to_nanoseconds(now - lastPosition, &elapsed);
    absolutetime_to_nanoseconds(now - lastProgress, &stalled);
    /* A successful read of an unchanged cursor does NOT renew progress. */
    return elapsed >= budget || stalled >= budget;
}
static void worker(void *, wait_result_t)
{
    for (;;) {
        lck_mtx_lock(&audioLock);
        if (!(control->state & X9AudioRunning)) {
            assert_wait((event_t)&haveWorker, THREAD_UNINT);
            lck_mtx_unlock(&audioLock);
            thread_block(THREAD_CONTINUE_NULL); continue;
        }
        X9AudioControl s = *control;
        uint64_t played = 0, underruns = 0;
        uint64_t observed = mach_absolute_time();
        int e = positionBudgetExpired() ? ETIMEDOUT : backend.position(backend.context, &played, &underruns);
        bool retry = e == EAGAIN;
        if (retry) {
            if (positionRetries != UINT32_MAX) positionRetries++;
            e = positionBudgetExpired() ? ETIMEDOUT : 0;
        }
        if (!e && !retry) {
            if (!prefillCommitted || played < physicalPlayed || played > physicalQueued ||
                underruns < s.underruns) e = EIO;
            else if (underruns != s.underruns) { s.underruns = underruns; e = EPIPE; }
            else if (positionBudgetExpired()) e = ETIMEDOUT; /* callback itself may have been late */
            else {
                if (played != physicalPlayed) lastProgress = observed;
                physicalPlayed = played; lastPosition = observed;
                /* Leading silence belongs to hardware, not writable PCM ABI.
                 * Clamping before sample0 is NOT fabricated cursor progress. */
                s.out_frames = played > prefillFrames ? played - prefillFrames : 0;
                s.underruns = underruns;
                if (physicalQueued - played <= X9_AUDIO_PERIOD) e = EPIPE;
            }
        }
        uint64_t written = __atomic_load_n(&producer->written, __ATOMIC_ACQUIRE);
        if (!e && (written < s.out_queued || written - s.out_queued > X9_AUDIO_RING)) {
            s.bad_producer++; e = EINVAL;
        }
        /* No submissions against an uncertain physical cursor; EAGAIN may
         * not authorize overwriting a consumed/replayed ring segment. */
        for (unsigned n = 0; !e && !retry && n < 8 && written - s.out_queued >= X9_AUDIO_PERIOD; n++) {
            if (s.out_queued > UINT64_MAX - prefillFrames - X9_AUDIO_PERIOD) { e = EOVERFLOW; break; }
            uint64_t start = s.out_queued;
            for (unsigned f = 0; f < X9_AUDIO_PERIOD; f++) {
                unsigned slot = (unsigned)((start + f) % X9_AUDIO_RING) * X9_AUDIO_CHANNELS;
                period[f * 2] = output[slot]; period[f * 2 + 1] = output[slot + 1];
            }
            e = backend.submit(backend.context, period, X9_AUDIO_PERIOD);
            if (e == EAGAIN) { e = 0; break; }
            if (!e) { s.out_queued += X9_AUDIO_PERIOD; physicalQueued += X9_AUDIO_PERIOD; }
        }
        if (e) stopState(&s, e);
        publish(s);
        lck_mtx_unlock(&audioLock);
        delay_for_interval(2, 1000 * NSEC_PER_USEC);
    }
}
static int ensureWorker()
{
    if (!control || !haveBackend || haveWorker) return 0;
    thread_t thread;
    if (kernel_thread_start(worker, nullptr, &thread) != KERN_SUCCESS) return ENOMEM;
    thread_deallocate(thread); haveWorker = true; return 0;
}
/* x9_dsp_boot=<1..600> with x9_audio=2: ONE unattended equivalent of
 * `audioctl dsp-boot` <N> s after start, for images without an iOS shell.
 * Same lock order and busy checks as the admin selector (audio -> transport
 * -> board); no retry, no automatic stop. Board/DSP state is logged after. */
/* x9_dsp_probe=<1..600> with x9_audio=2: read-only inherited-state report
 * at start and once more after <N> s. Never boots, votes or resets. */
static void dspProbeOnce(void *arg, wait_result_t)
{
    IOSleep((uint32_t)(uintptr_t)arg * 1000);
    x9_adsp_mt6993_inherited_report("delayed");
}
static void dspBootOnce(void *arg, wait_result_t)
{
    uint32_t delay = (uint32_t)(uintptr_t)arg;
    IOSleep(delay * 1000);
    /* The two paths share the exclusive audio resource lease, never overlap.
     * Logs 272/275/282: with the mic's AFE work first, the SoC hung right
     * after the speaker's "CLK native board -> 0" (282 with NO ADSP launch,
     * so this is separate from the 277 launch hang). 276, speaker first,
     * did not hang. Speaker first by default again (spk23); x9_spk_first=0
     * restores mic first. */
    int micMs = sweet_x9_arg("x9_mic_boot", 0);
    bool spkFirst = sweet_x9_arg("x9_spk_first", 1) != 0;
    /* spk28: the speaker is the X9Audio backend by default and holds the
     * shared audio lease for the boot, so a mic run would only get EBUSY.
     * Mic tests use x9_spk_boot=0 (mic alone; logs 282/284/287 hung when a
     * second audio subsystem started after the first). */
    bool spkBackend = sweet_x9_arg("x9_spk_boot", 1) && sweet_x9_arg("x9_spk_backend", 1);
    /* x9_mic_boot set = a mic test boot: run the mic ALONE (boot-args have no
     * room for x9_spk_boot=0). x9_spk_backend=0 restores the old one-shot
     * speaker + mic ordering. */
    if (spkBackend && micMs > 0) {
        IOLog("X9 AUDIO DSPBOOT: x9_mic_boot set: mic alone, speaker backend not started this boot\n");
        IOLog("X9 AUDIO DSPBOOT: mic capture result=%d\n", x9_mic_bringup_run((unsigned)micMs));
        return;
    }
    if (micMs > 0 && !spkFirst) IOLog("X9 AUDIO DSPBOOT: mic capture result=%d\n", x9_mic_bringup_run((unsigned)micMs));
    /* Logs 253-277: every boot that launched the ADSP (warm takeover) hung
     * the SoC 0.4-25 s later unless the DSP was halted first (269, 276); 277
     * hung 0.35 s after launch with no AP access and no mic run. The AP-side
     * ADSP runtime protocol (SLB init, awake, feature/suspend) is missing.
     * The speaker now holds the board AP-only (speaker_control 3: no DSP
     * reset/launch) and is on by default (x9_spk_boot=0 skips it). */
    if (!sweet_x9_arg("x9_spk_boot", 1)) {
        IOLog("X9 AUDIO DSPBOOT: speaker bring-up skipped (x9_spk_boot=0)\n");
        if (micMs > 0 && spkFirst) IOLog("X9 AUDIO DSPBOOT: mic capture result=%d\n", x9_mic_bringup_run((unsigned)micMs));
        return;
    }
    if (micMs > 0 && !spkFirst) IOSleep(1000);
    IOLog("X9 AUDIO DSPBOOT: speaker bring-up (AP-only board hold, no DSP launch; clocks, pads, AFE, TFA9865; %s)\n",
          spkBackend ? "X9Audio backend, no boot tone" : "one-shot");
    lck_mtx_lock(&audioLock);
    int e = haveBackend || (control && (control->state & X9AudioRunning)) ? EBUSY : x9_speaker_bringup_run();
    lck_mtx_unlock(&audioLock);
    /* register_backend takes audioLock itself: never call it while held. */
    if (!e && spkBackend) {
        e = x9_speaker_backend_register();
        IOLog("X9 AUDIO DSPBOOT: speaker backend register -> %d\n", e);
    }
    X9AudioBoardStatus b; bzero(&b, sizeof(b)); int be = x9_adsp_mt6993_status(&b);
    X9AudioDSPStatus d; bzero(&d, sizeof(d)); int de = x9_adsp_status(0, &d);
    IOLog("X9 AUDIO DSPBOOT: result=%d board_err=%d flags=%x error=%u restore=%u rollback=%u lease=%u\n",
          e, be, b.flags, b.error, b.restore_error, b.rollback_error, b.lease_error);
    IOLog("X9 AUDIO DSPBOOT: votes owned=%x ap=%x all=%x pd_done=%x pd_busy=%x pending=%x sleep=%x dpsw=%x/%x vlogic=%x sram=%x\n",
          b.owned_vote, b.ap_vote, b.all_vote, b.pd_done, b.pd_busy, b.pending, b.sleep,
          b.dpsw_req, b.dpsw_ack, b.vlogic_ack, b.sram_ack);
    IOLog("X9 AUDIO DSPBOOT: smc op=%u ret=%llx/%llx/%llx/%llx calls=%llu resets=%llu timeouts=%llu guard=%u reg=%x exp=%x act=%x\n",
          b.smc_op, b.smc_result[0], b.smc_result[1], b.smc_result[2], b.smc_result[3],
          b.smc_calls, b.resets, b.timeouts, b.guard_space, b.guard_reg, b.guard_expected, b.guard_actual);
    IOLog("X9 AUDIO DSPBOOT: dsp_err=%d state=%u error=%u gen=%u ready=%llu sent=%llu recv=%llu acks=%llu stray=%llu timeouts=%llu wake_err=%llu primary=%u cleanup=%u lease=%u guard=%u\n",
          de, d.state, d.error, d.generation, d.ready_count, d.sent, d.received, d.matched_acks,
          d.stray_acks, d.timeouts, d.wake_errors, d.primary_error, d.cleanup_error, d.lease_error, d.guard_stage);
    if (micMs > 0 && spkFirst) {
        IOSleep(1000);
        IOLog("X9 AUDIO DSPBOOT: mic capture (after speaker) result=%d\n", x9_mic_bringup_run((unsigned)micMs));
    }
}
extern "C" int x9_audio_register_backend(const x9_audio_backend *b)
{
    if (!sweet_early_is_x9() || (sweet_x9_arg("x9_audio", 0) != 1 && sweet_x9_arg("x9_audio", 0) != 2)) return ENXIO;
    if (!b || b->version != X9_AUDIO_BACKEND_VERSION || b->verified != X9_AUDIO_REQUIRED ||
        b->lead_frames < X9_AUDIO_PREFILL_MIN || b->lead_frames > X9_AUDIO_PREFILL_MAX ||
        b->lead_frames % X9_AUDIO_PERIOD || !b->start || !b->stop || !b->submit || !b->position) return EINVAL;
    lck_mtx_lock(&audioLock);
    int e = haveBackend ? EBUSY : 0;
    if (!e) {
        backend = *b; haveBackend = true;
        e = ensureWorker();
        if (e) { haveBackend = false; bzero(&backend, sizeof(backend)); }
        if (control) {
            beginUpdate(); control->blockers = e ? X9AudioNeedBackend : 0;
            control->state = e ? X9AudioFault : X9AudioReady; control->error = e;
            control->lead_frames = b->lead_frames; endUpdate();
        }
    }
    lck_mtx_unlock(&audioLock); return e;
}
static IOReturn result(int e)
{
    switch (e) {
    case 0: return kIOReturnSuccess;
    case EINVAL: return kIOReturnBadArgument;
    case ENOMEM: return kIOReturnNoMemory;
    case ENXIO: return kIOReturnNotReady;
    case EBUSY: return kIOReturnExclusiveAccess;
    case ETIMEDOUT: return kIOReturnTimeout;
    default: return kIOReturnIOError;
    }
}
class X9Audio : public IOService {
    OSDeclareDefaultStructors(X9Audio);
public:
    IOService *probe(IOService *, SInt32 *) APPLE_KEXT_OVERRIDE;
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(X9Audio, IOService);
IOService *X9Audio::probe(IOService *p, SInt32 *score)
{
    if (!sweet_early_is_x9() || (sweet_x9_arg("x9_audio", 0) != 1 && sweet_x9_arg("x9_audio", 0) != 2)) return nullptr;
    return IOService::probe(p, score);
}
bool X9Audio::start(IOService *p)
{
    if (!IOService::start(p)) return false;
    if (!setProperty("IOUserClientClass", "X9AudioUserClient") ||
        !setProperty("NativeSandboxGrantProcess", "audiomxd,mediaserverd") ||
        !setProperty("X9AudioABIVersion", X9_AUDIO_VERSION, 32) ||
        !setProperty("X9AudioStage", "physical-backend-pending")) return false;
    lck_mtx_lock(&audioLock);
    if (control) { lck_mtx_unlock(&audioLock); return false; }
    const unsigned sizes[] = { PAGE_SIZE, PAGE_SIZE, X9_AUDIO_RING_BYTES };
    for (unsigned n = 0; n < 3; n++) {
        memory[n] = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
            kIOMemoryKernelUserShared | kIODirectionInOut, sizes[n], PAGE_SIZE);
        if (!memory[n]) {
            for (unsigned i = 0; i < n; i++) { memory[i]->release(); memory[i] = nullptr; }
            lck_mtx_unlock(&audioLock); return false;
        }
        bzero(memory[n]->getBytesNoCopy(), sizes[n]);
    }
    control = (X9AudioControl *)memory[0]->getBytesNoCopy();
    producer = (X9AudioProducer *)memory[1]->getBytesNoCopy();
    output = (int16_t *)memory[2]->getBytesNoCopy();
    control->magic = X9_AUDIO_MAGIC; control->version = X9_AUDIO_VERSION;
    control->rate = X9_AUDIO_RATE; control->channels = X9_AUDIO_CHANNELS;
    control->period_frames = X9_AUDIO_PERIOD; control->ring_frames = X9_AUDIO_RING;
    control->lead_frames = haveBackend ? backend.lead_frames : X9_AUDIO_PERIOD * 2;
    mach_timebase_info_data_t tb; clock_timebase_info(&tb);
    control->ticks_per_frame_q32 = ((1000000000ULL * tb.denom / tb.numer) << 32) / X9_AUDIO_RATE;
    int e = ensureWorker();
    control->blockers = haveBackend && !e ? 0 : X9_AUDIO_REQUIRED;
    control->state = e ? X9AudioFault : haveBackend ? X9AudioReady : 0;
    control->error = e ? e : haveBackend ? 0 : ENXIO;
    lck_mtx_unlock(&audioLock);
    if (sweet_x9_arg("x9_audio", 0) == 2) {
        int boardError = x9_adsp_mt6993_init();
        setProperty("X9AudioBoardInitError", boardError, 32);
        int probeDelay = sweet_x9_arg("x9_dsp_probe", 0);
        if (probeDelay) {
            x9_adsp_mt6993_inherited_report("start");
            thread_t t;
            bool armed = !boardError && probeDelay > 0 && probeDelay <= 600 &&
                kernel_thread_start(dspProbeOnce, (void *)(uintptr_t)probeDelay, &t) == KERN_SUCCESS;
            if (armed) thread_deallocate(t);
            IOLog("X9 ADSP PROBE: delayed read-only report %s (x9_dsp_probe=%d board_init=%d)\n",
                  armed ? "armed" : "NOT armed", probeDelay, boardError);
        }
        int dspDelay = sweet_x9_arg("x9_dsp_boot", 0);
        if (dspDelay) {
            thread_t t;
            bool armed = !boardError && dspDelay > 0 && dspDelay <= 600 &&
                kernel_thread_start(dspBootOnce, (void *)(uintptr_t)dspDelay, &t) == KERN_SUCCESS;
            if (armed) thread_deallocate(t);
            IOLog("X9 AUDIO DSPBOOT: %s (x9_dsp_boot=%d board_init=%d)\n",
                  armed ? "armed, one dsp-boot after delay" : "NOT armed", dspDelay, boardError);
        }
    }
    registerService();
    IOLog("X9 AUDIO: ABI v%u output 48000/S16/stereo; physical backend=%u, no automatic PA/DSP writes\n",
          X9_AUDIO_VERSION, haveBackend);
    return true;
}
class X9AudioUserClient : public IOUserClient {
    OSDeclareDefaultStructors(X9AudioUserClient);
    bool writer = false, administrator = false, boardController = false, exclusive = false;
public:
    bool initWithTask(task_t, void *, UInt32, OSDictionary *) APPLE_KEXT_OVERRIDE;
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
    IOReturn clientMemoryForType(UInt32, IOOptionBits *, IOMemoryDescriptor **) APPLE_KEXT_OVERRIDE;
    IOReturn externalMethod(uint32_t, IOExternalMethodArguments *, IOExternalMethodDispatch *,
                           OSObject *, void *) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(X9AudioUserClient, IOUserClient);
bool X9AudioUserClient::initWithTask(task_t task, void *id, UInt32 type, OSDictionary *props)
{
    if (type > 2 || !IOUserClient::initWithTask(task, id, type, props)) return false;
    char name[32] = {}; proc_selfname(name, sizeof(name));
    administrator = clientHasPrivilege(task, kIOClientPrivilegeAdministrator) == kIOReturnSuccess;
    bool permitted = administrator ||
        !strcmp(name, "audiomxd") || !strcmp(name, "mediaserverd");
    if (type == 1 && !permitted) return false;
    if (type == 2 && (!administrator || sweet_x9_arg("x9_audio", 0) != 2)) return false;
    boardController = type == 2;
    writer = type == 1 || type == 2;
    /* spk28/log 288: backend registered but no Start ever arrived. Show who
     * opens the service (the HAL probes type0 every 500 ms: first 8 only). */
    static uint32_t opens;
    uint32_t n = __atomic_add_fetch(&opens, 1, __ATOMIC_RELAXED);
    if (n <= 8 || type) IOLog("X9 AUDIO UC: open #%u by %s type=%u admin=%d\n", n, name, type, administrator);
    return true;
}
bool X9AudioUserClient::start(IOService *p)
{
    if (!OSDynamicCast(X9Audio, p) || !IOUserClient::start(p)) return false;
    if (writer) {
        if (!p->open(this)) return false;
        exclusive = true;
        lck_mtx_lock(&audioLock);
        if (backendHeld) {
            /* A root control-only session may finish a retained stop, but it
             * must neither replace old pages nor reopen PCM for a new writer. */
            if (boardController) { lck_mtx_unlock(&audioLock); return true; }
            lck_mtx_unlock(&audioLock); p->close(this); exclusive = false; return false;
        }
        /* Board control never maps PCM and must not replace a writer's
         * timeline merely to read PA resources or issue a physical stop. */
        if (boardController) { lck_mtx_unlock(&audioLock); return true; }
        /* New session gets new writable pages. Explicitly closing a connection
         * without unmapping cannot let an old writer corrupt its successor. */
        IOBufferMemoryDescriptor *newProducer = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
            kIOMemoryKernelUserShared | kIODirectionInOut, PAGE_SIZE, PAGE_SIZE);
        IOBufferMemoryDescriptor *newOutput = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
            kIOMemoryKernelUserShared | kIODirectionInOut, X9_AUDIO_RING_BYTES, PAGE_SIZE);
        if (!newProducer || !newOutput) {
            if (newProducer) newProducer->release(); if (newOutput) newOutput->release();
            lck_mtx_unlock(&audioLock); p->close(this); exclusive = false; return false;
        }
        memory[1]->release(); memory[2]->release(); memory[1] = newProducer; memory[2] = newOutput;
        producer = (X9AudioProducer *)newProducer->getBytesNoCopy(); output = (int16_t *)newOutput->getBytesNoCopy();
        bzero(producer, PAGE_SIZE); bzero(output, X9_AUDIO_RING_BYTES);
        lck_mtx_unlock(&audioLock);
    }
    return true;
}
void X9AudioUserClient::stop(IOService *p)
{
    if (exclusive) {
        lck_mtx_lock(&audioLock);
        if (!boardController || backendHeld) stopLocked(0);
        lck_mtx_unlock(&audioLock);
        p->close(this); exclusive = false;
    }
    IOUserClient::stop(p);
}
IOReturn X9AudioUserClient::clientClose(void)
{
    if (exclusive) {
        lck_mtx_lock(&audioLock);
        if (!boardController || backendHeld) stopLocked(0);
        lck_mtx_unlock(&audioLock);
    }
    terminate(); return kIOReturnSuccess;
}
IOReturn X9AudioUserClient::clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **out)
{
    if (!out || !options || type > X9AudioOutputMemory) return kIOReturnBadArgument;
    *out = nullptr;
    if (isInactive() || !exclusive || boardController) return kIOReturnNotPermitted;
    *options = type == X9AudioControlMemory ? kIOMapReadOnly : 0;
    lck_mtx_lock(&audioLock); memory[type]->retain(); *out = memory[type]; lck_mtx_unlock(&audioLock);
    return kIOReturnSuccess;
}
IOReturn X9AudioUserClient::externalMethod(uint32_t sel, IOExternalMethodArguments *a,
    IOExternalMethodDispatch *dispatch, OSObject *target, void *reference)
{
    (void)dispatch; (void)target; (void)reference;
    if (!a || isInactive() || !control) return kIOReturnNotReady;
    if (sel == X9AudioLeftVDDStatusSelector) {
        if (a->structureInputDescriptor || a->structureOutputDescriptor || a->asyncWakePort ||
            a->scalarInputCount || a->scalarOutputCount || a->structureInputSize ||
            !a->structureOutput || a->structureOutputSize < sizeof(X9AudioLeftVDDStatus)) return kIOReturnBadArgument;
        X9AudioLeftVDDStatus s; int e = x9_pa_vdd_left_status(&s);
        if (e) return result(e);
        memcpy(a->structureOutput, &s, sizeof(s));
        a->structureOutputSize = sizeof(s); return kIOReturnSuccess;
    }
    if (sel == X9AudioI2C6StatusSelector) {
        if (a->structureInputDescriptor || a->structureOutputDescriptor || a->asyncWakePort ||
            a->scalarInputCount || a->scalarOutputCount || a->structureInputSize ||
            !a->structureOutput || a->structureOutputSize < sizeof(X9AudioI2C6Status)) return kIOReturnBadArgument;
        X9AudioI2C6Status s; int e = x9_i2c6_status(&s);
        if (e) return result(e);
        memcpy(a->structureOutput, &s, sizeof(s));
        a->structureOutputSize = sizeof(s); return kIOReturnSuccess;
    }
    if (sel == X9AudioPathStatusSelector) {
        if (a->structureInputDescriptor || a->structureOutputDescriptor || a->asyncWakePort ||
            a->scalarInputCount || a->scalarOutputCount || a->structureInputSize ||
            !a->structureOutput || a->structureOutputSize < sizeof(X9AudioPathStatus)) return kIOReturnBadArgument;
        X9AudioPathStatus s; int e = x9_speaker_mt6993_path_status(&s);
        if (e) return result(e);
        memcpy(a->structureOutput, &s, sizeof(s));
        a->structureOutputSize = sizeof(s); return kIOReturnSuccess;
    }
    if (sel == X9AudioPLLStatusSelector) {
        if (a->structureInputDescriptor || a->structureOutputDescriptor || a->asyncWakePort ||
            a->scalarInputCount || a->scalarOutputCount || a->structureInputSize ||
            !a->structureOutput || a->structureOutputSize < sizeof(X9AudioPLLStatus)) return kIOReturnBadArgument;
        X9AudioPLLStatus s; int e = x9_speaker_mt6993_pll_status(&s);
        if (e) return result(e);
        memcpy(a->structureOutput, &s, sizeof(s));
        a->structureOutputSize = sizeof(s); return kIOReturnSuccess;
    }
    if (sel == X9AudioClockStatusSelector) {
        if (a->structureInputDescriptor || a->structureOutputDescriptor || a->asyncWakePort ||
            a->scalarInputCount || a->scalarOutputCount || a->structureInputSize ||
            !a->structureOutput || a->structureOutputSize < sizeof(X9AudioClockStatus)) return kIOReturnBadArgument;
        X9AudioClockStatus s; int e = x9_speaker_mt6993_clock_status(&s);
        if (e) return result(e);
        memcpy(a->structureOutput, &s, sizeof(s));
        a->structureOutputSize = sizeof(s); return kIOReturnSuccess;
    }
    if (sel == X9AudioClockProbeSelector || sel == X9AudioPLLProbeSelector || sel == X9AudioPathProbeSelector) {
        if (a->structureInputDescriptor || a->structureOutputDescriptor || a->asyncWakePort ||
            a->scalarInputCount || a->scalarOutputCount || a->structureInputSize ||
            a->structureOutputSize) return kIOReturnBadArgument;
        if (!exclusive || !administrator || !boardController) return kIOReturnNotPrivileged;
        lck_mtx_lock(&audioLock);
        int e = haveBackend || backendHeld || (control->state & X9AudioRunning) ? EBUSY :
            (sel == X9AudioPathProbeSelector ? x9_speaker_mt6993_path_probe() :
                sel == X9AudioPLLProbeSelector ? x9_speaker_mt6993_pll_probe() :
                x9_speaker_mt6993_clock_probe());
        lck_mtx_unlock(&audioLock); return result(e);
    }
    if (sel == X9AudioResourceStatusSelector) {
        if (a->structureInputDescriptor || a->structureOutputDescriptor || a->asyncWakePort ||
            a->scalarInputCount || a->scalarOutputCount || a->structureInputSize ||
            !a->structureOutput || a->structureOutputSize < sizeof(X9AudioResourceStatus)) return kIOReturnBadArgument;
        X9AudioResourceStatus s; int e = x9_audio_resources_status(&s);
        if (e) return result(e);
        memcpy(a->structureOutput, &s, sizeof(s));
        a->structureOutputSize = sizeof(s); return kIOReturnSuccess;
    }
    if (sel == X9AudioAFEStatusSelector || sel == X9AudioStreamStatusSelector) {
        size_t size = sel == X9AudioAFEStatusSelector ? sizeof(struct x9_speaker_afe_status) : sizeof(X9AudioStreamStatus);
        if (a->structureInputDescriptor || a->structureOutputDescriptor || a->asyncWakePort ||
            a->scalarInputCount || a->scalarOutputCount || a->structureInputSize ||
            !a->structureOutput || a->structureOutputSize < size) return kIOReturnBadArgument;
        if (sel == X9AudioAFEStatusSelector) {
            struct x9_speaker_afe_status s; int e = x9_speaker_afe_status(&s);
            if (e) return result(e);
            memcpy(a->structureOutput, &s, sizeof(s));
        } else {
            lck_mtx_lock(&audioLock);
            X9AudioStreamStatus s = {X9_AUDIO_STREAM_STATUS_VERSION, backendHeld ? 1u : 0u,
                (uint32_t)stopError, prefillFrames, physicalPlayed, physicalQueued, physicalAnchor,
                prefillCommitted ? 1u : 0u, positionRetries};
            memcpy(a->structureOutput, &s, sizeof(s)); lck_mtx_unlock(&audioLock);
        }
        a->structureOutputSize = (uint32_t)size; return kIOReturnSuccess;
    }
    if (sel == X9AudioAmpStatusSelector) {
        if (a->structureInputDescriptor || a->structureOutputDescriptor || a->asyncWakePort ||
            a->scalarInputCount || a->scalarOutputCount || a->structureInputSize ||
            !a->structureOutput || a->structureOutputSize < sizeof(X9AudioAmpStatus)) return kIOReturnBadArgument;
        lck_mtx_lock(&audioLock);
        X9AudioAmpStatus s; int e = x9_tfa_plj110_status(&s);
        if (!e) memcpy(a->structureOutput, &s, sizeof(s));
        lck_mtx_unlock(&audioLock);
        if (e) return result(e);
        a->structureOutputSize = sizeof(s); return kIOReturnSuccess;
    }
    if (sel == X9AudioAmpProbeSelector) {
        if (a->structureInputDescriptor || a->structureOutputDescriptor || a->asyncWakePort ||
            a->scalarInputCount || a->scalarOutputCount || a->structureInputSize ||
            a->structureOutputSize) return kIOReturnBadArgument;
        if (!exclusive || !administrator || !boardController) return kIOReturnNotPrivileged;
        lck_mtx_lock(&audioLock);
        int e = haveBackend || backendHeld || (control->state & X9AudioRunning) ? EBUSY :
            x9_tfa_plj110_probe();
        lck_mtx_unlock(&audioLock); return result(e);
    }
    if (sel == X9AudioBoardStatusSelector) {
        if (a->structureInputDescriptor || a->structureOutputDescriptor || a->asyncWakePort ||
            a->scalarInputCount || a->scalarOutputCount || a->structureInputSize ||
            !a->structureOutput || a->structureOutputSize < sizeof(X9AudioBoardStatus)) return kIOReturnBadArgument;
        X9AudioBoardStatus s; int e = x9_adsp_mt6993_status(&s);
        if (e) return result(e);
        memcpy(a->structureOutput, &s, sizeof(s)); a->structureOutputSize = sizeof(s);
        return kIOReturnSuccess;
    }
    if (sel == X9AudioDSPBootSelector || sel == X9AudioDSPStopSelector) {
        if (a->structureInputDescriptor || a->structureOutputDescriptor || a->asyncWakePort ||
            a->scalarInputCount || a->scalarOutputCount || a->structureInputSize ||
            a->structureOutputSize) return kIOReturnBadArgument;
        if (!exclusive || !administrator || !boardController) return kIOReturnNotPrivileged;
        /* Lock order: audio -> transport -> board. No callback re-enters the
         * service. Never reset an ADSP used by an active/registered backend. */
        lck_mtx_lock(&audioLock);
        int e = haveBackend || (control->state & X9AudioRunning) ? EBUSY :
            x9_adsp_mt6993_control(sel == X9AudioDSPStopSelector ? 1 : 0);
        lck_mtx_unlock(&audioLock); return result(e);
    }
    if (sel == X9AudioDSPStatusSelector) {
        if (a->structureInputDescriptor || a->structureOutputDescriptor || a->asyncWakePort ||
            a->scalarInputCount != 1 || !a->scalarInput || a->scalarOutputCount || a->structureInputSize ||
            !a->structureOutput || a->structureOutputSize < sizeof(X9AudioDSPStatus) ||
            a->scalarInput[0] >= 2) return kIOReturnBadArgument;
        X9AudioDSPStatus s;
        int e = x9_adsp_status((uint32_t)a->scalarInput[0], &s);
        if (e) return result(e);
        memcpy(a->structureOutput, &s, sizeof(s)); a->structureOutputSize = sizeof(s);
        return kIOReturnSuccess;
    }
    if (a->structureInputDescriptor || a->structureOutputDescriptor || a->asyncWakePort ||
        a->scalarInputCount || a->scalarOutputCount || a->structureInputSize) return kIOReturnBadArgument;
    if (sel == X9AudioStatus || sel == X9AudioStatusNowaitSelector) {
        if (!a->structureOutput || a->structureOutputSize < sizeof(X9AudioControl)) return kIOReturnBadArgument;
        if (sel == X9AudioStatusNowaitSelector) {
            if (!lck_mtx_try_lock(&audioLock)) return kIOReturnBusy;
        } else lck_mtx_lock(&audioLock);
        memcpy(a->structureOutput, control, sizeof(*control)); lck_mtx_unlock(&audioLock);
        a->structureOutputSize = sizeof(*control); return kIOReturnSuccess;
    }
    if (a->structureOutputSize) return kIOReturnBadArgument;
    if (sel != X9AudioStart && sel != X9AudioStop) return kIOReturnUnsupported;
    if (!exclusive) return kIOReturnNotPrivileged;
    if (sel == X9AudioStart && boardController) return kIOReturnNotPermitted;
    lck_mtx_lock(&audioLock); X9AudioControl s = *control;
    int e = 0;
    if (sel == X9AudioStop) { e = stopState(&s, 0); }
    else if (s.state & X9AudioRunning) { /* idempotent Start */ }
    else if (!haveBackend || !haveWorker || s.blockers || (s.state & X9AudioFault)) e = ENXIO;
    else if (s.generation == UINT32_MAX) e = EOVERFLOW;
    else {
        bzero(output, X9_AUDIO_RING_BYTES); __atomic_store_n(&producer->written, 0, __ATOMIC_RELEASE);
        uint64_t anchor = 0;
        prefillFrames = backend.lead_frames;
        bzero(prefill, prefillFrames * X9_AUDIO_CHANNELS * sizeof(*prefill));
        physicalPlayed = physicalQueued = physicalAnchor = lastPosition = lastProgress = 0;
        prefillCommitted = false; positionRetries = 0;
        backendHeld = true; stopError = 0; /* start failure may own resources */
        e = backend.start(backend.context, prefill, prefillFrames, &anchor);
        if (!e) { prefillCommitted = true; physicalQueued = prefillFrames; physicalAnchor = anchor; }
        if (!e && (!anchor || anchor > mach_absolute_time())) e = EIO;
        uint64_t leadTicks = (uint64_t)(((__uint128_t)prefillFrames * s.ticks_per_frame_q32) >> 32);
        if (!e && anchor > UINT64_MAX - leadTicks) e = EOVERFLOW;
        if (!e) {
            lastPosition = lastProgress = anchor;
            if (positionBudgetExpired()) e = ETIMEDOUT; /* slow activation already exhausted prefill */
        }
        if (e) { stopState(&s, e); s.state = X9AudioFault; }
        else {
            s.generation++; s.out_anchor_host = anchor + leadTicks;
            s.out_frames = s.out_queued = s.underruns = 0;
            s.state = X9AudioReady | X9AudioRunning; s.starts++;
            thread_wakeup((event_t)&haveWorker);
        }
    }
    if (sel == X9AudioStart && !(s.state & X9AudioFault)) s.error = e;
    publish(s); lck_mtx_unlock(&audioLock);
    IOLog("X9 AUDIO UC: %s -> %d state=%x gen=%u\n", sel == X9AudioStart ? "START" : "STOP", e, s.state, s.generation);
    return result(e);
}
#endif
