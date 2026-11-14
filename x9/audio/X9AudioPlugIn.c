// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// Property/interface scaffold adapted from UniXNU-sheng 08cde20, LICENSE.UniXNU.
// D22 device model ("Speaker" with a silent stereo input, plus the silent
// "Hawking" companion) follows HakiOS userland/src/audio_plugin.c
// (xm6661885/hakios-xnu-kernel-umi-fixes 05f7477, MIT, LICENSE.hakios).
// X9: output-only, split RO control/RW producer, bounded seqlock snapshots,
// hardware readiness, explicit unmapping, no shared-cache patches or hooks.
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <math.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include "../kernel/src/bsd/dev/x9_audio.h"
_Static_assert(sizeof(X9AudioControl) == 112, "PCM ABI v1 size");

enum { kPlugIn = kAudioObjectPlugInObject, kDevice = 2, kOutStream = 3, kInStream = 4, kVolume = 5,
       kHawking = 6, kHawkingIn = 7, kLastObject = kHawkingIn };
/* iOS 16 VirtualAudio (DeviceFactory_Aspen) picks the device class by HAL
 * device UID. Under our D22AP identity it builds the iPhone X speaker route
 * from "Speaker" (stereo output + stereo input, the amplifier feedback on a
 * real iPhone: silent here) and refuses it without the mandatory 'vhaw'
 * companion, whose only port is the input of a device "Hawking"; otherwise
 * everything stays on the null port (HakiOS, D22 on iOS 16.5, tested). Log
 * 293 with "PuffinOutput": enumerated, never a client. Both UIDs are in
 * 20H392 VirtualAudio's UID table (0x596709 "Hawking", "Speaker"). */
#define kDeviceUID "Speaker"
#define kModelUID "Speaker"
#define kHawkingUID "Hawking"
static const Float64 kRate = X9_AUDIO_RATE;
static const UInt32 kZeroPeriod = X9_AUDIO_RING;
static AudioServerPlugInHostRef gHost;
static pthread_mutex_t gLock = PTHREAD_MUTEX_INITIALIZER;
static atomic_uint gRefs = 1;
static uint32_t gVolumeBits = 0x3f800000;
static uint32_t gStreamActive = 1;
static UInt32 gIoUsers;
static int gStopFailed;
static bool gReady, gIOFault, gControlDirty;
static uint32_t gGeneration;
static uint64_t gActiveAnchor, gActiveTicks;
static uint32_t gLeadFrames = X9_AUDIO_PREFILL_MIN;
#define X9_AUDIO_HAL_CLIENTS 64u
static struct { UInt32 id; bool used, running; } gClients[X9_AUDIO_HAL_CLIENTS];
static dispatch_source_t gControlTimer;
/* Hawking has no hardware: its clock is the host's, from its first StartIO. */
static UInt32 gHawkUsers;
static uint64_t gHawkAnchor;
static double gHostTicksPerFrame;
static void refreshControlState(void *);
static OSStatus stopClient(unsigned);
static io_connect_t gConnect;
static X9AudioControl *gControl;
static X9AudioProducer *gProducer;
static int16_t *gOut;
static mach_vm_address_t gMaps[3];
static mach_vm_size_t gSizes[3];
static uint32_t floatBits(float x) { uint32_t v; memcpy(&v, &x, 4); return v; }
/* ---- Diagnostics into the kernel log (log 292: iOS never opened the
 * writer and the unified log is not visible). Lines are queued here and
 * flushed by the 500 ms status timer through selector 22 on a short type0
 * connection. Never called from the IO path. */
static pthread_mutex_t gDiagLock = PTHREAD_MUTEX_INITIALIZER;
#define DIAG_LINES 48u
static char gDiag[DIAG_LINES][240];
static unsigned gDiagHead, gDiagCount, gDiagDropped;
static void halLog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void halLog(const char *fmt, ...) {
    char line[240]; va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    pthread_mutex_lock(&gDiagLock);
    if (gDiagCount == DIAG_LINES) gDiagDropped++;
    else { memcpy(gDiag[(gDiagHead + gDiagCount) % DIAG_LINES], line, sizeof line); gDiagCount++; }
    pthread_mutex_unlock(&gDiagLock);
}
static void flushDiag(void) {
    pthread_mutex_lock(&gDiagLock); unsigned pending = gDiagCount; pthread_mutex_unlock(&gDiagLock);
    if (!pending) return;
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(X9_AUDIO_CLASS));
    if (!service) return;
    io_connect_t connection = 0;
    kern_return_t e = IOServiceOpen(service, mach_task_self(), 0, &connection);
    IOObjectRelease(service);
    if (e) return;
    for (;;) {
        char line[240];
        pthread_mutex_lock(&gDiagLock);
        if (!gDiagCount) { pthread_mutex_unlock(&gDiagLock); break; }
        memcpy(line, gDiag[gDiagHead], sizeof line); gDiagHead = (gDiagHead + 1) % DIAG_LINES; gDiagCount--;
        unsigned dropped = gDiagDropped; gDiagDropped = 0;
        pthread_mutex_unlock(&gDiagLock);
        if (dropped) { char d[64]; int n = snprintf(d, sizeof d, "(%u lines dropped)", dropped);
            IOConnectCallStructMethod(connection, X9AudioHalLogSelector, d, (size_t)n, NULL, NULL); }
        size_t n = strnlen(line, sizeof line);
        if (n && IOConnectCallStructMethod(connection, X9AudioHalLogSelector, line, n, NULL, NULL)) break;
    }
    IOServiceClose(connection);
}
static void fourcc(UInt32 v, char out[5]) {
    for (int i = 0; i < 4; i++) { char ch = (char)(v >> (24 - 8 * i)); out[i] = ch >= 0x20 && ch < 0x7f ? ch : '?'; }
    out[4] = 0;
}
/* Distinct property accesses (object, selector, scope, kind, result), in
 * the order the audio server made them, packed several per line. */
#define TRACE_MAX 400u
static struct { UInt32 sel, scope; AudioObjectID obj; UInt8 kind; OSStatus r; } gTrace[TRACE_MAX];
static unsigned gTraceCount;
static char gTraceLine[200]; static unsigned gTraceUsed;
static void traceProperty(AudioObjectID o, const AudioObjectPropertyAddress *a, UInt8 kind, OSStatus r) {
    if (!a) return;
    pthread_mutex_lock(&gDiagLock);
    bool seen = false;
    for (unsigned i = 0; i < gTraceCount && !seen; i++)
        seen = gTrace[i].sel == a->mSelector && gTrace[i].scope == a->mScope && gTrace[i].obj == o &&
               gTrace[i].kind == kind && (gTrace[i].r == 0) == (r == 0);
    if (!seen && gTraceCount < TRACE_MAX) {
        gTrace[gTraceCount].sel = a->mSelector; gTrace[gTraceCount].scope = a->mScope; gTrace[gTraceCount].obj = o;
        gTrace[gTraceCount].kind = kind; gTrace[gTraceCount].r = r; gTraceCount++;
        char sel[5], sc[5]; fourcc(a->mSelector, sel); fourcc(a->mScope, sc);
        int n = snprintf(gTraceLine + gTraceUsed, sizeof gTraceLine - gTraceUsed, " %u%s:%s/%s%s",
                         (unsigned)o, kind ? "S" : "", sel, sc, r ? (r == kAudioHardwareUnknownPropertyError ? "?" : "!") : "");
        if (n > 0) gTraceUsed += (unsigned)n;
        if (gTraceUsed > 150) {
            pthread_mutex_unlock(&gDiagLock); halLog("Q%s", gTraceLine); pthread_mutex_lock(&gDiagLock);
            gTraceUsed = 0; gTraceLine[0] = 0;
        }
    }
    pthread_mutex_unlock(&gDiagLock);
}
static void traceFlushLine(void) {
    pthread_mutex_lock(&gDiagLock);
    char line[200]; bool have = gTraceUsed > 0;
    if (have) { memcpy(line, gTraceLine, sizeof line); gTraceUsed = 0; gTraceLine[0] = 0; }
    pthread_mutex_unlock(&gDiagLock);
    if (have) halLog("Q%s", line);
}
static Float32 volume(void) {
    uint32_t v = __atomic_load_n(&gVolumeBits, __ATOMIC_ACQUIRE);
    Float32 f; memcpy(&f, &v, 4); return f;
}
/* Caller holds gLock. Never spin indefinitely on a preempted kernel writer. */
static int snapshot(X9AudioControl *s) {
    if (!gControl) return -1;
    uint32_t seq = __atomic_load_n(&gControl->seq, __ATOMIC_ACQUIRE);
    if (seq & 1) return -1;
    memcpy(s, gControl, sizeof(*s));
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return s->seq == seq && seq == __atomic_load_n(&gControl->seq, __ATOMIC_ACQUIRE) ? 0 : -1;
}
static bool controlValid(const X9AudioControl *s) {
    return s->magic == X9_AUDIO_MAGIC && s->version == X9_AUDIO_VERSION &&
        s->rate == X9_AUDIO_RATE && s->channels == X9_AUDIO_CHANNELS &&
        s->ring_frames == X9_AUDIO_RING && s->period_frames == X9_AUDIO_PERIOD &&
        s->ticks_per_frame_q32 && s->lead_frames >= X9_AUDIO_PREFILL_MIN &&
        s->lead_frames <= X9_AUDIO_PREFILL_MAX && !(s->lead_frames % X9_AUDIO_PERIOD) &&
        !(s->state & ~(X9AudioReady | X9AudioRunning | X9AudioFault)) &&
        !(s->seq & 1) && s->out_frames <= s->out_queued;
}
static bool controlReady(const X9AudioControl *s) {
    return controlValid(s) && (s->state & X9AudioReady) && !(s->state & X9AudioFault) &&
        !s->blockers && !s->error;
}
/* Passive discovery and explicit StartIO's capability check open type0.
 * Neither maps PCM or acquires the writer; NOWAIT cannot call hardware. */
static kern_return_t probeControl(X9AudioControl *s) {
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(X9_AUDIO_CLASS));
    if (!service) return kIOReturnNotReady;
    io_connect_t connection = 0;
    kern_return_t e = IOServiceOpen(service, mach_task_self(), 0, &connection);
    IOObjectRelease(service);
    if (e) return e;
    size_t n = sizeof(*s);
    e = IOConnectCallStructMethod(connection, X9AudioStatusNowaitSelector, NULL, 0, s, &n);
    kern_return_t closed = IOServiceClose(connection);
    if (!e && n != sizeof(*s)) e = kIOReturnBadArgument;
    return e ? e : closed;
}
static int streamSnapshot(X9AudioControl *s) {
    if (!gIoUsers || gStopFailed || gIOFault || snapshot(s)) return -1;
    if (!controlReady(s) || !(s->state & X9AudioRunning) || s->generation != gGeneration ||
        s->out_anchor_host != gActiveAnchor || s->ticks_per_frame_q32 != gActiveTicks ||
        s->out_frames > 9007199254740991ULL) {
        gIOFault = true; gReady = false; gControlDirty = true; return -1;
    }
    return 0;
}
static UInt32 running(void) {
    pthread_mutex_lock(&gLock); X9AudioControl s;
    UInt32 r = !streamSnapshot(&s);
    pthread_mutex_unlock(&gLock); return r;
}
static UInt32 leadFrames(void) {
    pthread_mutex_lock(&gLock); X9AudioControl s;
    UInt32 r = !snapshot(&s) && controlValid(&s) ? s.lead_frames : gLeadFrames;
    pthread_mutex_unlock(&gLock); return r;
}
static kern_return_t call(uint32_t sel) {
    return gConnect ? IOConnectCallScalarMethod(gConnect, sel, NULL, 0, NULL, NULL) : KERN_FAILURE;
}
static int disconnect(void) {
    gControl = NULL; gProducer = NULL; gOut = NULL;
    int e = 0;
    for (unsigned i = 0; i < 3; i++) {
        if (gMaps[i] && IOConnectUnmapMemory64(gConnect, i, mach_task_self(), gMaps[i])) e = -1;
        else { gMaps[i] = 0; gSizes[i] = 0; }
    }
    if (e) return e;
    if (gConnect && IOServiceClose(gConnect)) return -1;
    gConnect = 0; return 0;
}
static void setHostClock(void);
static int connectService(void) {
    if (gControl) return 0;
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(X9_AUDIO_CLASS));
    if (!service) return -1;
    io_connect_t connection = 0;
    kern_return_t r = IOServiceOpen(service, mach_task_self(), 1, &connection);
    IOObjectRelease(service);
    if (r) return -1;
    gConnect = connection;
    const uint64_t minSizes[] = { sizeof(X9AudioControl), sizeof(X9AudioProducer), X9_AUDIO_RING_BYTES };
    for (unsigned i = 0; i < 3; i++) {
        r = IOConnectMapMemory64(gConnect, i, mach_task_self(), &gMaps[i], &gSizes[i],
                                kIOMapAnywhere | (i == 0 ? kIOMapReadOnly : 0));
        if (r || gSizes[i] < minSizes[i]) { gStopFailed = disconnect() != 0; return -1; }
    }
    X9AudioControl s; size_t n = sizeof(s);
    r = IOConnectCallStructMethod(gConnect, X9AudioStatus, NULL, 0, &s, &n);
    if (r || n != sizeof(s) || !controlReady(&s) || (s.state & X9AudioRunning)) {
        gStopFailed = disconnect() != 0; return -1;
    }
    gControl = (void *)(uintptr_t)gMaps[0]; gProducer = (void *)(uintptr_t)gMaps[1]; gOut = (void *)(uintptr_t)gMaps[2];
    setHostClock(); /* each session: the shared page may be re-read from disk */
    return 0;
}
/* CoreAudio assumes a 24 MHz host clock (AudioGetHostClockFrequency loads a
 * constant with adrp+ldr d0 from the shared cache); X9's timer is 1 GHz, so
 * the IO thread's cycle timing would be off by ~42x. The constant lives on a
 * shared-cache page this process cannot write: pass its address to the
 * kernel, which rewrites it (UniXNU-sheng ShengAudioSetHostClock). The second
 * scalar carries the two instruction words so the kernel log explains a
 * pattern mismatch (address 0). Caller holds gLock with gConnect open. */
static void setHostClock(void) {
    if (!gConnect) return;
    const uintptr_t symbol = (uintptr_t)dlsym(RTLD_DEFAULT, "AudioGetHostClockFrequency") & (((uintptr_t)1 << 47) - 1);
    uint64_t in[2] = {0, 0};
    if (symbol) {
        const uint32_t *code = (const uint32_t *)symbol, adrp = code[0], ldr = code[1];
        in[1] = (uint64_t)ldr << 32 | adrp;
        if ((adrp & 0x9f000000u) == 0x90000000u && (ldr & 0xffc00000u) == 0xfd400000u && ((ldr >> 5) & 31) == (adrp & 31)) {
            int64_t page = (int64_t)(((adrp >> 5) & 0x7ffffu) << 2 | ((adrp >> 29) & 3u));
            if (page & (1 << 20)) page -= 1 << 21;
            in[0] = ((uintptr_t)code & ~(uintptr_t)0xfff) + (uint64_t)(page << 12) + ((ldr >> 10) & 0xfffu) * 8;
        }
    }
    (void)IOConnectCallScalarMethod(gConnect, X9AudioSetHostClockSelector, in, 2, NULL, NULL);
}
static HRESULT QueryInterface(void *, REFIID, LPVOID *);
static ULONG AddRef(void *self) { (void)self; return atomic_fetch_add(&gRefs, 1) + 1; }
static ULONG Release(void *self) {
    (void)self; unsigned refs = atomic_load(&gRefs);
    /* The static driver/control observer have one process-lifetime reference,
     * like the microphone HAL. Never advertise unload while timer code lives. */
    while (refs > 1 && !atomic_compare_exchange_weak(&gRefs, &refs, refs - 1)) {}
    return refs > 1 ? refs - 1 : refs;
}
static OSStatus Initialize(AudioServerPlugInDriverRef d, AudioServerPlugInHostRef host) {
    (void)d;
    if (!host || !host->PropertiesChanged) return kAudioHardwareIllegalOperationError;
    pthread_mutex_lock(&gLock);
    if (gHost) {
        OSStatus e = gHost == host ? 0 : kAudioHardwareIllegalOperationError;
        pthread_mutex_unlock(&gLock); return e;
    }
    dispatch_queue_t queue = dispatch_queue_create("org.ios-sweet.x9.audio.status", NULL);
    if (!queue) { pthread_mutex_unlock(&gLock); return kAudioHardwareUnspecifiedError; }
    gControlTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue);
    if (!gControlTimer) {
        dispatch_release(queue); pthread_mutex_unlock(&gLock); return kAudioHardwareUnspecifiedError;
    }
    X9AudioControl s;
    kern_return_t pe = probeControl(&s);
    gReady = !pe && controlReady(&s) && !(s.state & X9AudioRunning);
    { mach_timebase_info_data_t tb; mach_timebase_info(&tb);
      gHostTicksPerFrame = 1e9 * (double)tb.denom / (double)tb.numer / kRate; }
    halLog("init uid=%s+" kHawkingUID " probe=%x ready=%d state=%x lead=%u", kDeviceUID, pe, gReady, pe ? 0 : s.state, pe ? 0 : s.lead_frames);
    if (gReady) gLeadFrames = s.lead_frames;
    gHost = host;
    dispatch_source_set_event_handler_f(gControlTimer, refreshControlState);
    dispatch_source_set_timer(gControlTimer, dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC),
                             500 * NSEC_PER_MSEC, 50 * NSEC_PER_MSEC);
    pthread_mutex_unlock(&gLock);
    dispatch_resume(gControlTimer); dispatch_release(queue); return 0;
}
static OSStatus CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef x, const AudioServerPlugInClientInfo *c, AudioObjectID *o)
{ (void)d; (void)x; (void)c; (void)o; return kAudioHardwareUnsupportedOperationError; }
static OSStatus DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID o)
{ (void)d; (void)o; return kAudioHardwareUnsupportedOperationError; }
static unsigned findClient(UInt32 id) {
    for (unsigned i = 0; i < X9_AUDIO_HAL_CLIENTS; i++)
        if (gClients[i].used && gClients[i].id == id) return i;
    return X9_AUDIO_HAL_CLIENTS;
}
static OSStatus AddDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID o, const AudioServerPlugInClientInfo *c) {
    (void)d;
    if (o == kHawking && c) { halLog("add client %u to Hawking", (unsigned)c->mClientID); return 0; }
    if (o != kDevice || !c) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gLock);
    unsigned slot = findClient(c->mClientID);
    OSStatus e = 0;
    if (slot == X9_AUDIO_HAL_CLIENTS) {
        for (slot = 0; slot < X9_AUDIO_HAL_CLIENTS && gClients[slot].used; slot++) {}
        if (slot == X9_AUDIO_HAL_CLIENTS) e = kAudioHardwareIllegalOperationError;
        else { gClients[slot].id = c->mClientID; gClients[slot].used = true; gClients[slot].running = false; }
    }
    pthread_mutex_unlock(&gLock);
    char bundle[96] = "";
    if (c->mBundleID) CFStringGetCString(c->mBundleID, bundle, sizeof bundle, kCFStringEncodingUTF8);
    halLog("add client %u pid=%d bundle=%s -> %d", (unsigned)c->mClientID, (int)c->mProcessID, bundle, (int)e);
    return e;
}
static OSStatus RemoveDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID o, const AudioServerPlugInClientInfo *c) {
    (void)d;
    if (o == kHawking && c) return 0;
    if (o != kDevice || !c) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gLock);
    unsigned slot = findClient(c->mClientID);
    OSStatus e = slot == X9_AUDIO_HAL_CLIENTS ? kAudioHardwareBadObjectError : stopClient(slot);
    if (slot != X9_AUDIO_HAL_CLIENTS && !e) gClients[slot].used = false;
    pthread_mutex_unlock(&gLock);
    halLog("remove client %u -> %d", (unsigned)c->mClientID, (int)e);
    return e;
}
static OSStatus PerformDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID o, UInt64 a, void *i)
{ (void)d; (void)o; (void)a; (void)i; return kAudioHardwareUnsupportedOperationError; }
static OSStatus AbortDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID o, UInt64 a, void *i)
{ (void)d; (void)o; (void)a; (void)i; return 0; }
static AudioStreamBasicDescription format(UInt32 channels) {
    AudioStreamBasicDescription f = {0};
    f.mSampleRate = kRate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    f.mBytesPerPacket = 4 * channels; f.mFramesPerPacket = 1; f.mBytesPerFrame = 4 * channels;
    f.mChannelsPerFrame = channels; f.mBitsPerChannel = 32;
    return f;
}
#define REPLY(type, value) do { if (inDataSize < sizeof(type)) return kAudioHardwareBadPropertySizeError; *(type *)outData = (value); *outDataSize = sizeof(type); return 0; } while (0)
#define REPLY_STR(text) REPLY(CFStringRef, CFSTR(text))
static UInt32 latencyFrames(void) {return leadFrames();}

static Boolean HasProperty(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a);
static OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a,
                                    UInt32 qs, const void *q, UInt32 *size);
static OSStatus GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a, UInt32 qs,
                                const void *q, UInt32 inDataSize, UInt32 *outDataSize, void *outData);

static OSStatus IsPropertySettable(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a, Boolean *out) {
    (void)d; (void)pid;
    if (!out || !a) return kAudioHardwareIllegalOperationError;
    *out = (o == kVolume && (a->mSelector == kAudioLevelControlPropertyScalarValue || a->mSelector == kAudioLevelControlPropertyDecibelValue)) ||
           ((o == kOutStream || o == kInStream || o == kHawkingIn) && (a->mSelector == kAudioStreamPropertyVirtualFormat || a->mSelector == kAudioStreamPropertyPhysicalFormat ||
                                                     a->mSelector == kAudioStreamPropertyIsActive)) ||
           ((o == kDevice || o == kHawking) && a->mSelector == kAudioDevicePropertyNominalSampleRate);
    return HasProperty(d, o, pid, a) ? 0 : kAudioHardwareUnknownPropertyError;
}

static const Float32 kMinDb = -36.0f;
static Float32 toDb(Float32 scalar) {const Float32 r = 1.0f - (scalar < 0 ? 0 : scalar > 1 ? 1 : scalar); return kMinDb * r * r;}
static Float32 toScalar(Float32 db) {return db <= kMinDb ? 0.0f : db >= 0 ? 1.0f : 1.0f - sqrtf(db / kMinDb);}
static Float32 gain(void) {return volume() <= 0.0f ? 0.0f : powf(10.0f, toDb(volume()) / 20.0f);}

static OSStatus setProperty(AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 size, const void *data) {
    if (o == kVolume && size >= sizeof(Float32)) {
        Float32 v = *(const Float32 *)data;
        if (!isfinite(v)) return kAudioHardwareIllegalOperationError;
        if (a->mSelector == kAudioLevelControlPropertyDecibelValue) v = toScalar(v);
        else if (a->mSelector != kAudioLevelControlPropertyScalarValue) return kAudioHardwareUnknownPropertyError;
        __atomic_store_n(&gVolumeBits, floatBits(v < 0 ? 0 : v > 1 ? 1 : v), __ATOMIC_RELEASE);
        AudioObjectPropertyAddress changed[2] = {{kAudioLevelControlPropertyScalarValue, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain},
                                                 {kAudioLevelControlPropertyDecibelValue, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain}};
        if (gHost) gHost->PropertiesChanged(gHost, kVolume, 2, changed);
        return 0;
    }
    if ((o == kOutStream || o == kInStream || o == kHawkingIn) &&
        (a->mSelector == kAudioStreamPropertyVirtualFormat || a->mSelector == kAudioStreamPropertyPhysicalFormat)) {
        const AudioStreamBasicDescription *f = data;
        if (size < sizeof(*f)) return kAudioHardwareBadPropertySizeError;
        AudioStreamBasicDescription expected = format(o == kHawkingIn ? 1 : X9_AUDIO_CHANNELS);
        return f->mSampleRate == expected.mSampleRate && f->mFormatID == expected.mFormatID &&
            f->mFormatFlags == expected.mFormatFlags && f->mChannelsPerFrame == expected.mChannelsPerFrame &&
            f->mBytesPerFrame == expected.mBytesPerFrame && f->mBitsPerChannel == expected.mBitsPerChannel &&
            f->mFramesPerPacket == 1 && f->mBytesPerPacket == expected.mBytesPerPacket ? 0 : kAudioDeviceUnsupportedFormatError;
    }
    if ((o == kDevice || o == kHawking) && a->mSelector == kAudioDevicePropertyNominalSampleRate)
        return size >= sizeof(Float64) && *(const Float64 *)data == kRate ? 0 : kAudioDeviceUnsupportedFormatError;
    if (o == kOutStream && a->mSelector == kAudioStreamPropertyIsActive) {
        if (size < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
        __atomic_store_n(&gStreamActive, !!*(const UInt32 *)data, __ATOMIC_RELEASE); return 0;
    }
    if ((o == kInStream || o == kHawkingIn) && a->mSelector == kAudioStreamPropertyIsActive)
        return size < sizeof(UInt32) ? kAudioHardwareBadPropertySizeError : 0; /* silent either way */
    return kAudioHardwareUnknownPropertyError;
}

static OSStatus SetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a, UInt32 qs,
                                const void *q, UInt32 size, const void *data) {
    (void)d; (void)pid; (void)qs; (void)q;
    if (!a || !data) return kAudioHardwareIllegalOperationError;
    const OSStatus r = setProperty(o, a, size, data);
    traceProperty(o, a, 1, r);
    return r;
}

static Boolean HasProperty(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a) {
    UInt32 size = 0;
    return GetPropertyDataSize(d, o, pid, a, 0, NULL, &size) == 0;
}

static OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a,
                                    UInt32 qs, const void *q, UInt32 *size) {
    if (!a || !size) return kAudioHardwareIllegalOperationError;
    UInt8 scratch[1024] __attribute__((aligned(16))) = {0};
    UInt32 written = 0;
    OSStatus r = GetPropertyData(d, o, pid, a, qs, q, sizeof scratch, &written, scratch);
    if (r == 0) *size = written;
    return r;
}

static UInt32 objects(AudioObjectID o, AudioObjectPropertyScope scope, AudioObjectID *list) {
    UInt32 n = 0;
    /* Always listed (Sheng): VirtualAudio builds the Puffin port when the
     * device appears; StartIO reports NotRunning until the kernel is Ready. */
    if (o == kPlugIn) { list[n++] = kDevice; list[n++] = kHawking; }
    else if (o == kDevice) {
        if (scope != kAudioObjectPropertyScopeInput) list[n++] = kOutStream;
        if (scope != kAudioObjectPropertyScopeOutput) list[n++] = kInStream;
        if (scope != kAudioObjectPropertyScopeInput) list[n++] = kVolume;
    } else if (o == kHawking) {
        if (scope != kAudioObjectPropertyScopeOutput) list[n++] = kHawkingIn;
    }
    return n;
}

static OSStatus propertyData(AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 inDataSize,
                             UInt32 *outDataSize, void *outData) {
    if (!a || !outDataSize || (!outData && inDataSize) || (!q && qs)) return kAudioHardwareIllegalOperationError;
    const AudioObjectPropertySelector s = a->mSelector;
    if (o <= kLastObject && (s == kAudioObjectPropertyCustomPropertyInfoList || ((s == kAudioPlugInPropertyBoxList ||
        s == kAudioPlugInPropertyClockDeviceList) && o == kPlugIn) ||
        ((s == 'taps') && (o == kOutStream || o == kInStream || o == kHawkingIn)))) {
        *outDataSize = 0;
        return 0;
    }
    switch (o) {
    case kPlugIn:
        switch (s) {
        case kAudioObjectPropertyBaseClass: REPLY(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: REPLY(AudioClassID, kAudioPlugInClassID);
        case kAudioObjectPropertyOwner: REPLY(AudioObjectID, kAudioObjectUnknown);
        case kAudioObjectPropertyManufacturer: REPLY_STR("ios-sweet");
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList: {
            AudioObjectID list[2]; UInt32 n = objects(kPlugIn, a->mScope, list), out = 0;
            for (UInt32 i = 0; i < n && (out + 1) * sizeof(AudioObjectID) <= inDataSize; i++)
                ((AudioObjectID *)outData)[out++] = list[i];
            *outDataSize = out * (UInt32)sizeof(AudioObjectID); return 0;
        }
        case kAudioPlugInPropertyTranslateUIDToDevice: {
            CFStringRef uid = qs >= sizeof(CFStringRef) ? *(const CFStringRef *)q : NULL;
            bool str = uid && CFGetTypeID(uid) == CFStringGetTypeID();
            REPLY(AudioObjectID, str && CFStringCompare(uid, CFSTR(kDeviceUID), 0) == kCFCompareEqualTo ? kDevice :
                  str && CFStringCompare(uid, CFSTR(kHawkingUID), 0) == kCFCompareEqualTo ? kHawking : kAudioObjectUnknown);
        }
        case kAudioPlugInPropertyResourceBundle: REPLY_STR("");
        default: return kAudioHardwareUnknownPropertyError;
        }
    case kDevice:
        switch (s) {
        case kAudioObjectPropertyBaseClass: REPLY(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: REPLY(AudioClassID, kAudioDeviceClassID);
        case kAudioObjectPropertyOwner: REPLY(AudioObjectID, kPlugIn);
        case kAudioObjectPropertyName: REPLY_STR(kDeviceUID);
        case kAudioObjectPropertyManufacturer: REPLY_STR("ios-sweet");
        case kAudioDevicePropertyDeviceUID: REPLY_STR(kDeviceUID);
        case kAudioDevicePropertyModelUID: REPLY_STR(kModelUID);
        case kAudioObjectPropertyModelName: REPLY_STR(kDeviceUID);
        case kAudioObjectPropertySerialNumber: REPLY_STR("0001");
        case kAudioObjectPropertyFirmwareVersion: REPLY_STR("1.0");
        case kAudioObjectPropertyIdentify: REPLY(UInt32, 0);
        case kAudioObjectPropertyElementName: REPLY_STR("");
        case kAudioDevicePropertyConfigurationApplication: REPLY_STR("com.apple.Preferences");
        case kAudioDevicePropertyTransportType: REPLY(UInt32, kAudioDeviceTransportTypeBuiltIn);
        case kAudioDevicePropertyRelatedDevices: REPLY(AudioObjectID, kDevice);
        case kAudioDevicePropertyClockDomain: REPLY(UInt32, 0);
        case kAudioDevicePropertyDeviceIsAlive: REPLY(UInt32, 1);
        case kAudioDevicePropertyDeviceIsRunning: REPLY(UInt32, running());
        /* HakiOS (tested D22): 1 in every scope for a device with an output. */
        case kAudioDevicePropertyDeviceCanBeDefaultDevice: REPLY(UInt32, 1);
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: REPLY(UInt32, 1);
        case kAudioDevicePropertyLatency: REPLY(UInt32, latencyFrames());
        case kAudioDevicePropertySafetyOffset: REPLY(UInt32, latencyFrames() + 2 * X9_AUDIO_PERIOD);
        case kAudioDevicePropertyNominalSampleRate: REPLY(Float64, kRate);
        case kAudioDevicePropertyAvailableNominalSampleRates: REPLY(AudioValueRange, ((AudioValueRange){kRate, kRate}));
        case kAudioDevicePropertyIsHidden: REPLY(UInt32, 0);
        case kAudioDevicePropertyZeroTimeStampPeriod: REPLY(UInt32, kZeroPeriod);
        case kAudioDevicePropertyClockIsStable: REPLY(UInt32, 1);
        case kAudioDevicePropertyClockAlgorithm: REPLY(UInt32, kAudioDeviceClockAlgorithmRaw);
        case kAudioDevicePropertyPreferredChannelsForStereo: {
            if (inDataSize < 2 * sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            ((UInt32 *)outData)[0] = 1; ((UInt32 *)outData)[1] = 2;
            *outDataSize = 2 * sizeof(UInt32); return 0;
        }
        case kAudioDevicePropertyPreferredChannelLayout: {
            const UInt32 channels = X9_AUDIO_CHANNELS;
            const UInt32 bytes = (UInt32)(offsetof(AudioChannelLayout, mChannelDescriptions) + channels * sizeof(AudioChannelDescription));
            if (inDataSize < bytes) return kAudioHardwareBadPropertySizeError;
            AudioChannelLayout *l = outData;
            memset(l, 0, bytes);
            l->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
            l->mNumberChannelDescriptions = channels;
            if (channels) l->mChannelDescriptions[0].mChannelLabel = kAudioChannelLabel_Left;
            if (channels == 2) l->mChannelDescriptions[1].mChannelLabel = kAudioChannelLabel_Right;
            *outDataSize = bytes; return 0;
        }
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyStreams:
        case kAudioObjectPropertyControlList: {
            AudioObjectID list[4];
            UInt32 n = objects(kDevice, a->mScope, list), out = 0;
            for (UInt32 i = 0; i < n; ++i) {
                if (s == kAudioDevicePropertyStreams && list[i] == kVolume) continue;
                if (s == kAudioObjectPropertyControlList && list[i] != kVolume) continue;
                if ((out + 1) * sizeof(AudioObjectID) > inDataSize) break;
                ((AudioObjectID *)outData)[out++] = list[i];
            }
            *outDataSize = out * (UInt32)sizeof(AudioObjectID); return 0;
        }
        default: return kAudioHardwareUnknownPropertyError;
        }
    case kHawking:
        switch (s) {
        case kAudioObjectPropertyBaseClass: REPLY(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: REPLY(AudioClassID, kAudioDeviceClassID);
        case kAudioObjectPropertyOwner: REPLY(AudioObjectID, kPlugIn);
        case kAudioObjectPropertyName: REPLY_STR(kHawkingUID);
        case kAudioObjectPropertyManufacturer: REPLY_STR("ios-sweet");
        case kAudioDevicePropertyDeviceUID: REPLY_STR(kHawkingUID);
        case kAudioDevicePropertyModelUID: REPLY_STR(kHawkingUID);
        case kAudioDevicePropertyTransportType: REPLY(UInt32, kAudioDeviceTransportTypeBuiltIn);
        case kAudioDevicePropertyRelatedDevices: REPLY(AudioObjectID, kHawking);
        case kAudioDevicePropertyClockDomain: REPLY(UInt32, 0);
        case kAudioDevicePropertyDeviceIsAlive: REPLY(UInt32, 1);
        case kAudioDevicePropertyDeviceIsRunning: REPLY(UInt32, __atomic_load_n(&gHawkUsers, __ATOMIC_ACQUIRE) != 0);
        case kAudioDevicePropertyDeviceCanBeDefaultDevice: REPLY(UInt32, 0);
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: REPLY(UInt32, 0);
        case kAudioDevicePropertyLatency: REPLY(UInt32, 0);
        case kAudioDevicePropertySafetyOffset: REPLY(UInt32, 0);
        case kAudioDevicePropertyNominalSampleRate: REPLY(Float64, kRate);
        case kAudioDevicePropertyAvailableNominalSampleRates: REPLY(AudioValueRange, ((AudioValueRange){kRate, kRate}));
        case kAudioDevicePropertyIsHidden: REPLY(UInt32, 0);
        case kAudioDevicePropertyZeroTimeStampPeriod: REPLY(UInt32, kZeroPeriod);
        case kAudioDevicePropertyPreferredChannelsForStereo: {
            if (inDataSize < 2 * sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            ((UInt32 *)outData)[0] = 1; ((UInt32 *)outData)[1] = 2;
            *outDataSize = 2 * sizeof(UInt32); return 0;
        }
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyStreams: {
            AudioObjectID list[1]; UInt32 n = objects(kHawking, a->mScope, list);
            if (n && inDataSize >= sizeof(AudioObjectID)) {((AudioObjectID *)outData)[0] = list[0]; *outDataSize = sizeof(AudioObjectID);}
            else *outDataSize = 0;
            return 0;
        }
        case kAudioObjectPropertyControlList: *outDataSize = 0; return 0;
        default: return kAudioHardwareUnknownPropertyError;
        }
    case kOutStream:
    case kInStream:
    case kHawkingIn:
    {
        const UInt32 channels = o == kHawkingIn ? 1 : X9_AUDIO_CHANNELS;
        const bool input = o != kOutStream;
        switch (s) {
        case kAudioObjectPropertyBaseClass: REPLY(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: REPLY(AudioClassID, kAudioStreamClassID);
        case kAudioObjectPropertyOwner: REPLY(AudioObjectID, o == kHawkingIn ? kHawking : kDevice);
        case kAudioObjectPropertyName: if (o == kOutStream) REPLY_STR("Speaker"); REPLY_STR("Feedback");
        case kAudioObjectPropertyOwnedObjects: *outDataSize = 0; return 0;
        case kAudioStreamPropertyIsActive: REPLY(UInt32, input ? 1 : __atomic_load_n(&gStreamActive, __ATOMIC_ACQUIRE));
        case kAudioStreamPropertyDirection: REPLY(UInt32, input);
        case kAudioStreamPropertyTerminalType: REPLY(UInt32, input ? kAudioStreamTerminalTypeMicrophone : kAudioStreamTerminalTypeSpeaker);
        case kAudioStreamPropertyStartingChannel: REPLY(UInt32, 1);
        case kAudioStreamPropertyLatency: REPLY(UInt32, 0);
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat: REPLY(AudioStreamBasicDescription, format(channels));
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats: {
            AudioStreamRangedDescription r = {format(channels), {kRate, kRate}};
            REPLY(AudioStreamRangedDescription, r);
        }
        default: return kAudioHardwareUnknownPropertyError;
        }
    }
    case kVolume:
        switch (s) {
        case kAudioObjectPropertyBaseClass: REPLY(AudioClassID, kAudioLevelControlClassID);
        case kAudioObjectPropertyClass: REPLY(AudioClassID, kAudioVolumeControlClassID);
        case kAudioObjectPropertyOwner: REPLY(AudioObjectID, kDevice);
        case kAudioObjectPropertyOwnedObjects: *outDataSize = 0; return 0;
        case kAudioControlPropertyScope: REPLY(AudioObjectPropertyScope, kAudioObjectPropertyScopeOutput);
        case kAudioControlPropertyElement: REPLY(AudioObjectPropertyElement, kAudioObjectPropertyElementMain);
        case kAudioLevelControlPropertyScalarValue: REPLY(Float32, volume());
        case kAudioLevelControlPropertyDecibelValue: REPLY(Float32, toDb(volume()));
        case kAudioLevelControlPropertyDecibelRange: REPLY(AudioValueRange, ((AudioValueRange){kMinDb, 0.0}));
        case kAudioLevelControlPropertyConvertScalarToDecibels:
            if (inDataSize < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
            *(Float32 *)outData = toDb(*(Float32 *)outData); *outDataSize = sizeof(Float32); return 0;
        case kAudioLevelControlPropertyConvertDecibelsToScalar:
            if (inDataSize < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
            *(Float32 *)outData = toScalar(*(Float32 *)outData); *outDataSize = sizeof(Float32); return 0;
        default: return kAudioHardwareUnknownPropertyError;
        }
    default:
        return kAudioHardwareBadObjectError;
    }
}

static OSStatus GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a,
    UInt32 qs, const void *q, UInt32 capacity, UInt32 *size, void *out) {
    (void)d; (void)pid;
    OSStatus r = propertyData(o, a, qs, q, capacity, size, out);
    traceProperty(o, a, 0, r);
    return r;
}
static void refreshControlState(void *context) {
    (void)context;
    traceFlushLine(); flushDiag(); /* diagnostics first: never behind gLock */
    /* No lifecycle call or writer connection on this queue. Do not wait for
     * realtime/control callbacks holding gLock, or kernel physical work. */
    if (pthread_mutex_trylock(&gLock)) return;
    bool wasReady = gReady;
    uint32_t wasLead = gLeadFrames;
    X9AudioControl s;
    kern_return_t e = gControl ? (snapshot(&s) ? kIOReturnBusy : 0) : probeControl(&s);
    if (e != kIOReturnBusy) {
        gReady = !e && controlReady(&s) && !gStopFailed && !gIOFault &&
            (gIoUsers ? (s.state & X9AudioRunning) && s.generation == gGeneration &&
                s.out_anchor_host == gActiveAnchor && s.ticks_per_frame_q32 == gActiveTicks &&
                s.out_frames <= 9007199254740991ULL : !(s.state & X9AudioRunning));
        if (gIoUsers && !gReady) gIOFault = true;
        if (gReady) gLeadFrames = s.lead_frames;
    }
    bool changed = wasReady != gReady || wasLead != gLeadFrames || gControlDirty;
    if (wasReady != gReady) halLog("kernel ready %d -> %d (probe=%x io=%u fault=%d stopfail=%d)", wasReady, gReady, e, gIoUsers, gIOFault, gStopFailed);
    gControlDirty = false;
    AudioServerPlugInHostRef host = gHost;
    pthread_mutex_unlock(&gLock);
    if (!changed || !host) return;
    /* Notify only from the non-realtime control queue, outside HAL lock.
     * Publish the containing list before the device/stream properties. */
    const AudioObjectPropertyAddress plugin[] = {
        {kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain},
        {kAudioObjectPropertyOwnedObjects, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain}};
    const AudioObjectPropertyAddress device[] = {
        {kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain},
        {kAudioDevicePropertyDeviceIsRunning, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain},
        {kAudioDevicePropertyClockIsStable, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain},
        {kAudioDevicePropertyDeviceCanBeDefaultDevice, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain},
        {kAudioDevicePropertyDeviceCanBeDefaultSystemDevice, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain},
        {kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain},
        {kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain}};
    AudioObjectPropertyAddress stream = {kAudioStreamPropertyIsActive, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    host->PropertiesChanged(host, kPlugIn, 2, plugin);
    host->PropertiesChanged(host, kDevice, 7, device);
    host->PropertiesChanged(host, kOutStream, 1, &stream);
}
static OSStatus stopDevice(void) {
    if (!gConnect) return gStopFailed ? kAudioHardwareUnspecifiedError : 0;
    /* STOP success must be verified at the authoritative physical-held
     * surface. Running=0 or a decremented HAL refcount is not cleanup. */
    X9AudioStreamStatus lease = {0}; X9AudioControl before, s;
    size_t n = sizeof(lease), bytes = sizeof(s);
    bool baseline = !IOConnectCallStructMethod(gConnect, X9AudioStatus, NULL, 0, &before, &bytes) &&
        bytes == sizeof(before) && controlValid(&before);
    bytes = sizeof(s);
    bool stopped = !call(X9AudioStop); /* attempt even if baseline could not be read */
    stopped = stopped && baseline &&
        !IOConnectCallStructMethod(gConnect, X9AudioStreamStatusSelector, NULL, 0, &lease, &n) &&
        n == sizeof(lease) && lease.version == X9_AUDIO_STREAM_STATUS_VERSION &&
        !lease.backend_held && !lease.stop_error &&
        !IOConnectCallStructMethod(gConnect, X9AudioStatus, NULL, 0, &s, &bytes) &&
        bytes == sizeof(s) && controlValid(&s) && s.generation == before.generation && !(s.state & X9AudioRunning);
    if (!stopped) {
        gStopFailed = 1; gIOFault = true; gReady = false; gControlDirty = true;
        return kAudioHardwareUnspecifiedError; /* retain original maps/connection */
    }
    gStopFailed = disconnect() != 0;
    gReady = !gStopFailed && controlReady(&s);
    gIOFault = !gReady; gControlDirty = true;
    return gStopFailed ? kAudioHardwareUnspecifiedError : 0;
}
static OSStatus stopClient(unsigned slot) {
    if (gClients[slot].running) {
        gClients[slot].running = false; gIoUsers--; gControlDirty = true;
    }
    return !gIoUsers ? stopDevice() : 0;
}
static OSStatus StartIO(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client) {
    (void)d;
    if (o == kHawking) {
        pthread_mutex_lock(&gLock);
        if (!gHawkUsers) gHawkAnchor = mach_absolute_time();
        __atomic_store_n(&gHawkUsers, gHawkUsers + 1, __ATOMIC_RELEASE);
        pthread_mutex_unlock(&gLock);
        halLog("StartIO Hawking client %u", (unsigned)client);
        return 0;
    }
    if (o != kDevice) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gLock);
    OSStatus r = 0;
    unsigned slot = findClient(client);
    if (slot == X9_AUDIO_HAL_CLIENTS) { r = kAudioHardwareBadObjectError; halLog("StartIO client %u: not added", (unsigned)client); }
    else if (!gIoUsers) {
        X9AudioControl before, after; memset(&before, 0, sizeof before); memset(&after, 0, sizeof after);
        kern_return_t pe = 0, se = 0; int why = 0;
        if (gStopFailed) why = 1;
        else if ((pe = probeControl(&before))) why = 2;
        else if (!controlReady(&before)) why = 3;
        else if (before.state & X9AudioRunning) why = 4;
        else if (before.generation == UINT32_MAX) why = 5;
        else if (connectService()) why = 6;
        else if (snapshot(&before) || !controlReady(&before) || (before.state & X9AudioRunning)) why = 7;
        else if ((se = call(X9AudioStart))) why = 8;
        else if (snapshot(&after) || !controlReady(&after) || !(after.state & X9AudioRunning)) why = 9;
        else if (after.generation != before.generation + 1 || !after.out_anchor_host ||
                 after.ticks_per_frame_q32 != before.ticks_per_frame_q32 || after.lead_frames != before.lead_frames) why = 10;
        halLog("StartIO client %u: why=%d probe=%x start=%x state=%x->%x blockers=%x error=%u gen=%u->%u",
               (unsigned)client, why, pe, se, before.state, after.state, before.blockers, before.error,
               before.generation, after.generation);
        if (why) {
            r = kAudioHardwareNotRunningError;
        } else {
            gGeneration = after.generation; gActiveAnchor = after.out_anchor_host;
            gActiveTicks = after.ticks_per_frame_q32; gLeadFrames = after.lead_frames;
            gIOFault = false; gReady = true;
        }
        if (r) { gReady = false; gControlDirty = true; if (gConnect) stopDevice(); }
    } else {
        X9AudioControl s;
        if (streamSnapshot(&s)) r = kAudioHardwareNotRunningError;
    }
    if (!r && !gClients[slot].running) { gClients[slot].running = true; gIoUsers++; gControlDirty = true; }
    pthread_mutex_unlock(&gLock); return r;
}
static OSStatus StopIO(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client) {
    (void)d;
    if (o == kHawking) {
        pthread_mutex_lock(&gLock);
        if (gHawkUsers) __atomic_store_n(&gHawkUsers, gHawkUsers - 1, __ATOMIC_RELEASE);
        pthread_mutex_unlock(&gLock);
        return 0;
    }
    if (o != kDevice) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gLock);
    unsigned slot = findClient(client);
    OSStatus r = slot == X9_AUDIO_HAL_CLIENTS ? kAudioHardwareBadObjectError : stopClient(slot);
    pthread_mutex_unlock(&gLock);
    halLog("StopIO client %u -> %d", (unsigned)client, (int)r);
    return r;
}
static OSStatus GetZeroTimeStamp(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client,
    Float64 *sample, UInt64 *host, UInt64 *seed) {
    (void)d;
    if (o == kHawking && sample && host && seed) {
        (void)client;
        if (!__atomic_load_n(&gHawkUsers, __ATOMIC_ACQUIRE) || gHostTicksPerFrame <= 0) return kAudioHardwareNotRunningError;
        uint64_t now = mach_absolute_time(), anchor = gHawkAnchor;
        uint64_t frames = now > anchor ? (uint64_t)((double)(now - anchor) / gHostTicksPerFrame) : 0;
        uint64_t period = frames / kZeroPeriod * kZeroPeriod;
        *sample = (Float64)period;
        *host = anchor + (uint64_t)((double)period * gHostTicksPerFrame);
        *seed = 1; return 0;
    }
    if (o != kDevice || !sample || !host || !seed) return kAudioHardwareBadObjectError;
    if (pthread_mutex_trylock(&gLock)) return kAudioHardwareNotRunningError;
    X9AudioControl s;
    OSStatus r = kAudioHardwareNotRunningError;
    unsigned slot = findClient(client);
    if (slot != X9_AUDIO_HAL_CLIENTS && gClients[slot].running && !streamSnapshot(&s)) {
        uint64_t frame = s.out_frames / kZeroPeriod * kZeroPeriod;
        __uint128_t ticks = ((__uint128_t)frame * s.ticks_per_frame_q32) >> 32;
        if (ticks <= UINT64_MAX - s.out_anchor_host) {
            *sample = (Float64)frame;
            *host = s.out_anchor_host + (uint64_t)ticks;
            *seed = s.generation; r = 0;
        }
    }
    pthread_mutex_unlock(&gLock); return r;
}
static OSStatus WillDoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 c, UInt32 op, Boolean *will, Boolean *inPlace) {
    (void)d; (void)c;
    if ((o != kDevice && o != kHawking) || !will || !inPlace) return kAudioHardwareBadObjectError;
    *will = op == kAudioServerPlugInIOOperationReadInput ||
            (o == kDevice && op == kAudioServerPlugInIOOperationWriteMix);
    *inPlace = true; return 0;
}
static OSStatus BeginIOOperation(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 c, UInt32 op, UInt32 n, const AudioServerPlugInIOCycleInfo *i)
{ (void)d; (void)o; (void)c; (void)op; (void)n; (void)i; return 0; }
static int16_t quantize(float v) {
    if (!isfinite(v)) return 0;
    v *= 32767.0f; return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}
static OSStatus DoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID o, AudioObjectID stream, UInt32 c, UInt32 op,
    UInt32 frames, const AudioServerPlugInIOCycleInfo *info, void *main, void *secondary) {
    (void)d; (void)secondary;
    /* Speaker's feedback input and Hawking's input: silence. */
    if ((o == kDevice && stream == kInStream) || (o == kHawking && stream == kHawkingIn)) {
        if (op != kAudioServerPlugInIOOperationReadInput) return kAudioHardwareUnsupportedOperationError;
        if (frames > X9_AUDIO_RING || (!main && frames)) return kAudioHardwareIllegalOperationError;
        if (frames) memset(main, 0, (size_t)frames * (stream == kHawkingIn ? 1 : X9_AUDIO_CHANNELS) * sizeof(Float32));
        return 0;
    }
    if (o != kDevice || stream != kOutStream) return kAudioHardwareBadObjectError;
    if (op != kAudioServerPlugInIOOperationWriteMix) return kAudioHardwareUnsupportedOperationError;
    if (!info || (!main && frames) || frames > X9_AUDIO_RING ||
        !(info->mOutputTime.mFlags & kAudioTimeStampSampleTimeValid)) return kAudioHardwareIllegalOperationError;
    double t = info->mOutputTime.mSampleTime;
    if (!isfinite(t) || t < 0 || t > 9007199254740991.0 || t != floor(t)) return kAudioHardwareIllegalOperationError;
    if (pthread_mutex_trylock(&gLock)) return kAudioHardwareNotRunningError;
    X9AudioControl s;
    OSStatus r = kAudioHardwareNotRunningError;
    unsigned clientSlot = findClient(c);
    if (clientSlot != X9_AUDIO_HAL_CLIENTS && gClients[clientSlot].running && !streamSnapshot(&s)) {
        if (!frames) { pthread_mutex_unlock(&gLock); return 0; }
        uint64_t start = (uint64_t)t, written = __atomic_load_n(&gProducer->written, __ATOMIC_ACQUIRE);
        /* Do not overwrite a queued slot or alias a slot in a future ring turn.
         * Explicit zeroing of gaps prevents stale PCM replay after wrap. */
        if (start <= UINT64_MAX - X9_AUDIO_PREFILL_MAX - frames &&
            start >= written && start >= s.out_queued && start - s.out_queued <= X9_AUDIO_RING &&
            frames <= X9_AUDIO_RING - (start - s.out_queued) && written >= s.out_queued &&
            written - s.out_queued <= X9_AUDIO_RING) {
            for (uint64_t f = written; f < start; f++) {
                gOut[(f % X9_AUDIO_RING) * 2] = 0; gOut[(f % X9_AUDIO_RING) * 2 + 1] = 0;
            }
            const Float32 *src = main;
            Float32 g = __atomic_load_n(&gStreamActive, __ATOMIC_ACQUIRE) ? gain() : 0;
            for (UInt32 f = 0; f < frames; f++) {
                uint64_t slot = ((start + f) % X9_AUDIO_RING) * 2;
                /* Committed frames belong to the kernel until consumed. An
                 * overlapping second IO context is rejected above, not mixed
                 * into PCM which DMA may already be reading. */
                gOut[slot] = quantize(src[f * 2] * g);
                gOut[slot + 1] = quantize(src[f * 2 + 1] * g);
            }
            if (start + frames > written) __atomic_store_n(&gProducer->written, start + frames, __ATOMIC_RELEASE);
            r = 0;
        } else r = kAudioHardwareIllegalOperationError;
    }
    pthread_mutex_unlock(&gLock); return r;
}
static OSStatus EndIOOperation(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 c, UInt32 op, UInt32 n, const AudioServerPlugInIOCycleInfo *i)
{ return BeginIOOperation(d, o, c, op, n, i); }
static AudioServerPlugInDriverInterface gInterface = {
    NULL, QueryInterface, AddRef, Release, Initialize, CreateDevice, DestroyDevice, AddDeviceClient, RemoveDeviceClient,
    PerformDeviceConfigurationChange, AbortDeviceConfigurationChange, HasProperty, IsPropertySettable, GetPropertyDataSize,
    GetPropertyData, SetPropertyData, StartIO, StopIO, GetZeroTimeStamp, WillDoIOOperation, BeginIOOperation, DoIOOperation, EndIOOperation
};
static AudioServerPlugInDriverInterface *gInterfacePtr = &gInterface;
static HRESULT QueryInterface(void *self, REFIID uuid, LPVOID *out) {
    if (!out) return E_POINTER; *out = NULL;
    CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(NULL, uuid);
    HRESULT r = E_NOINTERFACE;
    if (requested && (CFEqual(requested, IUnknownUUID) || CFEqual(requested, kAudioServerPlugInDriverInterfaceUUID))) {
        AddRef(self); *out = &gInterfacePtr; r = S_OK;
    }
    if (requested) CFRelease(requested); return r;
}
__attribute__((visibility("default"))) void *X9Audio_Create(CFAllocatorRef allocator, CFUUIDRef type) {
    (void)allocator;
    return type && CFEqual(type, kAudioServerPlugInTypeUUID) ? &gInterfacePtr : NULL;
}
