// Exercises the production listener .inc against host substitutes for IOKit.
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
using UInt32 = uint32_t;
using io_user_reference_t = uint64_t;
using IOReturn = int;
constexpr IOReturn kIOReturnSuccess=0, kIOReturnBadArgument=1, kIOReturnNoMemory=2,
 kIOReturnNoResources=3, kIOReturnNotOpen=4, kIOReturnAborted=5;
constexpr uint64_t MACH_PORT_NULL=0;
constexpr unsigned kOSAsyncRef64Count=8, kIOAsyncCalloutRefconIndex=2;
using OSAsyncReference64 = io_user_reference_t[8];
#define IOLog(...) ((void)0)
#define bzero(p,n) std::memset((p),0,(n))
#define APPLE_KEXT_OVERRIDE override
#define OSTypeAlloc(x) new x
#define OSSafeReleaseNULL(p) do { if(p) { (p)->release(); (p)=nullptr; } } while(0)
#define OSDeclareDefaultStructors(x)
#define OSDefineMetaClassAndStructors(x,y)
struct IOLock { std::mutex mutex; };
IOLock *IOLockAlloc() { return new IOLock; }
void IOLockFree(IOLock *p) { delete p; }
void IOLockLock(IOLock *p) { p->mutex.lock(); }
void IOLockUnlock(IOLock *p) { p->mutex.unlock(); }
struct OSObject {
 std::atomic<int> count{1};
 virtual ~OSObject() = default;
 virtual bool init() { return true; }
 virtual void free() {}
 void retain() { ++count; }
 void release() { if (--count==0) { free(); delete this; } }
};
std::atomic<int> releases{0}, sends{0};
int forcedResult=0; uint64_t fullPort=0;
std::vector<uint64_t> delivered;
uint64_t sent[4], sentport;
struct IOUserClient : OSObject {
 static int releaseAsyncReference64(OSAsyncReference64 ref) { assert(ref[0]); ++releases; return 0; }
 static int sendAsyncResult64WithOptions(OSAsyncReference64 ref, int result, io_user_reference_t *a, unsigned n, unsigned) {
  assert(ref[0] && result==0 && n==4); sentport=ref[0]; std::memcpy(sent,a,sizeof(sent)); ++sends; if (ref[0]==fullPort) return 100; if (!forcedResult) delivered.push_back(a[0]); return forcedResult;
 }
};
constexpr int MACH_SEND_TIMED_OUT=100; constexpr unsigned kIOUserNotifyOptionCanDrop=1;
using thread_call_param_t=void *; struct Call { void (*fn)(void *,void *); void *arg; bool pending; }; using thread_call_t=Call *;
thread_call_t lastCall=nullptr;
constexpr unsigned THREAD_CALL_PRIORITY_KERNEL=1, THREAD_CALL_OPTIONS_ONCE=1;
thread_call_t thread_call_allocate_with_options(void (*fn)(void*,void*),void *arg,unsigned,unsigned) { return lastCall=new Call{fn,arg,false}; }
void thread_call_free(thread_call_t c) { delete c; }
void thread_call_enter(thread_call_t c) { c->pending=true; }
void thread_call_enter_delayed(thread_call_t c,uint64_t) { c->pending=true; }
void thread_call_cancel_wait(thread_call_t c) { c->pending=false; }
constexpr unsigned NSEC_PER_MSEC=1000000; void clock_interval_to_deadline(unsigned t,unsigned n,uint64_t *v) { *v=(uint64_t)t*n; }
struct IOExternalMethodArguments { uint64_t asyncWakePort; io_user_reference_t *asyncReference; uint32_t asyncReferenceCount; };
class SweetEventListener : public OSObject {
public:
 OSAsyncReference64 ref{}; IOLock *sendLock=nullptr; bool ownsRight=false, canceled=false; io_user_reference_t key=0;
 bool initReference(IOExternalMethodArguments *args); void free() override;
};
class IOSurfaceRootUserClient : public IOUserClient {
 IOLock *fLock=IOLockAlloc(); SweetEventListener *fEventListeners[64]{}; bool fEventListenersClosed=false;
public:
 ~IOSurfaceRootUserClient() { eventListenersClose(); IOLockFree(fLock); }
 IOReturn eventListenerAdd(IOExternalMethodArguments *args);
 IOReturn eventListenerRemove(io_user_reference_t key);
 SweetEventListener *eventListenerCopy(io_user_reference_t key);
 static IOReturn eventListenerDeliver(SweetEventListener *, io_user_reference_t, io_user_reference_t, uint64_t);
 void eventListenersClose();
};
static void sweet_event_cancel_listener(SweetEventListener *listener) {
 IOLockLock(listener->sendLock);
 listener->canceled=true;
 IOLockUnlock(listener->sendLock);
}
#include "../../kernel/src/iokit/Kernel/arm/SweetSurfaceEventListeners.inc"

class SweetSharedEventCounter : public OSObject {
 std::atomic<uint64_t> current{0};
public:
 uint64_t value() const { return current.load(); }
 bool advance(uint64_t n) { auto x=current.load(); if (n<=x) return false; current.store(n); return true; }
};
#include "../../kernel/src/iokit/Kernel/arm/SweetSharedEventPending.inc"
int main() {
 IOSurfaceRootUserClient client;
 uint64_t ref[8]={0x1234,0x89,0x456}; IOExternalMethodArguments a{0x1234,ref,3};
 assert(client.eventListenerAdd(&a)==0);
 auto *l=client.eventListenerCopy(0x456); assert(l);
 auto *c=new SweetSharedEventCounter; auto *q=SweetSharedEventPending::make(c); assert(q); c->release();
 assert(q->subscribe(l,1,2,10)==0); assert(!q->drainOne(nullptr));
 assert(q->advance(10)); IOReturn ret=-1; assert(q->drainOne(&ret) && ret==0);
 assert(sent[0]==1 && sent[1]==2 && sent[2]==10 && sends==1); assert(!q->drainOne(nullptr));
 assert(q->subscribe(l,1,2,5)==0 && q->drainOne(&ret));
 forcedResult=MACH_SEND_TIMED_OUT;
 assert(q->subscribe(l,6,7,5)==0 && lastCall->pending);
 lastCall->pending=false; lastCall->fn(lastCall->arg,nullptr);
 assert(lastCall->pending); forcedResult=0;
 lastCall->pending=false; lastCall->fn(lastCall->arg,nullptr);
 assert(!lastCall->pending && !q->drainOne(&ret));
 // A full listener must not block another; reused slot must not reorder A.
 uint64_t ref2[8]={0x5678,0x89,0x777}; IOExternalMethodArguments a2{0x5678,ref2,3};
 assert(client.eventListenerAdd(&a2)==0);
 auto *l2=client.eventListenerCopy(0x777); assert(l2);
 fullPort=0x1234; delivered.clear();
 assert(q->subscribe(l,101,2,5)==0);
 assert(q->subscribe(l,102,2,5)==0);
 assert(q->subscribe(l2,201,2,5)==0);
 lastCall->pending=false; lastCall->fn(lastCall->arg,nullptr);
 assert(lastCall->pending && delivered.size()==1 && delivered[0]==201);
 fullPort=0; delivered.clear();
 lastCall->pending=false; lastCall->fn(lastCall->arg,nullptr);
 assert(delivered.size()==2 && delivered[0]==101 && delivered[1]==102);
 // Reuse an earlier physical slot while an older item remains in another slot.
 assert(q->subscribe(l2,301,2,5)==0); assert(q->subscribe(l,103,2,5)==0);
 assert(q->drainOne(&ret)); assert(q->subscribe(l,104,2,5)==0);
 delivered.clear(); lastCall->pending=false; lastCall->fn(lastCall->arg,nullptr);
 assert(delivered.size()==2 && delivered[0]==103 && delivered[1]==104);
 fullPort=0x1234; delivered.clear();
 assert(q->subscribe(l,105,2,5)==0 && q->subscribe(l2,202,2,5)==0);
 lastCall->pending=false; lastCall->fn(lastCall->arg,nullptr);
 assert(delivered.size()==1 && delivered[0]==202 && lastCall->pending);
 assert(q->cancelListener(l)==1); fullPort=0;
 lastCall->pending=false; lastCall->fn(lastCall->arg,nullptr);
 assert(delivered.size()==1 && !lastCall->pending);
 l2->release(); assert(client.eventListenerRemove(0x777)==0);
 for(unsigned i=0;i<64;i++) assert(q->subscribe(l,1,2,100)==0);
 assert(q->subscribe(l,1,2,100)==kIOReturnNoResources);
 assert(q->cancelListener(l)==64 && q->cancelListener(l)==0);
 std::thread sub([&]{ assert(q->subscribe(l,3,4,11)==0); });
 std::thread adv([&]{ assert(q->advance(11)); });
 sub.join(); adv.join(); assert(q->drainOne(&ret));
 assert(q->subscribe(l,1,2,12)==0); q->close();
 assert(q->subscribe(l,1,2,12)==kIOReturnNotOpen && !q->advance(12) && !q->drainOne(nullptr));
 q->release(); l->release(); assert(client.eventListenerRemove(0x456)==0 && releases==2);
 puts("PASS: pending threshold, ready, timeout retry, cancellation, capacity and close");
}
