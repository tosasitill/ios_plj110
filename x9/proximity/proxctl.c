/* Own-code iOS16 tool. Normal registry property path; no custom ABI,
 * sensor register writes, synthetic events, calibration or SCP reset. */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static void show(io_service_t service)
{
 const char *names[]={"X9ProxBackend","DetectionMode","X9ProxSCPReady",
     "X9ProxSCPEnabled","X9ProxControlPending","X9ProxErrno",
     "X9ProxDataValid","X9ProxPhysicalTransportReady","X9ProxDiagnostic",
     "X9ProxSequence","X9ProxNear","X9ProxOneshot","X9ProxSteps"};
 for (unsigned i=0;i<sizeof(names)/sizeof(names[0]);i++) {
  CFStringRef key=CFStringCreateWithCString(NULL,names[i],kCFStringEncodingUTF8);
  CFTypeRef value=IORegistryEntryCreateCFProperty(service,key,NULL,0);
  CFRelease(key);
  printf("%s=",names[i]);
  if (!value) printf("absent");
  else if (CFGetTypeID(value)==CFBooleanGetTypeID()) printf("%d",CFBooleanGetValue(value));
  else if (CFGetTypeID(value)==CFNumberGetTypeID()) {
   long long n=0; CFNumberGetValue(value,kCFNumberLongLongType,&n); printf("%lld",n);
  } else if (CFGetTypeID(value)==CFStringGetTypeID()) {
   char buf[128]; if (CFStringGetCString(value,buf,sizeof(buf),kCFStringEncodingUTF8)) printf("%s",buf);
   else printf("unprintable");
  } else printf("unexpected type");
  if (value) CFRelease(value);
  printf(i+1==sizeof(names)/sizeof(names[0])?"\n":" ");
 }
 fflush(stdout);
}
static int number(const char *s,int maximum,int *out)
{
 char *end; long n=strtol(s,&end,10);
 if (!*s || *end || n<0 || n>maximum) return 0;
 *out=(int)n; return 1;
}
int main(int argc,char **argv)
{
 int mode=-1,watch=0;
 if (argc==1 || (argc==2 && !strcmp(argv[1],"--status"))) { }
 else if (argc==3 && !strcmp(argv[1],"--mode") && number(argv[2],5,&mode)) { }
 else if (argc==3 && !strcmp(argv[1],"--watch") && number(argv[2],60,&watch) && watch>0) { }
 else { fprintf(stderr,"usage: proxctl [--status | --mode 0..5 | --watch 1..60(seconds)]\n"); return 2; }
 io_service_t service=IOServiceGetMatchingService(MACH_PORT_NULL,IOServiceMatching("X9Proximity"));
 if (!service) { fprintf(stderr,"X9Proximity absent (boot x9_prox=3 for native, 2 for diagnostic)\n"); return 1; }
 int result=0;
 if (mode>=0) {
  if (geteuid()) {
   fprintf(stderr,"--mode requires root; --status/--watch remain read-only\n");
   IOObjectRelease(service); return 1;
  }
  /* iOS SDK marks this unavailable; original 20H392 IOKit exports it.
   * Use the same runtime resolution as x9/usb/native_iokit.c, not a new ABI. */
  typedef kern_return_t (*set_properties_fn)(io_registry_entry_t,CFTypeRef);
  set_properties_fn set_properties=(set_properties_fn)dlsym(RTLD_DEFAULT,"IORegistryEntrySetCFProperties");
  if (!set_properties) {
   fprintf(stderr,"IORegistryEntrySetCFProperties absent\n");
   IOObjectRelease(service); return 1;
  }
  CFNumberRef value=CFNumberCreate(NULL,kCFNumberIntType,&mode);
  if (!value) { IOObjectRelease(service); return 1; }
  const void *key=CFSTR("DetectionMode"),*v=value;
  CFDictionaryRef props=CFDictionaryCreate(NULL,&key,&v,1,
      &kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
  if (!props) { CFRelease(value); IOObjectRelease(service); return 1; }
  kern_return_t kr=set_properties(service,props);
  CFRelease(props); CFRelease(value);
  printf("DetectionMode=%d submission=0x%x (NOT proof of ACK or physical DATA)\n",mode,kr);
  if (kr) result=1;
 }
 unsigned count=watch?(unsigned)watch*5:1;
 for (unsigned i=0;i<count;i++) { show(service); if (i+1<count) usleep(200000); }
 IOObjectRelease(service); return result;
}
