// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// Property/interface scaffold adapted from UniXNU-sheng 08cde20, LICENSE.UniXNU.
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
#include "../kernel/src/bsd/dev/x9_audio.h"
_Static_assert(sizeof(X9AudioControl) == 112, "PCM ABI v1 size");

enum { kPlugIn = kAudioObjectPlugInObject, kDevice = 2, kOutStream = 3, kVolume = 5 };
#define kDeviceUID "X9SpeakerDevice"
#define kModelUID "MT6993Speaker"
static const Float64 kRate = X9_AUDIO_RATE;
static const UInt32 kZeroPeriod = X9_AUDIO_RING;
static AudioServerPlugInHostRef gHost;
static pthread_mutex_t gLock = PTHREAD_MUTEX_INITIALIZER;
static atomic_uint gRefs = 1;
static uint32_t gVolumeBits = 0x3f800000;
static uint32_t gStreamActive = 1;
static UInt32 gIoUsers;
static int gStopFailed;
static io_connect_t gConnect;
static X9AudioControl *gControl;
static X9AudioProducer *gProducer;
static int16_t *gOut;
static mach_vm_address_t gMaps[3];
static mach_vm_size_t gSizes[3];
static uint32_t floatBits(float x) { uint32_t v; memcpy(&v, &x, 4); return v; }
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
    return seq == __atomic_load_n(&gControl->seq, __ATOMIC_ACQUIRE) ? 0 : -1;
}
static UInt32 alive(void) {
    pthread_mutex_lock(&gLock); X9AudioControl s;
    UInt32 r = !snapshot(&s) && (s.state & X9AudioReady) && !s.blockers;
    pthread_mutex_unlock(&gLock); return r;
}
static UInt32 running(void) {
    pthread_mutex_lock(&gLock); X9AudioControl s;
    UInt32 r = !snapshot(&s) && (s.state & X9AudioRunning);
    pthread_mutex_unlock(&gLock); return r;
}
static UInt32 leadFrames(void) {
    pthread_mutex_lock(&gLock); X9AudioControl s;
    UInt32 r = !snapshot(&s) ? s.lead_frames : X9_AUDIO_PERIOD * 2;
    pthread_mutex_unlock(&gLock); return r;
}
static kern_return_t call(uint32_t sel) {
    return gConnect ? IOConnectCallScalarMethod(gConnect, sel, NULL, 0, NULL, NULL) : KERN_FAILURE;
}
static void disconnect(void) {
    gControl = NULL; gProducer = NULL; gOut = NULL;
    for (unsigned i = 0; i < 3; i++) {
        if (gMaps[i]) IOConnectUnmapMemory64(gConnect, i, mach_task_self(), gMaps[i]);
        gMaps[i] = 0; gSizes[i] = 0;
    }
    if (gConnect) IOServiceClose(gConnect);
    gConnect = 0;
}
static int connectService(void) {
    if (gControl) return 0;
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(X9_AUDIO_CLASS));
    if (!service) return -1;
    kern_return_t r = IOServiceOpen(service, mach_task_self(), 1, &gConnect);
    IOObjectRelease(service);
    if (r) return -1;
    const uint64_t minSizes[] = { sizeof(X9AudioControl), sizeof(X9AudioProducer), X9_AUDIO_RING_BYTES };
    for (unsigned i = 0; i < 3; i++) {
        r = IOConnectMapMemory64(gConnect, i, mach_task_self(), &gMaps[i], &gSizes[i],
                                kIOMapAnywhere | (i == 0 ? kIOMapReadOnly : 0));
        if (r || gSizes[i] < minSizes[i]) { disconnect(); return -1; }
    }
    X9AudioControl s; size_t n = sizeof(s);
    r = IOConnectCallStructMethod(gConnect, X9AudioStatus, NULL, 0, &s, &n);
    if (r || n != sizeof(s) || s.magic != X9_AUDIO_MAGIC || s.version != X9_AUDIO_VERSION ||
        s.rate != X9_AUDIO_RATE || s.channels != X9_AUDIO_CHANNELS ||
        s.ring_frames != X9_AUDIO_RING || s.period_frames != X9_AUDIO_PERIOD ||
        !s.ticks_per_frame_q32 || s.lead_frames < X9_AUDIO_PREFILL_MIN ||
        s.lead_frames > X9_AUDIO_PREFILL_MAX || s.lead_frames % X9_AUDIO_PERIOD) { disconnect(); return -1; }
    gControl = (void *)(uintptr_t)gMaps[0]; gProducer = (void *)(uintptr_t)gMaps[1]; gOut = (void *)(uintptr_t)gMaps[2];
    return 0;
}
static HRESULT QueryInterface(void *, REFIID, LPVOID *);
static ULONG AddRef(void *self) { (void)self; return atomic_fetch_add(&gRefs, 1) + 1; }
static ULONG Release(void *self) {
    (void)self; unsigned refs = atomic_load(&gRefs);
    while (refs && !atomic_compare_exchange_weak(&gRefs, &refs, refs - 1)) {}
    return refs ? refs - 1 : 0;
}
static OSStatus Initialize(AudioServerPlugInDriverRef d, AudioServerPlugInHostRef host) {
    (void)d; gHost = host; pthread_mutex_lock(&gLock); connectService(); pthread_mutex_unlock(&gLock); return 0;
}
static OSStatus CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef x, const AudioServerPlugInClientInfo *c, AudioObjectID *o)
{ (void)d; (void)x; (void)c; (void)o; return kAudioHardwareUnsupportedOperationError; }
static OSStatus DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID o)
{ (void)d; (void)o; return kAudioHardwareUnsupportedOperationError; }
static OSStatus AddDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID o, const AudioServerPlugInClientInfo *c)
{ (void)d; (void)c; return o == kDevice ? 0 : kAudioHardwareBadObjectError; }
static OSStatus RemoveDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID o, const AudioServerPlugInClientInfo *c)
{ return AddDeviceClient(d, o, c); }
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
           ((o == kOutStream) && (a->mSelector == kAudioStreamPropertyVirtualFormat || a->mSelector == kAudioStreamPropertyPhysicalFormat ||
                                                     a->mSelector == kAudioStreamPropertyIsActive)) ||
           (o == kDevice && a->mSelector == kAudioDevicePropertyNominalSampleRate);
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
    if ((o == kOutStream) && (a->mSelector == kAudioStreamPropertyVirtualFormat || a->mSelector == kAudioStreamPropertyPhysicalFormat)) {
        const AudioStreamBasicDescription *f = data;
        if (size < sizeof(*f)) return kAudioHardwareBadPropertySizeError;
        AudioStreamBasicDescription expected = format(X9_AUDIO_CHANNELS);
        return f->mSampleRate == expected.mSampleRate && f->mFormatID == expected.mFormatID &&
            f->mFormatFlags == expected.mFormatFlags && f->mChannelsPerFrame == expected.mChannelsPerFrame &&
            f->mBytesPerFrame == expected.mBytesPerFrame && f->mBitsPerChannel == expected.mBitsPerChannel &&
            f->mFramesPerPacket == 1 && f->mBytesPerPacket == expected.mBytesPerPacket ? 0 : kAudioDeviceUnsupportedFormatError;
    }
    if (o == kDevice && a->mSelector == kAudioDevicePropertyNominalSampleRate)
        return size >= sizeof(Float64) && *(const Float64 *)data == kRate ? 0 : kAudioDeviceUnsupportedFormatError;
    if (o == kOutStream && a->mSelector == kAudioStreamPropertyIsActive) {
        if (size < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
        __atomic_store_n(&gStreamActive, !!*(const UInt32 *)data, __ATOMIC_RELEASE); return 0;
    }
    return kAudioHardwareUnknownPropertyError;
}

static OSStatus SetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a, UInt32 qs,
                                const void *q, UInt32 size, const void *data) {
    (void)d; (void)pid; (void)qs; (void)q;
    if (!a || !data) return kAudioHardwareIllegalOperationError;
    const OSStatus r = setProperty(o, a, size, data);
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
    if (o == kPlugIn) list[n++] = kDevice;
    else if (o == kDevice) {
        if (scope != kAudioObjectPropertyScopeInput) list[n++] = kOutStream;
        if (scope != kAudioObjectPropertyScopeInput) list[n++] = kVolume;
    }
    return n;
}

static OSStatus propertyData(AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 inDataSize,
                             UInt32 *outDataSize, void *outData) {
    if (!a || !outDataSize || (!outData && inDataSize) || (!q && qs)) return kAudioHardwareIllegalOperationError;
    const AudioObjectPropertySelector s = a->mSelector;
    if (o <= kVolume && (s == kAudioObjectPropertyCustomPropertyInfoList || ((s == kAudioPlugInPropertyBoxList ||
        s == kAudioPlugInPropertyClockDeviceList) && o == kPlugIn) || ((s == 'taps') && (o == kOutStream)))) {
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
            if (inDataSize < sizeof(AudioObjectID)) {*outDataSize = 0; return 0;}
            ((AudioObjectID *)outData)[0] = kDevice; *outDataSize = sizeof(AudioObjectID); return 0;
        }
        case kAudioPlugInPropertyTranslateUIDToDevice: {
            CFStringRef uid = qs >= sizeof(CFStringRef) ? *(const CFStringRef *)q : NULL;
            REPLY(AudioObjectID, uid && CFStringCompare(uid, CFSTR(kDeviceUID), 0) == kCFCompareEqualTo ? kDevice : kAudioObjectUnknown);
        }
        case kAudioPlugInPropertyResourceBundle: REPLY_STR("");
        default: return kAudioHardwareUnknownPropertyError;
        }
    case kDevice:
        switch (s) {
        case kAudioObjectPropertyBaseClass: REPLY(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: REPLY(AudioClassID, kAudioDeviceClassID);
        case kAudioObjectPropertyOwner: REPLY(AudioObjectID, kPlugIn);
        case kAudioObjectPropertyName: REPLY_STR("X9 Speaker");
        case kAudioObjectPropertyManufacturer: REPLY_STR("ios-sweet");
        case kAudioDevicePropertyDeviceUID: REPLY_STR(kDeviceUID);
        case kAudioDevicePropertyModelUID: REPLY_STR(kModelUID);
        case kAudioObjectPropertyModelName: REPLY_STR("X9 Speaker");
        case kAudioObjectPropertySerialNumber: REPLY_STR("0001");
        case kAudioObjectPropertyFirmwareVersion: REPLY_STR("1.0");
        case kAudioObjectPropertyIdentify: REPLY(UInt32, 0);
        case 'accs': REPLY(UInt32, 0);
        case 'auto': REPLY(UInt32, 0);
        case kAudioObjectPropertyElementName: REPLY_STR("");
        case kAudioDevicePropertyConfigurationApplication: REPLY_STR("com.apple.Preferences");
        case kAudioDevicePropertyTransportType: REPLY(UInt32, kAudioDeviceTransportTypeBuiltIn);
        case kAudioDevicePropertyRelatedDevices: REPLY(AudioObjectID, kDevice);
        case kAudioDevicePropertyClockDomain: REPLY(UInt32, 0);
        case kAudioDevicePropertyDeviceIsAlive: REPLY(UInt32, alive());
        case kAudioDevicePropertyDeviceIsRunning: REPLY(UInt32, running());
        case kAudioDevicePropertyDeviceCanBeDefaultDevice: REPLY(UInt32, alive() && a->mScope != kAudioObjectPropertyScopeInput);
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: REPLY(UInt32, alive() && a->mScope != kAudioObjectPropertyScopeInput);
        case kAudioDevicePropertyLatency: REPLY(UInt32, latencyFrames());
        case kAudioDevicePropertySafetyOffset: REPLY(UInt32, latencyFrames() + 2 * X9_AUDIO_PERIOD);
        case kAudioDevicePropertyNominalSampleRate: REPLY(Float64, kRate);
        case kAudioDevicePropertyAvailableNominalSampleRates: REPLY(AudioValueRange, ((AudioValueRange){kRate, kRate}));
        case kAudioDevicePropertyIsHidden: REPLY(UInt32, 0);
        case kAudioDevicePropertyZeroTimeStampPeriod: REPLY(UInt32, kZeroPeriod);
        case kAudioDevicePropertyClockIsStable: REPLY(UInt32, alive());
        case kAudioDevicePropertyClockAlgorithm: REPLY(UInt32, kAudioDeviceClockAlgorithmRaw);
        case kAudioDevicePropertyPreferredChannelsForStereo: {
            if (inDataSize < 2 * sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            ((UInt32 *)outData)[0] = 1; ((UInt32 *)outData)[1] = a->mScope == kAudioObjectPropertyScopeInput ? 1 : 2;
            *outDataSize = 2 * sizeof(UInt32); return 0;
        }
        case kAudioDevicePropertyPreferredChannelLayout: {
            const UInt32 channels = a->mScope == kAudioObjectPropertyScopeInput ? 0 : X9_AUDIO_CHANNELS;
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
            AudioObjectID list[3];
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
    case kOutStream:
    {
        const UInt32 channels = X9_AUDIO_CHANNELS;
        switch (s) {
        case kAudioObjectPropertyBaseClass: REPLY(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: REPLY(AudioClassID, kAudioStreamClassID);
        case kAudioObjectPropertyOwner: REPLY(AudioObjectID, kDevice);
        case kAudioObjectPropertyName: if (o == kOutStream) REPLY_STR("X9 Speaker"); REPLY_STR("Microphone");
        case kAudioObjectPropertyOwnedObjects: *outDataSize = 0; return 0;
        case kAudioStreamPropertyIsActive: REPLY(UInt32, __atomic_load_n(&gStreamActive, __ATOMIC_ACQUIRE));
        case kAudioStreamPropertyDirection: REPLY(UInt32, 0);
        case kAudioStreamPropertyTerminalType: REPLY(UInt32, o == kOutStream ? kAudioStreamTerminalTypeSpeaker : kAudioStreamTerminalTypeMicrophone);
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
    (void)d; (void)pid; return propertyData(o, a, qs, q, capacity, size, out);
}
static OSStatus StartIO(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client) {
    (void)d; (void)client;
    if (o != kDevice) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gLock);
    OSStatus r = 0;
    if (!gIoUsers) {
        if (gStopFailed || connectService() || call(X9AudioStart)) r = kAudioHardwareNotRunningError;
        else { X9AudioControl s;
            if (snapshot(&s) || !(s.state & X9AudioRunning) || (s.state & X9AudioFault)) {
                gStopFailed = call(X9AudioStop) != 0; r = kAudioHardwareNotRunningError;
            }
        }
    } else {
        X9AudioControl s;
        if (snapshot(&s) || !(s.state & X9AudioRunning) || (s.state & X9AudioFault)) r = kAudioHardwareNotRunningError;
    }
    if (!r) gIoUsers++;
    pthread_mutex_unlock(&gLock); return r;
}
static OSStatus StopIO(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client) {
    (void)d; (void)client;
    if (o != kDevice) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gLock);
    if (gIoUsers) gIoUsers--;
    kern_return_t r = !gIoUsers && (gStopFailed || gConnect) ? call(X9AudioStop) : 0;
    if (!gIoUsers) gStopFailed = r != 0;
    pthread_mutex_unlock(&gLock); return r ? kAudioHardwareUnspecifiedError : 0;
}
static OSStatus GetZeroTimeStamp(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client,
    Float64 *sample, UInt64 *host, UInt64 *seed) {
    (void)d; (void)client;
    if (o != kDevice || !sample || !host || !seed) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gLock); X9AudioControl s;
    OSStatus r = kAudioHardwareNotRunningError;
    if (!snapshot(&s) && (s.state & X9AudioRunning)) {
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
    if (o != kDevice || !will || !inPlace) return kAudioHardwareBadObjectError;
    *will = op == kAudioServerPlugInIOOperationWriteMix; *inPlace = true; return 0;
}
static OSStatus BeginIOOperation(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 c, UInt32 op, UInt32 n, const AudioServerPlugInIOCycleInfo *i)
{ (void)d; (void)o; (void)c; (void)op; (void)n; (void)i; return 0; }
static int16_t quantize(float v) {
    if (!isfinite(v)) return 0;
    v *= 32767.0f; return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}
static OSStatus DoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID o, AudioObjectID stream, UInt32 c, UInt32 op,
    UInt32 frames, const AudioServerPlugInIOCycleInfo *info, void *main, void *secondary) {
    (void)d; (void)c; (void)secondary;
    if (o != kDevice || stream != kOutStream) return kAudioHardwareBadObjectError;
    if (op != kAudioServerPlugInIOOperationWriteMix) return kAudioHardwareUnsupportedOperationError;
    if (!info || !main || frames > X9_AUDIO_RING) return kAudioHardwareIllegalOperationError;
    double t = info->mOutputTime.mSampleTime;
    if (!isfinite(t) || t < 0 || t >= (double)UINT64_MAX || t != floor(t)) return kAudioHardwareIllegalOperationError;
    pthread_mutex_lock(&gLock); X9AudioControl s;
    OSStatus r = kAudioHardwareNotRunningError;
    if (!snapshot(&s) && (s.state & X9AudioRunning)) {
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
