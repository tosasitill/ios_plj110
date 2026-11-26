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
 kIOReturnNoResources=3, kIOReturnNotOpen=4;
constexpr uint64_t MACH_PORT_NULL=0;
constexpr unsigned kOSAsyncRef64Count=8, kIOAsyncCalloutRefconIndex=2;
using OSAsyncReference64 = io_user_reference_t[8];
#define bzero(p,n) std::memset((p),0,(n))
#define APPLE_KEXT_OVERRIDE override
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
uint64_t sent[4], sentport;
struct IOUserClient : OSObject {
 static int releaseAsyncReference64(OSAsyncReference64 ref) { assert(ref[0]); ++releases; return 0; }
 static int sendAsyncResult64(OSAsyncReference64 ref, int result, io_user_reference_t *a, unsigned n) {
  assert(ref[0] && result==0 && n==4); sentport=ref[0]; std::memcpy(sent,a,sizeof(sent)); ++sends; return 0;
 }
};
struct IOExternalMethodArguments { uint64_t asyncWakePort; io_user_reference_t *asyncReference; uint32_t asyncReferenceCount; };
class SweetEventListener : public OSObject {
public:
 OSAsyncReference64 ref{}; IOLock *sendLock=nullptr; bool ownsRight=false; io_user_reference_t key=0;
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
#include "../../kernel/src/iokit/Kernel/arm/SweetSurfaceEventListeners.inc"
int main() {
 IOSurfaceRootUserClient client;
 uint64_t ref[8]={0x1234,0x89,0x456}; IOExternalMethodArguments a{0x1234,ref,3};
 assert(client.eventListenerAdd(nullptr)==kIOReturnBadArgument && releases==0);
 assert(client.eventListenerAdd(&a)==0);
 assert(client.eventListenerAdd(&a)==kIOReturnBadArgument && releases==0);
 SweetEventListener *hold=client.eventListenerCopy(0x456); assert(hold);
 assert(client.eventListenerRemove(0x456)==0 && releases==0);
 assert(client.eventListenerRemove(0x456)==kIOReturnBadArgument);
 assert(!client.eventListenerCopy(0x456));
 assert(client.eventListenerDeliver(hold,0x100,0x200,0x123456789abcdef0ULL)==0);
 assert(sends==1 && sentport==0x1234 && sent[0]==0x100 && sent[1]==0x200 && sent[2]==0x123456789abcdef0ULL && sent[3]==0x12345678);
 assert(client.eventListenerDeliver(hold,0,0,0)==kIOReturnBadArgument && sends==1);
 hold->release(); assert(releases==1);
 for(unsigned i=0;i<64;i++) { ref[2]=i; assert(client.eventListenerAdd(&a)==0); }
 ref[2]=64; assert(client.eventListenerAdd(&a)==kIOReturnNoResources && releases==1);
 client.eventListenersClose(); assert(releases==65);
 client.eventListenersClose(); assert(releases==65);
 assert(client.eventListenerAdd(&a)==kIOReturnNotOpen && releases==65);
 {
  IOSurfaceRootUserClient racing;
  std::atomic<int> accepted{0};
  std::vector<std::thread> threads;
  for (unsigned i=0;i<64;i++) threads.emplace_back([&,i] {
   uint64_t local[8]={0x1234,0x89,1000+i}; IOExternalMethodArguments a2{0x1234,local,3};
   int r=racing.eventListenerAdd(&a2);
   assert(r==kIOReturnSuccess || r==kIOReturnNotOpen);
   if (r==kIOReturnSuccess) ++accepted;
  });
  std::thread closer([&]{ racing.eventListenersClose(); });
  for (auto &t:threads) t.join();
  closer.join();
  assert(releases==65+accepted);
 }
 puts("PASS: add/duplicate/remove/retained send/capacity/close/race and rights");
}
