/* X9Microphone.driver: input-only iOS AudioServerPlugIn.
 * HakiOS (MIT, LICENSE.hakios) supplies the HAL ABI and Codec/imic routing
 * precedent. No Speaker/Hawking/Actuator stand-ins, no successful StartIO
 * when hardware is absent. The Apple VirtualAudio route is not yet verified
 * on PLJ110/20H392; exposing Codec is not proof of successful AVAudioSession. */
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CFPlugInCOM.h>
#include <stdbool.h>
#include "../kernel/src/bsd/dev/x9_microphone.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <limits.h>

enum {PLUGIN=1,CODEC=2,INPUT=3,SOURCE=4};
#define MIC_SOURCE 'imic'
#define PERIOD 512u
#define MAX_IO_FRAMES 4096u
#define MAX_CLIENTS 64u
struct client {UInt32 id;bool used,running;};
static struct client clients[MAX_CLIENTS];
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static AudioServerPlugInHostRef host;
static unsigned running;
static int mic=-1;
static bool ready;
static UInt64 anchor,seed=1,received,underrun;
static int16_t cycle_pcm[MAX_IO_FRAMES];
static Float64 cycle_sample;
static UInt64 cycle_seed;
static UInt32 cycle_frames;
static bool cycle_valid;
static Float64 ticks_per_frame;
static OSStatus bad_object= '!obj', unavailable= 'nope';
static AudioStreamBasicDescription pcm_format(void)
{
    return (AudioStreamBasicDescription){.mSampleRate=X9_MIC_RATE,.mFormatID=kAudioFormatLinearPCM,
        .mFormatFlags=kAudioFormatFlagIsSignedInteger|kAudioFormatFlagIsPacked,
        .mBytesPerPacket=2,.mFramesPerPacket=1,.mBytesPerFrame=2,.mChannelsPerFrame=1,.mBitsPerChannel=16};
}
static bool probe_ready(void)
{
    struct x9_mic_status s={0};int fd=open(X9_MIC_STATUS_DEVICE,O_RDONLY|O_CLOEXEC);
    if(fd<0)return false;
    bool ok=ioctl(fd,X9_MIC_GET_STATUS,&s)==0 && s.version==1 && s.mode==2 &&
        (s.flags&X9_MIC_READY) && !(s.flags&X9_MIC_FAULT) &&
        (s.supported_sources&X9_MIC_SOURCE_INTERNAL) && s.rate==X9_MIC_RATE && s.channels==1 && s.sample_bits==16;
    close(fd);return ok;
}
static HRESULT query(void *d,REFIID uuid,LPVOID *out)
{
    if(!out)return E_POINTER;*out=NULL;
    CFUUIDRef u=CFUUIDCreateFromUUIDBytes(NULL,uuid);
    bool yes=CFEqual(u,IUnknownUUID)||CFEqual(u,kAudioServerPlugInDriverInterfaceUUID);
    CFRelease(u);if(!yes)return E_NOINTERFACE;*out=d;return S_OK;
}
static ULONG retain_driver(__unused void *d){return 1;}
static ULONG release_driver(__unused void *d){return 1;}
static OSStatus initialize(__unused AudioServerPlugInDriverRef d,AudioServerPlugInHostRef h)
{
    mach_timebase_info_data_t t;mach_timebase_info(&t);
    ticks_per_frame=1e9*t.denom/t.numer/X9_MIC_RATE;host=h;
    ready=probe_ready();
    fprintf(stderr,"X9 MIC HAL: Codec/imic hardware=%s; no fake input device\n",ready?"ready":"unavailable");
    return 0;
}
static OSStatus create(__unused AudioServerPlugInDriverRef d,__unused CFDictionaryRef p,
    __unused const AudioServerPlugInClientInfo *c,__unused AudioObjectID *o){return 'unop';}
static OSStatus destroy(__unused AudioServerPlugInDriverRef d,__unused AudioObjectID o){return 'unop';}
static struct client *find_client(UInt32 id)
{
    for(unsigned i=0;i<MAX_CLIENTS;i++)if(clients[i].used && clients[i].id==id)return &clients[i];
    return NULL;
}
static OSStatus add_client(__unused AudioServerPlugInDriverRef d,AudioObjectID o,const AudioServerPlugInClientInfo *c)
{
    if(o!=CODEC || !c)return bad_object;
    pthread_mutex_lock(&lock);OSStatus e=0;
    if(!find_client(c->mClientID)) {
        struct client *slot=NULL;
        for(unsigned i=0;i<MAX_CLIENTS;i++)if(!clients[i].used){slot=&clients[i];break;}
        if(slot)*slot=(struct client){.id=c->mClientID,.used=true};else e=unavailable;
    }
    pthread_mutex_unlock(&lock);return e;
}
static void stop_client(struct client *c)
{
    if(c->running) {
        c->running=false;
        if(--running==0) {
            if(mic>=0){(void)ioctl(mic,X9_MIC_STOP);close(mic);mic=-1;}
            memset(cycle_pcm,0,sizeof(cycle_pcm));cycle_valid=false;
            ++seed;
            fprintf(stderr,"X9 MIC HAL: stopped real_frames=%llu underrun_frames=%llu\n",received,underrun);
        }
    }
}
static OSStatus remove_client(__unused AudioServerPlugInDriverRef d,AudioObjectID o,const AudioServerPlugInClientInfo *c)
{
    if(o!=CODEC || !c)return bad_object;
    pthread_mutex_lock(&lock);struct client *slot=find_client(c->mClientID);
    if(slot){stop_client(slot);memset(slot,0,sizeof(*slot));}
    pthread_mutex_unlock(&lock);return 0;
}
static OSStatus configuration(__unused AudioServerPlugInDriverRef d,AudioObjectID o,
    __unused UInt64 action,__unused void *info){return o==CODEC?0:bad_object;}

struct value {UInt32 size;bool exists,string;union {UInt32 words[4];Float64 number;CFStringRef str;
    AudioValueRange range;AudioStreamBasicDescription format;AudioStreamRangedDescription ranged;} data;};
static struct value property(AudioObjectID o,const AudioObjectPropertyAddress *a,UInt32 qsize,const void *q)
{
    struct value v={0};if(!a)return v;
    if(o<PLUGIN || o>SOURCE)return v;
    if(a->mElement)return v;
    UInt32 scope=a->mScope,s=a->mSelector;
    if(scope!=kAudioObjectPropertyScopeGlobal && scope!=kAudioObjectPropertyScopeInput && scope!=kAudioObjectPropertyScopeOutput)return v;
#define WORD(x) do {v.exists=true;v.size=4;v.data.words[0]=(x);} while(0)
#define EMPTY() do {v.exists=true;v.size=0;} while(0)
#define STR(x) do {v.exists=v.string=true;v.size=sizeof(CFStringRef);v.data.str=(x);} while(0)
#define NUMBER(x) do {v.exists=true;v.size=sizeof(Float64);v.data.number=(x);} while(0)
    if(s==kAudioObjectPropertyBaseClass){WORD(o==SOURCE?kAudioSelectorControlClassID:kAudioObjectClassID);return v;}
    if(s==kAudioObjectPropertyClass){WORD(o==PLUGIN?kAudioPlugInClassID:o==CODEC?kAudioDeviceClassID:o==INPUT?kAudioStreamClassID:kAudioDataSourceControlClassID);return v;}
    if(s==kAudioObjectPropertyOwner){WORD(o==PLUGIN?0:o==CODEC?PLUGIN:CODEC);return v;}
    if(s==kAudioObjectPropertyManufacturer){STR(CFSTR("X9"));return v;}
    if(s==kAudioObjectPropertyName){STR(o==PLUGIN?CFSTR("X9Microphone"):o==CODEC?CFSTR("Codec"):o==INPUT?CFSTR("Internal Microphone"):CFSTR("Input Source"));return v;}
    if(o==PLUGIN) {
        switch(s) {
        case kAudioObjectPropertyOwnedObjects:case kAudioPlugInPropertyDeviceList:
            if(ready)WORD(CODEC);else EMPTY();break;
        case kAudioPlugInPropertyTranslateUIDToDevice:
            WORD(0);
            if(qsize==sizeof(CFStringRef) && q && *(CFStringRef const *)q)
                WORD(ready && CFEqual(*(CFStringRef const *)q,CFSTR("Codec"))?CODEC:0);
            break;
        case kAudioPlugInPropertyBoxList:EMPTY();break;
        case kAudioPlugInPropertyTranslateUIDToBox:WORD(0);break;
        case kAudioPlugInPropertyResourceBundle:STR(CFSTR(""));break;
        }
    } else if(o==CODEC) {
        bool input=scope!=kAudioObjectPropertyScopeOutput;
        switch(s) {
        case kAudioObjectPropertyOwnedObjects:
            EMPTY();if(input){v.size=8;v.data.words[0]=INPUT;v.data.words[1]=SOURCE;}break;
        case kAudioDevicePropertyStreams:if(input)WORD(INPUT);else EMPTY();break;
        case kAudioObjectPropertyControlList:if(input)WORD(SOURCE);else EMPTY();break;
        case kAudioDevicePropertyDeviceUID:STR(CFSTR("Codec"));break;
        case kAudioDevicePropertyModelUID:STR(CFSTR("X9-MT6681-Microphone"));break;
        case kAudioDevicePropertyTransportType:WORD('bltn');break;
        case kAudioDevicePropertyRelatedDevices:WORD(CODEC);break;
        case kAudioDevicePropertyClockDomain:WORD(0);break;
        case kAudioDevicePropertyDeviceIsAlive:WORD(ready);break;
        case kAudioDevicePropertyDeviceIsRunning:WORD(running!=0);break;
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:WORD(input && ready);break;
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:WORD(0);break;
        case kAudioDevicePropertyLatency:WORD(256);break;
        case kAudioDevicePropertySafetyOffset:WORD(256);break;
        case kAudioDevicePropertyZeroTimeStampPeriod:WORD(PERIOD);break;
        case kAudioDevicePropertyIsHidden:WORD(0);break;
        case kAudioDevicePropertyNominalSampleRate:NUMBER(X9_MIC_RATE);break;
        case kAudioDevicePropertyAvailableNominalSampleRates:
            v.exists=true;v.size=sizeof(AudioValueRange);v.data.range=(AudioValueRange){X9_MIC_RATE,X9_MIC_RATE};break;
        }
    } else if(o==INPUT) {
        switch(s) {
        case kAudioObjectPropertyOwnedObjects:EMPTY();break;
        case kAudioStreamPropertyIsActive:WORD(ready);break;
        case kAudioStreamPropertyDirection:WORD(1);break;
        case kAudioStreamPropertyTerminalType:WORD(kAudioStreamTerminalTypeMicrophone);break;
        case kAudioStreamPropertyStartingChannel:WORD(1);break;
        case kAudioStreamPropertyLatency:WORD(0);break;
        case kAudioStreamPropertyVirtualFormat:case kAudioStreamPropertyPhysicalFormat:
            v.exists=true;v.size=sizeof(AudioStreamBasicDescription);v.data.format=pcm_format();break;
        case kAudioStreamPropertyAvailableVirtualFormats:case kAudioStreamPropertyAvailablePhysicalFormats:
            v.exists=true;v.size=sizeof(AudioStreamRangedDescription);
            v.data.ranged=(AudioStreamRangedDescription){pcm_format(),{X9_MIC_RATE,X9_MIC_RATE}};break;
        }
    } else {
        switch(s) {
        case kAudioObjectPropertyOwnedObjects:EMPTY();break;
        case kAudioControlPropertyScope:WORD(kAudioObjectPropertyScopeInput);break;
        case kAudioControlPropertyElement:WORD(0);break;
        case kAudioSelectorControlPropertyCurrentItem:case kAudioSelectorControlPropertyAvailableItems:WORD(MIC_SOURCE);break;
        case kAudioSelectorControlPropertyItemName:case kAudioSelectorControlPropertyItemKind:
            if(qsize==4 && q && *(const UInt32 *)q==MIC_SOURCE) {
                if(s==kAudioSelectorControlPropertyItemName)STR(CFSTR("Internal Microphone"));else WORD('micr');
            }break;
        }
    }
#undef WORD
#undef STR
#undef EMPTY
#undef NUMBER
    return v;
}
static Boolean has_property(__unused AudioServerPlugInDriverRef d,AudioObjectID o,__unused pid_t p,const AudioObjectPropertyAddress *a)
{
    /* Selector-item metadata exists independently of its required qualifier. */
    if(o==SOURCE && a && !a->mElement &&
       (a->mSelector==kAudioSelectorControlPropertyItemName || a->mSelector==kAudioSelectorControlPropertyItemKind))return true;
    pthread_mutex_lock(&lock);bool yes=property(o,a,0,NULL).exists;pthread_mutex_unlock(&lock);return yes;
}
static Boolean settable(AudioObjectID o,const AudioObjectPropertyAddress *a)
{
    return a && !a->mElement && ((o==SOURCE && a->mSelector==kAudioSelectorControlPropertyCurrentItem) ||
        (o==CODEC && a->mSelector==kAudioDevicePropertyNominalSampleRate) ||
        (o==INPUT && (a->mSelector==kAudioStreamPropertyVirtualFormat || a->mSelector==kAudioStreamPropertyPhysicalFormat)));
}
static OSStatus is_settable(AudioServerPlugInDriverRef d,AudioObjectID o,pid_t p,const AudioObjectPropertyAddress *a,Boolean *out)
{
    if(!out)return '!siz';if(!has_property(d,o,p,a))return 'who?';*out=settable(o,a);return 0;
}
static OSStatus get_size(__unused AudioServerPlugInDriverRef d,AudioObjectID o,__unused pid_t p,
    const AudioObjectPropertyAddress *a,UInt32 n,const void *q,UInt32 *out)
{
    if(!out)return '!siz';pthread_mutex_lock(&lock);struct value v=property(o,a,n,q);pthread_mutex_unlock(&lock);
    if(!v.exists)return 'who?';*out=v.size;return 0;
}
static OSStatus get_data(__unused AudioServerPlugInDriverRef d,AudioObjectID o,__unused pid_t p,
    const AudioObjectPropertyAddress *a,UInt32 n,const void *q,UInt32 size,UInt32 *out,void *data)
{
    if(!out)return '!siz';*out=0;
    pthread_mutex_lock(&lock);struct value v=property(o,a,n,q);pthread_mutex_unlock(&lock);
    if(!v.exists)return 'who?';if(size<v.size || (v.size && !data))return '!siz';
    if(v.string)CFRetain(v.data.str);if(v.size)memcpy(data,&v.data,v.size);*out=v.size;return 0;
}
static OSStatus set_data(__unused AudioServerPlugInDriverRef d,AudioObjectID o,__unused pid_t p,
    const AudioObjectPropertyAddress *a,__unused UInt32 n,__unused const void *q,UInt32 size,const void *data)
{
    if(!settable(o,a))return 'who?';if(!data)return '!siz';
    if(o==SOURCE)return size==4 && *(const UInt32 *)data==MIC_SOURCE?0:'!dat';
    if(o==CODEC)return size==sizeof(Float64) && *(const Float64 *)data==X9_MIC_RATE?0:'!dat';
    if(size!=sizeof(AudioStreamBasicDescription))return '!siz';
    const AudioStreamBasicDescription *f=data,expected=pcm_format();
    return f->mSampleRate==expected.mSampleRate && f->mFormatID==expected.mFormatID &&
        f->mFormatFlags==expected.mFormatFlags && f->mBytesPerPacket==2 && f->mFramesPerPacket==1 &&
        f->mBytesPerFrame==2 && f->mChannelsPerFrame==1 && f->mBitsPerChannel==16?0:'!dat';
}
static OSStatus start_io(__unused AudioServerPlugInDriverRef d,AudioObjectID o,UInt32 id)
{
    if(o!=CODEC)return bad_object;
    pthread_mutex_lock(&lock);struct client *c=find_client(id);OSStatus e=0;
    if(!c)e=bad_object;
    else if(!c->running) {
        if(!running) {
            if(!probe_ready())e=unavailable;
            else {
                mic=open(X9_MIC_DEVICE,O_RDONLY|O_NONBLOCK|O_CLOEXEC);
                struct x9_mic_config config={.version=1,.rate=X9_MIC_RATE,.channels=1,.sample_bits=16,.source=X9_MIC_SOURCE_INTERNAL};
                if(mic<0 || ioctl(mic,X9_MIC_START,&config)<0) {
                    if(mic>=0)close(mic);mic=-1;e=unavailable;
                } else {anchor=mach_absolute_time();++seed;received=underrun=0;cycle_valid=false;}
            }
        }
        if(!e){c->running=true;++running;}
    }
    pthread_mutex_unlock(&lock);
    if(!e && host) {
        AudioObjectPropertyAddress a={kAudioDevicePropertyDeviceIsRunning,kAudioObjectPropertyScopeGlobal,0};
        host->PropertiesChanged(host,CODEC,1,&a);
    }
    return e;
}
static OSStatus stop_io(__unused AudioServerPlugInDriverRef d,AudioObjectID o,UInt32 id)
{
    if(o!=CODEC)return bad_object;
    pthread_mutex_lock(&lock);struct client *c=find_client(id);if(c)stop_client(c);pthread_mutex_unlock(&lock);
    if(host) {
        AudioObjectPropertyAddress a={kAudioDevicePropertyDeviceIsRunning,kAudioObjectPropertyScopeGlobal,0};
        host->PropertiesChanged(host,CODEC,1,&a);
    }
    return c?0:bad_object;
}
static OSStatus timestamp(__unused AudioServerPlugInDriverRef d,AudioObjectID o,__unused UInt32 id,
    Float64 *sample,UInt64 *time,UInt64 *out_seed)
{
    if(o!=CODEC || !sample || !time || !out_seed)return bad_object;
    if(pthread_mutex_trylock(&lock))return unavailable;
    OSStatus e=running?0:unavailable;
    if(!e) {
        UInt64 now=mach_absolute_time();double periods=floor((now-anchor)/(ticks_per_frame*PERIOD));
        *sample=periods*PERIOD;*time=anchor+(UInt64)(*sample*ticks_per_frame);*out_seed=seed;
    }
    pthread_mutex_unlock(&lock);return e;
}
static OSStatus will_io(__unused AudioServerPlugInDriverRef d,AudioObjectID o,__unused UInt32 id,
    UInt32 operation,Boolean *will,Boolean *in_place)
{
    if(o!=CODEC || !will || !in_place)return bad_object;
    *will=operation==kAudioServerPlugInIOOperationReadInput;*in_place=true;return 0;
}
static OSStatus begin_end(__unused AudioServerPlugInDriverRef d,AudioObjectID o,__unused UInt32 id,
    __unused UInt32 operation,__unused UInt32 frames,__unused const AudioServerPlugInIOCycleInfo *cycle)
{return o==CODEC?0:bad_object;}
static OSStatus do_io(__unused AudioServerPlugInDriverRef d,AudioObjectID o,AudioObjectID stream,UInt32 id,
    UInt32 operation,UInt32 frames,const AudioServerPlugInIOCycleInfo *cycle,void *buffer,__unused void *secondary)
{
    if(o!=CODEC || stream!=INPUT || operation!=kAudioServerPlugInIOOperationReadInput)return bad_object;
    if(frames>MAX_IO_FRAMES || (!buffer && frames))return '!siz';
    if(!frames)return 0;
    /* Zero only an explicit transient underflow. Never allocate, log, sleep,
     * wait on a mutex, or conceal a permanent hardware error in the IO path. */
    memset(buffer,0,(size_t)frames*2);
    if(pthread_mutex_trylock(&lock))return unavailable;
    struct client *c=find_client(id);OSStatus e=0;UInt32 have=0;
    bool timed=cycle && (cycle->mInputTime.mFlags&kAudioTimeStampSampleTimeValid) && isfinite(cycle->mInputTime.mSampleTime);
    if(c && c->running && timed && cycle_valid && cycle_seed==seed && cycle_frames==frames &&
       cycle_sample==cycle->mInputTime.mSampleTime) {
        /* Multiple HAL clients asking for the same input cycle must hear
         * the same samples, not advance the physical microphone twice. */
        memcpy(buffer,cycle_pcm,(size_t)frames*2);pthread_mutex_unlock(&lock);return 0;
    }
    if(!c || !c->running || mic<0)e=unavailable;
    else while(have<frames) {
        ssize_t n=read(mic,(int16_t *)buffer+have,(size_t)(frames-have)*2);
        if(n>0 && !(n&1)){have+=(UInt32)n/2;continue;}
        if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK))break;
        e=unavailable;ready=false;break;
    }
    received+=have;underrun+=frames-have;
    cycle_valid=!e && timed;
    if(cycle_valid) {
        memcpy(cycle_pcm,buffer,(size_t)frames*2);cycle_sample=cycle->mInputTime.mSampleTime;
        cycle_seed=seed;cycle_frames=frames;
    }
    pthread_mutex_unlock(&lock);return e;
}
static AudioServerPlugInDriverInterface interface={NULL,query,retain_driver,release_driver,initialize,
    create,destroy,add_client,remove_client,configuration,configuration,has_property,is_settable,
    get_size,get_data,set_data,start_io,stop_io,timestamp,will_io,begin_end,do_io,begin_end};
static AudioServerPlugInDriverInterface *driver=&interface;
void *X9Microphone_Create(__unused CFAllocatorRef allocator,CFUUIDRef type)
{return type && CFEqual(type,kAudioServerPlugInTypeUUID)?&driver:NULL;}
