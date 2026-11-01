/* Explicit private compute service. The opt-in Metal plugin is published
 * after the hardware baseline; compute clients use the GPU permission check. */
#include <IOKit/IOUserClient.h>
#include <IOKit/IOService.h>
#include <IOKit/IOKitKeys.h>
#include <sys/proc.h>
#include <sys/kauth.h>
#include <kern/task.h>
extern "C" int sweet_x9_gpu_user_compute(void *);
static IOLock *metal_service_lock;
static IOLock *desktop_frame_lock;
static bool metal_service_disabled;
static uint32_t metal_sequence,metal_root_passes,metal_mobile_passes;
extern "C" { int sweet_x9_metal_payload_ready=0; }
static bool metal_discovery_enabled;
extern "C" bool sweet_x9_metal_discovery_ready(void) { return metal_discovery_enabled&&sweet_x9_metal_payload_ready; }
class SweetX9ComputeUserClient : public IOUserClient {
 OSDeclareDefaultStructors(SweetX9ComputeUserClient);
public:
 virtual bool initWithTask(task_t task,void *security,UInt32 type,OSDictionary *properties) APPLE_KEXT_OVERRIDE {
  if(type||clientHasPrivilege(task,kIOClientPrivilegeForeground)!=kIOReturnSuccess)return false;
  bool initialized=IOUserClient::initWithTask(task,security,type,properties);
  if(initialized) {
   char caller[32]={0};proc_selfname(caller,sizeof(caller));
   IOLog("X9 METAL API: client opened caller=%s pid=%d uid=%u GPU permission accepted\n",caller,proc_selfpid(),kauth_cred_getuid(kauth_cred_get()));
  }
  return initialized;
 }
 virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE {
  if(!isInactive())terminate();return kIOReturnSuccess;
 }
 virtual IOReturn externalMethod(uint32_t selector,IOExternalMethodArguments *args,
  IOExternalMethodDispatch *,OSObject *,void *) APPLE_KEXT_OVERRIDE {
  if(clientHasPrivilege(current_task(),kIOClientPrivilegeForeground)!=kIOReturnSuccess)return kIOReturnNotPrivileged;
  if(!args||args->scalarInputCount||args->scalarOutputCount||
     args->structureInputDescriptor||args->structureOutputDescriptor)return kIOReturnBadArgument;
  if(selector==0) {
   if(args->structureInputSize||args->structureOutputSize!=sizeof(X9MetalCaps)||!args->structureOutput)return kIOReturnBadArgument;
   X9MetalCaps caps={X9_METAL_ABI,X9_METAL_MAX,128,0xe8800010};
   memcpy(args->structureOutput,&caps,sizeof(caps));return kIOReturnSuccess;
  }
  if(selector!=1)return kIOReturnUnsupported;
  if(args->structureInputSize!=sizeof(X9MetalRequest)||args->structureOutputSize!=sizeof(X9MetalResponse)||
     !args->structureInput||!args->structureOutput)return kIOReturnBadArgument;
  X9MetalJob *job=(X9MetalJob *)IOMallocZero(sizeof(X9MetalJob));
  if(!job)return kIOReturnNoMemory;
  memcpy(&job->request,args->structureInput,sizeof(job->request));
  const X9MetalRequest &r=job->request;
  if(r.abi!=X9_METAL_ABI||r.reserved||!r.count||r.count>X9_METAL_MAX||
     !r.local_x||r.local_x>128||r.count%r.local_x) {
   IOFree(job,sizeof(*job));return kIOReturnBadArgument;
  }
  IOLockLock(metal_service_lock);
  int result=metal_service_disabled?-1:sweet_x9_gpu_user_compute(job);
  if(result<0)metal_service_disabled=true; /* Unknown power/DMA lifetime: never retry. */
  IOReturn kr=kIOReturnIOError;
  if(result==1&&job->computed&&job->cleaned) {
   job->response.abi=X9_METAL_ABI;job->response.count=r.count;
   job->response.sequence=++metal_sequence;job->response.status=0;
   uid_t verified_uid=kauth_cred_getuid(kauth_cred_get());
   if(verified_uid==0)metal_root_passes++;else if(verified_uid==501)metal_mobile_passes++;
   memcpy(args->structureOutput,&job->response,sizeof(job->response));kr=kIOReturnSuccess;
   char caller[32]={0};proc_selfname(caller,sizeof(caller));
   IOLog("X9 METAL API: sequence=%u count=%u local=%u hardware-result returned after complete cleanup caller=%s pid=%d uid=%u\n",
    metal_sequence,r.count,r.local_x,caller,proc_selfpid(),kauth_cred_getuid(kauth_cred_get()));
  }else IOLog("X9 METAL API: request failed result=%d disabled=%u; no success response\n",result,metal_service_disabled);
  IOLockUnlock(metal_service_lock);
  IOFree(job,sizeof(*job));return kr;
 }
};
OSDefineMetaClassAndStructors(SweetX9ComputeUserClient,IOUserClient);
class SweetX9Compute : public IOService {
 OSDeclareDefaultStructors(SweetX9Compute);
public:
 virtual IOReturn newUserClient(task_t task,void *security,UInt32 type,
  OSDictionary *properties,IOUserClient **handler) APPLE_KEXT_OVERRIDE {
  if(!handler)return kIOReturnBadArgument;
  SweetX9ComputeUserClient *client=new SweetX9ComputeUserClient;
  if(!client)return kIOReturnNoMemory;
  if(!client->initWithTask(task,security,type,properties)) {client->release();return kIOReturnNotPrivileged;}
  if(!client->attach(this)||!client->start(this)) {
   if(client->getProvider()==this)client->detach(this);
   client->release();return kIOReturnError;
  }
  *handler=client;return kIOReturnSuccess;
 }
};
OSDefineMetaClassAndStructors(SweetX9Compute,IOService);
extern "C" void sweet_x9_compute_publish(void) {
 static bool published;
 if(published||!metal_startup_verified)return;
 int system_enabled=0;
 if(!PE_parse_boot_argn("x9_metal_system",&system_enabled,sizeof(system_enabled))||(system_enabled!=1&&system_enabled!=2))return;
 metal_service_lock=IOLockAlloc();
 if(!metal_service_lock)return;
 SweetX9Compute *service=new SweetX9Compute;
 IOService *provider=IOService::getServiceRoot();
 if(!service||!provider||!service->init()||!service->attach(provider)||!service->start(provider)) {
  if(service) {if(service->getProvider()==provider)service->detach(provider);service->release();}
  IOLockFree(metal_service_lock);metal_service_lock=NULL;return;
 }
 service->setName(X9_METAL_SERVICE);
 service->setProperty("X9ComputeABI",X9_METAL_ABI,32);
 service->setProperty("X9ComputeMaxElements",X9_METAL_MAX,32);
 service->setProperty("X9ComputeScope","system-compute");
 service->setProperty("X9ComputeUserAccess","GPU-permitted");
 /* Discovery strings are synthesized by IOAcceleratorES::copyProperty
  * for system compute clients. Rendering owners keep a passive property view. */
 int opt=0;
 if(PE_parse_boot_argn("x9_metal_system",&opt,sizeof(opt))&&(opt==1||opt==2)) {
  __asm__ volatile("dmb ish" ::: "memory");metal_discovery_enabled=true;
  IOLog("X9 METAL API: system compute discovery enabled; render-owner fallback retained\n");
 }
 desktop_frame_lock=IOLockAlloc();
 published=true;service->registerService();
 IOLog("X9 METAL API: SweetX9Compute registered after75-dispatch baseline and power release; copied per-request ABI1, max256; GPU-permitted clients\n");
}


#include "SweetX9DesktopWorker.inc"
