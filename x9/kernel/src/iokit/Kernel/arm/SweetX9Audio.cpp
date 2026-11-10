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
#include <sys/errno.h>
#include <sys/proc.h>
#include "../../../bsd/dev/x9_audio.h"
#include "../../../bsd/dev/x9_adsp_transport.h"
#include "../../../bsd/dev/x9_adsp_mt6993.h"
#include "../../../bsd/dev/x9_speaker_afe.h"
#include "../../../bsd/dev/x9_tfa_plj110.h"
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
static int16_t period[X9_AUDIO_PERIOD * X9_AUDIO_CHANNELS];
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
static void worker(void *, wait_result_t)
{
    for (;;) {
        lck_mtx_lock(&audioLock);
        if (control->state & X9AudioRunning) {
            X9AudioControl s = *control;
            uint64_t played = 0, underruns = 0;
            int e = backend.position(backend.context, &played, &underruns);
            if (!e && (played < s.out_frames || played > s.out_queued ||
                       underruns < s.underruns)) e = EIO;
            if (!e) { s.out_frames = played; s.underruns = underruns; }
            uint64_t written = __atomic_load_n(&producer->written, __ATOMIC_ACQUIRE);
            if (!e && (written < s.out_queued || written - s.out_queued > X9_AUDIO_RING)) {
                s.bad_producer++; e = EINVAL;
            }
            /* Queue at most eight periods per pass, without assuming that a
             * submitted period has played. EAGAIN preserves its PCM/cursor. */
            for (unsigned n = 0; !e && n < 8 && written - s.out_queued >= X9_AUDIO_PERIOD; n++) {
                uint64_t start = s.out_queued;
                for (unsigned f = 0; f < X9_AUDIO_PERIOD; f++) {
                    unsigned slot = (unsigned)((start + f) % X9_AUDIO_RING) * X9_AUDIO_CHANNELS;
                    period[f * 2] = output[slot]; period[f * 2 + 1] = output[slot + 1];
                }
                e = backend.submit(backend.context, period, X9_AUDIO_PERIOD);
                if (e == EAGAIN) { e = 0; break; }
                if (!e) s.out_queued += X9_AUDIO_PERIOD;
            }
            if (e) stopState(&s, e);
            publish(s);
        }
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
extern "C" int x9_audio_register_backend(const x9_audio_backend *b)
{
    if (!sweet_early_is_x9() || (sweet_x9_arg("x9_audio", 0) != 1 && sweet_x9_arg("x9_audio", 0) != 2)) return ENXIO;
    if (!b || b->version != X9_AUDIO_VERSION || b->verified != X9_AUDIO_REQUIRED ||
        b->lead_frames > X9_AUDIO_RING / 2 || !b->start || !b->stop || !b->submit || !b->position) return EINVAL;
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
    writer = type == 1 || type == 2; return true;
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
            X9AudioStreamStatus s = {1, backendHeld ? 1u : 0u, (uint32_t)stopError, 0};
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
    if (sel == X9AudioStatus) {
        if (!a->structureOutput || a->structureOutputSize < sizeof(X9AudioControl)) return kIOReturnBadArgument;
        lck_mtx_lock(&audioLock); memcpy(a->structureOutput, control, sizeof(*control)); lck_mtx_unlock(&audioLock);
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
    else {
        bzero(output, X9_AUDIO_RING_BYTES); __atomic_store_n(&producer->written, 0, __ATOMIC_RELEASE);
        uint64_t anchor = 0;
        backendHeld = true; stopError = 0; /* start failure may own resources */
        e = backend.start(backend.context, &anchor);
        if (!e && !anchor) e = EIO;
        if (e) { stopState(&s, e); s.state = X9AudioFault; }
        else {
            s.generation++; s.out_anchor_host = anchor;
            s.out_frames = s.out_queued = s.underruns = 0;
            s.state = X9AudioReady | X9AudioRunning; s.starts++;
        }
    }
    if (sel == X9AudioStart && !(s.state & X9AudioFault)) s.error = e;
    publish(s); lck_mtx_unlock(&audioLock); return result(e);
}
#endif
