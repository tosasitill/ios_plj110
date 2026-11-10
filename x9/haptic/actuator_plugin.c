/* Native iOS Actuator HAL. Property/COM convention: HakiOS audio_plugin.c
 * and X9Microphone (MIT; LICENSE.hakios). No Apple file patching or UIKit
 * swizzling: VirtualAudio/CoreHaptics deliver their original mixed S16 PCM.
 * IO only copies into a bounded queue; all syscalls/notifications are deferred.
 */
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CFPlugInCOM.h>
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <math.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "../kernel/src/bsd/dev/x9_haptic.h"
enum {PLUGIN=1,ACTUATOR=2,OUTPUT=3,MAX_CLIENTS=64,QUEUE=16,PERIOD=512};
struct client {UInt32 id;bool used,running;};
static struct client clients[MAX_CLIENTS];
static struct x9_haptic_packet queue[QUEUE];
static uint64_t enqueued_at[QUEUE];
static uint64_t due_at[QUEUE];
static unsigned first,count,running;
static int fd=-1;
static bool ready,fault,stop_failed,dirty,cycle_valid;
static Float64 last_sample,ticks_per_frame;
static UInt32 last_frames,last_hash;
static uint64_t generation,sequence,anchor,seed=1,last_due;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static AudioServerPlugInHostRef host;
static dispatch_source_t timer;
static atomic_uint refs=1;
static uint64_t now_ns(void)
{mach_timebase_info_data_t t;mach_timebase_info(&t);return (uint64_t)((__uint128_t)mach_absolute_time()*t.numer/t.denom);}
static bool qualified(const struct x9_haptic_status *s)
{
    return s->version==1 && s->mode==2 && s->input_rate==48000 && s->rtp_rate==12000 &&
        (s->chip_id==0x9371 || s->chip_id==0x9381) &&
        (s->flags&(X9_HAPTIC_READY|X9_HAPTIC_FAULT|X9_HAPTIC_OFF_FAILED))==X9_HAPTIC_READY;
}
static bool discover(void)
{
    int f=open("/dev/x9-haptic-status",O_RDONLY|O_CLOEXEC);
    if(f<0)return false;struct x9_haptic_status s={0};
    bool ok=ioctl(f,X9_HAPTIC_GET_STATUS,&s)==0 && qualified(&s);close(f);return ok;
}
static struct client *client(UInt32 id)
{for(unsigned i=0;i<MAX_CLIENTS;i++)if(clients[i].used && clients[i].id==id)return &clients[i];return NULL;}
static void fault_io(void)
{if(!fault){++seed;dirty=true;}fault=true;ready=false;cycle_valid=false;}
static OSStatus stop_device(void)
{
    if(fd<0)return stop_failed?'nope':0;
    struct x9_haptic_status a={0},b={0};
    bool before=ioctl(fd,X9_HAPTIC_GET_STATUS,&a)==0 && a.version==1;
    bool stopped=ioctl(fd,X9_HAPTIC_STOP)==0;
    bool after=ioctl(fd,X9_HAPTIC_GET_STATUS,&b)==0 && b.version==1;
    if(!before || !stopped || !after || b.generation!=a.generation+1 || b.queued_frames ||
        (b.flags&(X9_HAPTIC_RUNNING|X9_HAPTIC_OFF_FAILED)) || b.hw_state) {
        stop_failed=true;fault_io();return 'nope'; /* retain fd for STOP retry */
    }
    int old=fd;fd=-1;stop_failed=false;first=count=0;cycle_valid=false;
    memset(queue,0,sizeof(queue));ready=qualified(&b) && !fault;
    if(close(old)<0){fault_io();return 'nope';}return 0;
}
static void notify(void)
{
    if(!host)return;
    AudioObjectPropertyAddress p[]={
        {kAudioPlugInPropertyDeviceList,kAudioObjectPropertyScopeGlobal,0},
        {kAudioObjectPropertyOwnedObjects,kAudioObjectPropertyScopeGlobal,0}};
    AudioObjectPropertyAddress d[]={
        {kAudioDevicePropertyDeviceIsAlive,kAudioObjectPropertyScopeGlobal,0},
        {kAudioDevicePropertyDeviceIsRunning,kAudioObjectPropertyScopeGlobal,0}};
    host->PropertiesChanged(host,PLUGIN,2,p);host->PropertiesChanged(host,ACTUATOR,2,d);
}
static void service(__unused void *context)
{
    static unsigned tick;
    if(pthread_mutex_trylock(&lock))return;
    bool was=ready;
    if(fd>=0 && !fault && running) {
        /* Never retry a partial write: that violates the packet contract.
         * EAGAIN means kernel accepted NOTHING, so retain this exact packet.
         * Long stale queues fault instead of replaying old feedback later. */
        unsigned budget=4;
        while(count && budget--) {
            if(now_ns()<due_at[first])break;
            if(now_ns()>due_at[first]+50000000ULL){fault_io();break;}
            struct x9_haptic_packet *p=&queue[first];
            size_t bytes=24+(size_t)p->frames*2;
            ssize_t n=write(fd,p,bytes);
            if(n<0 && (errno==EAGAIN || errno==EINTR))break;
            if(n!=(ssize_t)bytes){fault_io();break;}
            memset(p,0,sizeof(*p));first=(first+1)%QUEUE;--count;
        }
    }
    if(fault && fd>=0 && !stop_failed)(void)stop_device();
    if(++tick%500==0) {
        if(!running && fd<0 && !fault && !stop_failed)ready=discover();
        if(running && fd>=0 && !fault) {
            struct x9_haptic_status s;
            int result=ioctl(fd,X9_HAPTIC_GET_STATUS_NOWAIT,&s);
            if(result<0 && (errno==EAGAIN || errno==EINTR)){}
            else if(result<0 || !qualified(&s) || !(s.flags&X9_HAPTIC_RUNNING) || s.generation!=generation)fault_io();
        }
    }
    bool changed=dirty || was!=ready;dirty=false;
    pthread_mutex_unlock(&lock);if(changed)notify();
}
static ULONG retain_driver(__unused void *d){return atomic_fetch_add(&refs,1)+1;}
static ULONG release_driver(__unused void *d)
{unsigned n=atomic_load(&refs);while(n && !atomic_compare_exchange_weak(&refs,&n,n-1)){}return n?n-1:0;}
static HRESULT query(void *d,REFIID iid,LPVOID *out)
{
    if(!out)return E_POINTER;*out=NULL;
    CFUUIDRef u=CFUUIDCreateFromUUIDBytes(NULL,iid);
    bool ok=u && (CFEqual(u,IUnknownUUID) || CFEqual(u,kAudioServerPlugInDriverInterfaceUUID));
    if(u)CFRelease(u);if(!ok)return E_NOINTERFACE;retain_driver(d);*out=d;return S_OK;
}
static OSStatus initialize(__unused AudioServerPlugInDriverRef d,AudioServerPlugInHostRef h)
{
    if(!h || !h->PropertiesChanged)return 'nope';pthread_mutex_lock(&lock);
    if(host){OSStatus e=host==h?0:'nope';pthread_mutex_unlock(&lock);return e;}
    mach_timebase_info_data_t t;if(mach_timebase_info(&t)!=KERN_SUCCESS || !t.numer || !t.denom)
        {pthread_mutex_unlock(&lock);return 'nope';}
    dispatch_queue_t q=dispatch_queue_create("com.x9.haptic.actuator",NULL);
    timer=q?dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,q):NULL;
    if(!timer){if(q)dispatch_release(q);pthread_mutex_unlock(&lock);return 'nope';}
    ticks_per_frame=1e9*t.denom/t.numer/48000;host=h;ready=discover();
    dispatch_source_set_event_handler_f(timer,service);
    dispatch_source_set_timer(timer,dispatch_time(DISPATCH_TIME_NOW,NSEC_PER_MSEC),NSEC_PER_MSEC,100000);
    pthread_mutex_unlock(&lock);dispatch_resume(timer);dispatch_release(q);
    fprintf(stderr,"X9 HAPTIC HAL: Actuator native route hardware=%s; no private API hooks\n",ready?"ready":"unavailable");return 0;
}
static OSStatus create(__unused AudioServerPlugInDriverRef d,__unused CFDictionaryRef p,
    __unused const AudioServerPlugInClientInfo *c,__unused AudioObjectID *o){return 'unop';}
static OSStatus destroy(__unused AudioServerPlugInDriverRef d,__unused AudioObjectID o){return 'unop';}
static OSStatus add_client(__unused AudioServerPlugInDriverRef d,AudioObjectID o,const AudioServerPlugInClientInfo *c)
{
    if(o!=ACTUATOR || !c)return '!obj';pthread_mutex_lock(&lock);OSStatus e=0;
    if(!client(c->mClientID)) {
        struct client *slot=NULL;for(unsigned i=0;i<MAX_CLIENTS;i++)if(!clients[i].used){slot=&clients[i];break;}
        if(slot)*slot=(struct client){.id=c->mClientID,.used=true};else e='nope';
    }
    pthread_mutex_unlock(&lock);return e;
}
static OSStatus stop_client(struct client *c)
{if(c->running){c->running=false;if(!--running)++seed;}return running?0:stop_device();}
static OSStatus remove_client(__unused AudioServerPlugInDriverRef d,AudioObjectID o,const AudioServerPlugInClientInfo *c)
{
    if(o!=ACTUATOR || !c)return '!obj';pthread_mutex_lock(&lock);struct client *s=client(c->mClientID);
    OSStatus e=s?stop_client(s):0;if(s)memset(s,0,sizeof(*s));pthread_mutex_unlock(&lock);notify();return e;
}
static OSStatus config(__unused AudioServerPlugInDriverRef d,AudioObjectID o,__unused UInt64 a,__unused void *i)
{return o==ACTUATOR?0:'!obj';}
static AudioStreamBasicDescription format(void)
{return (AudioStreamBasicDescription){48000,kAudioFormatLinearPCM,kAudioFormatFlagIsSignedInteger|kAudioFormatFlagIsPacked,2,1,2,1,16,0};}
struct value {UInt32 size;bool exists,string;union {UInt32 words[2];Float64 number;CFStringRef str;
    AudioValueRange range;AudioStreamBasicDescription pcm;AudioStreamRangedDescription ranged;
    AudioBufferList buffers;AudioChannelLayout layout;} data;};
static struct value property(AudioObjectID o,const AudioObjectPropertyAddress *a,UInt32 qs,const void *q)
{
    struct value v={0};if(!a || o<PLUGIN || o>OUTPUT || a->mElement)return v;
    UInt32 s=a->mSelector,scope=a->mScope;
    if(scope!=kAudioObjectPropertyScopeGlobal && scope!=kAudioObjectPropertyScopeInput && scope!=kAudioObjectPropertyScopeOutput)return v;
#define WORD(x) do{v.exists=true;v.size=4;v.data.words[0]=(x);}while(0)
#define EMPTY() do{v.exists=true;v.size=0;}while(0)
#define STR(x) do{v.exists=v.string=true;v.size=sizeof(CFStringRef);v.data.str=CFSTR(x);}while(0)
#define NUMBER(x) do{v.exists=true;v.size=8;v.data.number=(x);}while(0)
    if(s==kAudioObjectPropertyCustomPropertyInfoList || (o==OUTPUT && s=='taps')){EMPTY();return v;}
    if(s==kAudioObjectPropertyBaseClass){WORD(kAudioObjectClassID);return v;}
    if(s==kAudioObjectPropertyClass){WORD(o==PLUGIN?kAudioPlugInClassID:o==ACTUATOR?kAudioDeviceClassID:kAudioStreamClassID);return v;}
    if(s==kAudioObjectPropertyOwner){WORD(o==PLUGIN?0:o==ACTUATOR?PLUGIN:ACTUATOR);return v;}
    if(s==kAudioObjectPropertyManufacturer){STR("X9");return v;}
    if(s==kAudioObjectPropertyName){STR("Actuator");return v;}
    if(o==PLUGIN)switch(s) {
        case kAudioObjectPropertyOwnedObjects:case kAudioPlugInPropertyDeviceList:if(ready)WORD(ACTUATOR);else EMPTY();break;
        case kAudioPlugInPropertyTranslateUIDToDevice:
            WORD(qs==sizeof(CFStringRef) && q && *(CFStringRef const *)q && ready && CFEqual(*(CFStringRef const *)q,CFSTR("Actuator"))?ACTUATOR:0);break;
        case kAudioPlugInPropertyBoxList:case kAudioPlugInPropertyClockDeviceList:EMPTY();break;
        case kAudioPlugInPropertyTranslateUIDToBox:case kAudioPlugInPropertyTranslateUIDToClockDevice:WORD(0);break;
        case kAudioPlugInPropertyResourceBundle:STR("");break;
    } else if(o==ACTUATOR) {
        bool output=scope!=kAudioObjectPropertyScopeInput;
        switch(s) {
        case kAudioObjectPropertyOwnedObjects:case kAudioDevicePropertyStreams:if(output)WORD(OUTPUT);else EMPTY();break;
        case kAudioObjectPropertyControlList:EMPTY();break;
        case kAudioDevicePropertyPreferredChannelsForStereo:
            v.exists=true;v.size=8;v.data.words[0]=v.data.words[1]=1;break;
        case kAudioDevicePropertyPreferredChannelLayout:
            v.exists=true;v.size=offsetof(AudioChannelLayout,mChannelDescriptions);
            v.data.layout.mChannelLayoutTag=output?kAudioChannelLayoutTag_Mono:kAudioChannelLayoutTag_UseChannelDescriptions;break;
        case kAudioDevicePropertyDeviceUID:STR("Actuator");break;
        case kAudioDevicePropertyModelUID:STR("X9-AW8693XS-Actuator");break;
        case kAudioDevicePropertyTransportType:WORD(kAudioDeviceTransportTypeBuiltIn);break;
        case kAudioDevicePropertyRelatedDevices:WORD(ACTUATOR);break;
        case kAudioDevicePropertyClockDomain:WORD(0);break;
        case kAudioDevicePropertyDeviceIsAlive:WORD(ready);break;
        case kAudioDevicePropertyDeviceIsRunning:WORD(running && !fault && !stop_failed);break;
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:WORD(0);break;
        case kAudioDevicePropertyIsHidden:WORD(0);break;
        case kAudioDevicePropertyLatency:WORD(480);break;
        case kAudioDevicePropertySafetyOffset:WORD(240);break;
        case kAudioDevicePropertyZeroTimeStampPeriod:WORD(PERIOD);break;
        case kAudioDevicePropertyNominalSampleRate:NUMBER(48000);break;
        case kAudioDevicePropertyAvailableNominalSampleRates:v.exists=true;v.size=sizeof(AudioValueRange);v.data.range=(AudioValueRange){48000,48000};break;
        }
    } else switch(s) {
        case kAudioObjectPropertyOwnedObjects:EMPTY();break;
        case kAudioStreamPropertyIsActive:WORD(ready);break;
        case kAudioStreamPropertyDirection:WORD(0);break;
        case kAudioStreamPropertyTerminalType:WORD(kAudioStreamTerminalTypeUnknown);break;
        case kAudioStreamPropertyStartingChannel:WORD(1);break;
        case kAudioStreamPropertyLatency:WORD(0);break;
        case kAudioStreamPropertyVirtualFormat:case kAudioStreamPropertyPhysicalFormat:v.exists=true;v.size=sizeof(AudioStreamBasicDescription);v.data.pcm=format();break;
        case kAudioStreamPropertyAvailableVirtualFormats:case kAudioStreamPropertyAvailablePhysicalFormats:
            v.exists=true;v.size=sizeof(AudioStreamRangedDescription);v.data.ranged=(AudioStreamRangedDescription){format(),{48000,48000}};break;
    }
#undef WORD
#undef EMPTY
#undef STR
#undef NUMBER
    return v;
}
static Boolean has(__unused AudioServerPlugInDriverRef d,AudioObjectID o,__unused pid_t p,const AudioObjectPropertyAddress *a)
{pthread_mutex_lock(&lock);bool yes=property(o,a,0,NULL).exists;pthread_mutex_unlock(&lock);return yes;}
static bool settable(AudioObjectID o,const AudioObjectPropertyAddress *a)
{return a && !a->mElement && ((o==ACTUATOR && a->mSelector==kAudioDevicePropertyNominalSampleRate) ||
    (o==OUTPUT && (a->mSelector==kAudioStreamPropertyVirtualFormat || a->mSelector==kAudioStreamPropertyPhysicalFormat)));}
static OSStatus is_settable(__unused AudioServerPlugInDriverRef d,AudioObjectID o,__unused pid_t p,const AudioObjectPropertyAddress *a,Boolean *out)
{if(!out)return '!siz';if(!has(d,o,p,a))return 'who?';*out=settable(o,a);return 0;}
static OSStatus get_size(__unused AudioServerPlugInDriverRef d,AudioObjectID o,__unused pid_t p,const AudioObjectPropertyAddress *a,UInt32 qs,const void *q,UInt32 *out)
{if(!out)return '!siz';pthread_mutex_lock(&lock);struct value v=property(o,a,qs,q);pthread_mutex_unlock(&lock);if(!v.exists)return 'who?';*out=v.size;return 0;}
static OSStatus get_data(__unused AudioServerPlugInDriverRef d,AudioObjectID o,__unused pid_t p,const AudioObjectPropertyAddress *a,UInt32 qs,const void *q,UInt32 size,UInt32 *out,void *data)
{if(!out)return '!siz';*out=0;pthread_mutex_lock(&lock);struct value v=property(o,a,qs,q);pthread_mutex_unlock(&lock);
    if(!v.exists)return 'who?';if(size<v.size || (v.size && !data))return '!siz';if(v.string)CFRetain(v.data.str);
    if(v.size)memcpy(data,&v.data,v.size);*out=v.size;return 0;}
static OSStatus set_data(__unused AudioServerPlugInDriverRef d,AudioObjectID o,__unused pid_t p,const AudioObjectPropertyAddress *a,__unused UInt32 qs,__unused const void *q,UInt32 size,const void *data)
{
    if(!settable(o,a))return 'who?';if(!data)return '!siz';
    if(o==ACTUATOR)return size==8 && *(const Float64 *)data==48000?0:'!dat';
    if(size!=sizeof(AudioStreamBasicDescription))return '!siz';
    const AudioStreamBasicDescription *f=data,expected=format();
    return f->mSampleRate==48000 && f->mFormatID==expected.mFormatID && f->mFormatFlags==expected.mFormatFlags &&
        f->mBytesPerPacket==2 && f->mFramesPerPacket==1 && f->mBytesPerFrame==2 && f->mChannelsPerFrame==1 && f->mBitsPerChannel==16?0:'!dat';
}
static OSStatus start_io(__unused AudioServerPlugInDriverRef d,AudioObjectID o,UInt32 id)
{
    if(o!=ACTUATOR)return '!obj';pthread_mutex_lock(&lock);struct client *c=client(id);OSStatus e=0;
    if(!c)e='!obj';else if(fault || stop_failed)e='nope';
    else if(!c->running) {
        if(!running) {
            ready=discover();if(!ready)e='nope';
            else {
                fd=open("/dev/x9-haptic",O_WRONLY|O_NONBLOCK|O_CLOEXEC);
                struct x9_haptic_start s={.version=1,.conversion=X9_HAPTIC_RETUNE,.intensity=1000,.sharpness=650};
                if(fd<0 || ioctl(fd,X9_HAPTIC_START,&s)<0){e='nope';if(fd>=0)(void)stop_device();}
                else {
                    struct x9_haptic_status state;
                    if(ioctl(fd,X9_HAPTIC_GET_STATUS,&state)<0 || !qualified(&state) ||
                       !(state.flags&X9_HAPTIC_RUNNING) || state.generation!=s.generation) {
                        fault_io();(void)stop_device();e='nope';
                    } else {generation=s.generation;sequence=0;anchor=mach_absolute_time();
                        first=count=0;cycle_valid=false;++seed;}
                }
            }
        }
        if(!e){c->running=true;++running;}
    }
    pthread_mutex_unlock(&lock);notify();return e;
}
static OSStatus stop_io(__unused AudioServerPlugInDriverRef d,AudioObjectID o,UInt32 id)
{if(o!=ACTUATOR)return '!obj';pthread_mutex_lock(&lock);struct client *c=client(id);OSStatus e=c?stop_client(c):'!obj';pthread_mutex_unlock(&lock);notify();return e;}
static OSStatus timestamp(__unused AudioServerPlugInDriverRef d,AudioObjectID o,UInt32 id,Float64 *sample,UInt64 *time,UInt64 *out_seed)
{
    if(o!=ACTUATOR || !sample || !time || !out_seed)return '!obj';
    if(pthread_mutex_trylock(&lock))return 'nope';struct client *c=client(id);OSStatus e=0;
    if(!c || !c->running || !ready || fault || stop_failed)e='nope';
    else {
        /* Software render clock, NOT a measured AW oscillator/playback
         * cursor. Physical completion lives in kernel FIFO/standby status. */
        uint64_t elapsed=mach_absolute_time()-anchor;
        Float64 frames=floor((elapsed/ticks_per_frame)/PERIOD)*PERIOD;
        *sample=frames;*time=anchor+(uint64_t)(frames*ticks_per_frame);*out_seed=seed;
    }
    pthread_mutex_unlock(&lock);return e;
}
static OSStatus will(__unused AudioServerPlugInDriverRef d,AudioObjectID o,__unused UInt32 id,UInt32 operation,Boolean *out,Boolean *in_place)
{if(o!=ACTUATOR || !out || !in_place)return '!obj';*out=operation==kAudioServerPlugInIOOperationWriteMix;*in_place=true;return 0;}
static OSStatus begin_end(__unused AudioServerPlugInDriverRef d,AudioObjectID o,__unused UInt32 id,__unused UInt32 operation,__unused UInt32 frames,__unused const AudioServerPlugInIOCycleInfo *cycle)
{return o==ACTUATOR?0:'!obj';}
static OSStatus do_io(__unused AudioServerPlugInDriverRef d,AudioObjectID o,AudioObjectID stream,UInt32 id,UInt32 operation,UInt32 frames,const AudioServerPlugInIOCycleInfo *cycle,void *buffer,__unused void *secondary)
{
    if(o!=ACTUATOR || stream!=OUTPUT || operation!=kAudioServerPlugInIOOperationWriteMix)return '!obj';
    if(frames>X9_HAPTIC_MAX_FRAMES || (!buffer && frames))return '!siz';if(!frames)return 0;
    if(!cycle || !(cycle->mOutputTime.mFlags&kAudioTimeStampSampleTimeValid) ||
       !isfinite(cycle->mOutputTime.mSampleTime) || cycle->mOutputTime.mSampleTime<0 ||
       cycle->mOutputTime.mSampleTime>9007199254740991.0 ||
       floor(cycle->mOutputTime.mSampleTime)!=cycle->mOutputTime.mSampleTime)return '!dat';
    if(pthread_mutex_trylock(&lock))return 'nope';
    struct client *c=client(id);OSStatus e=0;
    if(!c || !c->running || !running || fd<0 || !ready || fault || stop_failed)e='nope';
    Float64 sample=cycle->mOutputTime.mSampleTime;
    uint32_t hash=2166136261u;const uint8_t *bytes=buffer;
    for(unsigned i=0;i<frames*2;i++)hash=(hash^bytes[i])*16777619u;
    bool duplicate=cycle_valid && sample==last_sample && frames==last_frames && hash==last_hash;
    if(!e && !duplicate) {
        if(cycle_valid && sample<last_sample+last_frames)e='!dat';
        else if(count==QUEUE)e='nope';
        else {
            /* Preserve scheduled inter-event gaps instead of appending the
             * next tap immediately behind earlier PCM. A valid host time
             * schedules delivery on the software render clock. With no host
             * time, an empty queue means the idle gap already passed in real
             * time; queued unscheduled discontinuities are not accepted. */
            uint64_t due=now_ns();bool timed=(cycle->mOutputTime.mFlags&kAudioTimeStampHostTimeValid)!=0;
            if(timed) {
                mach_timebase_info_data_t t;mach_timebase_info(&t);
                due=(uint64_t)((__uint128_t)cycle->mOutputTime.mHostTime*t.numer/t.denom);
                if(due>now_ns()+100000000ULL)e='!dat';
            }
            if(cycle_valid && sample>last_sample+last_frames && count && !timed)e='!dat';
            if(cycle_valid && timed && due<last_due)e='!dat';
            if(e){pthread_mutex_unlock(&lock);return e;}
            unsigned n=(first+count)%QUEUE;struct x9_haptic_packet *p=&queue[n];
            p->version=1;p->frames=frames;p->generation=generation;p->sequence=sequence++;
            memcpy(p->samples,buffer,frames*2);enqueued_at[n]=now_ns();due_at[n]=due;++count;
            last_sample=sample;last_frames=frames;last_hash=hash;last_due=due;cycle_valid=true;
        }
    }
    pthread_mutex_unlock(&lock);return e;
}
static AudioServerPlugInDriverInterface interface={NULL,query,retain_driver,release_driver,initialize,
    create,destroy,add_client,remove_client,config,config,has,is_settable,get_size,get_data,set_data,
    start_io,stop_io,timestamp,will,begin_end,do_io,begin_end};
static AudioServerPlugInDriverInterface *driver=&interface;
void *X9Haptic_Create(__unused CFAllocatorRef allocator,CFUUIDRef type)
{return type && CFEqual(type,kAudioServerPlugInTypeUUID)?&driver:NULL;}
