/*
 * Built-in IOMobileFramebuffer for SWEET: the ABL's continuous-splash
 * framebuffer (1080x2400 a8r8g8b8 in RAM, the panel scans it out by
 * itself) as the main display, driven by CoreAnimation's CPU renderer.
 *
 * IOMobileFramebuffer.framework (iOS 16.7) counts /arm-io children with
 * device_type "display-subsystem", waits for that many services matching
 * IOServiceMatching("IOMobileFramebuffer"), opens each with type 0 and
 * reads canvas-width/canvas-height from the service's provider. The class
 * here (AppleCLCD, an IOMobileFramebuffer) matches the /arm-io/disp0 nub.
 *
 * User client selectors used by CoreAnimation:
 *   3  GetLayerDefaultSurface [layer, ?] -> [IOSurface port name]
 *   4  SwapBegin -> [swap id]
 *   5  SwapEnd, struct 0x46c: +0x50 swap id, +0x54 IOSurface id per layer
 *      (4 layers), +0x64 rect a / +0xc4 rect b per layer (int x,y,w,h),
 *      +0x104 enabled layer mask
 *   6  SwapWait, 7 GetID, 8 GetDisplaySize -> [w, h],
 *   12 RequestPowerChange [state]: 0 = display off (backboardd, power
 *      button / auto-lock), else on. Off fades the panel out over a few
 *      vsyncs (halving each pixel) and keeps it black until on again;
 *      the panel itself stays powered (AMOLED black pixels are off).
 *   18 IsMainDisplay,
 *   28 GetDotPitch, 72 Enable/DisableNotifications [func, refcon, type, 0]
 * Everything else is accepted and logged once.
 *
 * SwapEnd copies the enabled layers' surfaces into the panel memory (no
 * scaling, CPU copy). Vsync (notification type 5) is a 60 Hz timer that
 * sends (count, mach_absolute_time, 0) as 32-bit halves to clients that
 * enabled it, through the port they registered (IOConnectSetNotificationPort).
 *
 * The panel memory itself is published as an IOSurface and returned as the
 * default framebuffer surface. The kernel text console keeps drawing over
 * the GUI by default: boot-arg sweetfb_noconsole=1 stops drawing it from
 * the first swap on (it still reaches the persistent log; panics draw).
 */
#include "SweetSurface.h"
#include "SweetBoard.h"
#include "X9Desktop.h"
static uint64_t x9_fb_present_seq;

/*
 * Diagnostic logging enabled by the exact boot-arg token x9vf, the same token
 * the userspace capture reads. It only raises the sample density and adds
 * layer/damage lines: no panel timing, copy or compositing decision changes.
 */
static bool
x9_diag_logging(void)
{
	static int enabled = -1;
	if (enabled < 0) {
		enabled = 0;
		char *args = PE_boot_args();
		for (const char *p = args; p != NULL && *p != 0;) {
			while (*p != 0 && (unsigned char)*p <= 32)
				p++;
			const char *end = p;
			while (*end != 0 && (unsigned char)*end > 32)
				end++;
			if (end - p == 4 && memcmp(p, "x9vf", 4) == 0)
				enabled = 1;
			p = end;
		}
		if (enabled)
			IOLog("X9 DIAG: x9vf active; panel sampler 64x64, layer and damage rectangles logged\n");
	}
	return enabled != 0;
}

extern "C" int x9_lk_gui_present(void);
extern "C" int x9_lk_gui_busy(void);
extern "C" int x9_lk_gui_cancel(void);
extern "C" void x9_lk_gui_probe(void);
extern "C" int x9_lk_overlay(unsigned index, uint64_t *address, uint64_t *bytes,
    unsigned *engine, uint32_t *pitch, uint32_t *height);
extern "C" int x9_lk_overlay_level(void);
extern "C" int x9_lk_panel_dcs(const uint8_t *data, unsigned len, int readback, uint32_t rd[4]);
extern "C" void sweet_watchdog_pet(void);   /* osfmk/arm64/sweet_early.c */
extern "C" int sweet_x9_render_display_try_enter(void);
extern "C" void sweet_x9_render_owner_leave(void);
extern "C" int sweet_console_muted;     /* osfmk/console/serial_console.c */
extern "C" void sweet_console_retarget(vm_offset_t va);   /* osfmk/console/video_console.c */
extern "C" void (*sweet_console_show_hook)(void);
extern "C" void topaz_modem_init(void);
extern "C" void sweet_busy_init(void);              /* iokit/Kernel/arm/SweetBusy.cpp (boot-arg sweetbusy=N) */             /* iokit/Kernel/arm/TopazModem.cpp (boot-arg modem=1) */
extern "C" int sweet_console_copy_dirty(void *dst);

/*
 * topaz: a surface the MDP may still be fetching counts as in use, so
 * CoreAnimation does not render into it. IOSurfaceClientIsInUse (iOS 16.7)
 * reads the u64 at +0x18 of the shared state page; held from SwapEnd until
 * one vsync after the next frame replaced it on the panel.
 */
static void
sweet_hold(SweetSurface *s, bool on)
{
	if (sweet_board_x676c()) {
		return;
	}
	OSAddAtomic64(on ? 1 : -1, (volatile SInt64 *)((uint8_t *)s->state->getBytesNoCopy() + 0x18));
}

static void
sweet_hold_set(SweetSurface **set, bool on)
{
	for (uint32_t i = 0; i < 4; i++) {
		if (set[i] != NULL && !on) {
			sweet_hold(set[i], false);
			set[i]->release();
			set[i] = NULL;
		}
	}
}
class AppleCLCD;
static AppleCLCD *sweet_cons_fb;
static void sweet_cons_show(void);
class OSDictionary;
static void sweet_bl_log(const char *who, OSDictionary *d);
extern "C" void sweet_dcache_clean(vm_offset_t addr, unsigned length);  /* osfmk/arm/caches.c */
extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

static volatile uint32_t *sMtkDsi = NULL;
static volatile uint32_t *sMtkMutex = NULL;
static volatile uint32_t *sMtkWdt = NULL;

static void
mtk_dsi_push_hw(void)
{
	if (!sweet_board_x676c()) {
		return;
	}
	/*
	 * Native MT6789 DSI scanout on ili7835 panel at 60 Hz VSYNC.
	 * Replaces the bare-metal shim CPU 7 display pump.
	 */
	if (sMtkDsi == NULL) {
		sMtkDsi = (volatile uint32_t *)ml_io_map(0x14013000, 0x1000);
	}
	if (sMtkMutex == NULL) {
		sMtkMutex = (volatile uint32_t *)ml_io_map(0x14001000, 0x1000);
	}
	if (sMtkWdt == NULL) {
		sMtkWdt = (volatile uint32_t *)ml_io_map(0x10007000, 0x1000);
	}
	if (sMtkWdt != NULL) {
		sMtkWdt[0x08 / 4] = 0x1971; /* Kick MTK TOPRGU watchdog */
	}
	if (sMtkDsi != NULL && sMtkMutex != NULL) {
		sMtkDsi[0x0c / 4] = 0;   /* DSI_INTSTA */
		sMtkMutex[0x20 / 4] = 1; /* MUTEX0_EN */
		sMtkDsi[0x00 / 4] = 0;   /* DSI_START = 0 */
		sMtkDsi[0x00 / 4] = 1;   /* DSI_START = 1 */
	}
}

#if defined(SWEET)
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <kern/thread_call.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/system.h>
#include <pexpert/pexpert.h>
#include <kern/clock.h>
#include <sys/proc.h>

enum {
	kSweetFBGetLayerDefaultSurface  = 3,
	kSweetFBSwapBegin               = 4,
	kSweetFBSwapEnd                 = 5,
	kSweetFBSwapWait                = 6,
	kSweetFBGetID                   = 7,
	kSweetFBGetDisplaySize          = 8,
	kSweetFBRequestPowerChange      = 12,
	kSweetFBIsMainDisplay           = 18,
	kSweetFBGetDotPitch             = 28,
	kSweetFBNotifications           = 72,
};

#define SWEET_FB_SWAP_REC_SIZE          0x46c
#define SWEET_FB_LAYERS                 4
#define SWEET_FB_FADE_STEPS             30      /* ~0.5 s at 60 Hz */
#define SWEET_FB_MAX_NITS               500     /* IOMFBBrightnessLevel at panel DBV 0x7ff */
/* X9 ac287 (oplus DT): DBV 1..4094, normal max 3515 (above = HBM), default
 * 35%. oplus_panel_set_backlight_cmdq writes level>>8, level&0xff into the
 * 51 00 00 template and drops level 1, so the floor is 2. */
#define X9_BL_DBV_MIN                   2
#define X9_BL_DBV_MAX                   3515
#define X9_BL_DBV_DEFAULT               1433
#define SWEET_FB_NOTIFY_TYPES           8
#define SWEET_FB_NOTIFY_VSYNC           5
#define SWEET_FB_VSYNC_US               16667
#define SWEET_FB_DOT_PITCH              458     /* ppi, as the iPhone X panel */
/*
 * The iOS userland only has status bar / layout data for real iPhone
 * screens (UIKit: "No status bar visual provider for screen", the red
 * rdar://45025538 bar for 360x800 points). The display is therefore
 * presented as the iPhone X panel, 1125x2436 (375x812 points @3x), and the
 * centered 1080x2400 part of it is shown on the 1080x2400 panel.
 * Boot-arg sweetfb_native=1 presents the real panel size instead.
 */
#define SWEET_FB_LOGICAL_W              1125
#define SWEET_FB_LOGICAL_H              2436
#define SWEET_FB_FORMAT_BGRA            0x42475241

class IOMobileFramebuffer : public IOService {
	OSDeclareDefaultStructors(IOMobileFramebuffer);
};

OSDefineMetaClassAndStructors(IOMobileFramebuffer, IOService);

class IOMobileFramebufferUserClient;

class AppleCLCD : public IOMobileFramebuffer {
	OSDeclareDefaultStructors(AppleCLCD);

	IOWorkLoop         *fWorkLoop;
	IOTimerEventSource *fVsync;
	IOTimerEventSource *fX9PresentTimer;
	uint64_t fX9PresentBegan, fX9LastPoll;
	uint64_t fCadenceTEFrames, fCadenceTETimeouts, fCadenceTEWaitNs, fCadenceTEGapNs;
	uint64_t            fVsyncCount;
	uint32_t            fTickHz;
	uint64_t            fTickPeriod, fTickDeadline;
	uint64_t            fCadenceStart, fCadenceTicks, fCadenceCopies, fCadenceSubmits;
	uint64_t            fCadenceMissed, fCadenceGpuBusy, fCadenceDisplayBusy;
	uint64_t            fCadenceCopyNs, fCadenceMaxCopyNs, fCadenceMaxLateNs;

	static void vsyncFired(OSObject *owner, IOTimerEventSource *sender);
	static void x9PresentFired(OSObject *owner, IOTimerEventSource *sender);
	void x9QueuePresent(void);
	void scheduleVsync(IOTimerEventSource *sender);

public:
	uint64_t      fPhys;
	uint32_t      fWidth, fHeight, fRowBytes;
	uint32_t      fLogicalW, fLogicalH;     /* size reported to userland */
	int32_t       fOffX, fOffY;             /* logical origin of the panel */
	uintptr_t     fPixels;          /* kernel mapping of the panel memory */
	IOMemoryMap  *fMap;
	SweetSurface *fSurface;         /* the panel memory as an IOSurface */
	IOLock       *fLock;            /* swaps and client list */
	OSArray      *fClients;
	uint32_t      fSwapID;
	bool          fConsolePaused;
	uint8_t       fPending[SWEET_FB_SWAP_REC_SIZE];     /* latest swap, not yet shown */
	bool          fDirty;
	bool          fX9OwnerHeld;   /* display tick holds the GPU owner lock for this present */
 bool          fX9SubmitPending, fX9ObservePending;
 unsigned      fX9RetryTicks, fX9BusyTicks;
	/* topaz direct MDP scanout (VIG0 / CTL0 from TopazGpuW disp.c) */
	volatile uint32_t *fVig0;
	volatile uint32_t *fCtl0;
	volatile uint32_t *fIntf1;      /* INTF_1 (DSI0) timing engine: refresh diagnostics */
	uint32_t      fIntfFrames;
	uint64_t      fScanPhys;      /* what VIG0 currently fetches */
	uint32_t      fScanStride;
	uint32_t      fBootSrc, fBootStride;
	bool          directPresent(const uint8_t *rec);
	void          scanFrom(uint64_t pa, uint32_t stride);
	uint32_t      fFlushTicks;    /* vsync ticks between panel copies */
	bool          fPowerOff;      /* RequestPowerChange(0) */
	uint32_t      fFadeStep;      /* fade-out steps done while off */
	/* topaz fade: cached copy of the last frame and a contiguous output frame */
	IOBufferMemoryDescriptor *fFadeOrig;
	/* kernel-drawn frames (console, fade) are double buffered like iOS's:
	 * built in the back buffer, then the MDP flips to it at vsync */
	IOBufferMemoryDescriptor *fFlip[2];
	uint64_t      fFlipPhys[2];
	uint32_t      fFlipIdx;
	bool          fConsShown;     /* console frames on the panel (no iOS frame yet) */
	void          flipFrom(const uint8_t *src, uint32_t srcStride);
	uint8_t      *backBuffer(void) { return (uint8_t *)fFlip[fFlipIdx ^ 1]->getBytesNoCopy(); }
	void          flipBack(void);
	IOBufferMemoryDescriptor *fCons;    /* console shadow frame (cached, contiguous) */
	uint64_t      fConsPhys;
	const uint8_t *fLastVA;       /* last directly scanned frame (panel window) */
	uint32_t      fLastStride;
	SweetSurface *fLastSurf;      /* keeps fLastVA alive */
	SweetSurface *fPendHold[4], *fShowHold[4], *fRetireHold[4];   /* in-use holds */
	/* X9: cached shadow of the panel, used when upper layers must be blended
	 * so the write-combined panel mapping is never read back per pixel. */
	IOBufferMemoryDescriptor *fX9Compose;
	bool          fX9ComposeValid;  /* shadow holds what the panel shows */
	void          fadeStep(const uint8_t *src, uint32_t srcStride, uint32_t step);

	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE;
	/* topaz panel brightness: DCS 0x51 over DSI0 (MakrSas/topaz-woa TopazGpuW disp.c) */
	volatile uint8_t *fDsi;
	IOBufferMemoryDescriptor *fDsiBuf;
	uint64_t      fDsiBufPhys;
	/* MT6789 (x676c) hardware DSI push */
	volatile uint32_t *fMtkDsi;
	volatile uint32_t *fMtkMutex;
	void          mtkDsiPush(void);
	IOLock       *fBlLock;
	uint32_t      fBlLevel;       /* panel DBV 1..0x7ff, kept while the display is off */
	uint32_t      fBlSent;        /* last level sent to the panel */
	thread_call_t fBlCall;        /* sends the latest level off backboardd's thread */
	static void   blWork(thread_call_param_t p0, thread_call_param_t p1);
	bool          dsiDcsLong(const uint8_t *payload, uint32_t len);
	bool          dsiSend(const uint8_t hdr[4], const uint8_t *payload, uint32_t len);
	void          setPanelOn(bool on);
	void          setPanelLevel(uint32_t level);
	bool          fX9Bl;          /* X9: DCS 0x51 through the inherited LK DSI */
	uint32_t      fX9BlSent, fX9BlBusy, fX9BlFail;
	bool          blReady(void) const { return fDsi != NULL || fX9Bl; }
	void addClient(IOMobileFramebufferUserClient *client);
	void removeClient(IOMobileFramebufferUserClient *client);
	void swap(const uint8_t *rec);
	void present(const uint8_t *rec);
};

/*
 * CoreBrightness (-[CBDisplayContaineriOS findBacklight]) matches
 * {IOPropertyMatch: {backlight-control: true}} and requires the class to
 * conform to AppleARMBacklight; without it there is no display module and
 * the brightness slider is dead. It then sets "brightness-nits" here
 * (SetBLDriverNitsValue) and IOMFBBrightnessLevel on the framebuffer.
 */
class AppleARMBacklight : public IOService {
	OSDeclareDefaultStructors(AppleARMBacklight);
public:
	AppleCLCD *fFB;
	IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleARMBacklight, IOService);

/*
 * The graphics accelerator's service class. QuartzCore (CAMetalContextCreate,
 * once per process) polls IOServiceMatching("IOAcceleratorES") 300 times,
 * 100 ms apart, before it goes on: thirty seconds at every backboardd start.
 * One without MetalPluginName/MetalPluginClassName is found and then passed
 * over by Metal (MTLRegisterDevices), which leaves the renderers as they were.
 */
#include "X9MetalPolicy.h"
extern "C" bool sweet_x9_metal_discovery_ready(void);
extern "C" bool sweet_x9_render_discovery_ready(void);
extern "C" const char *sweet_x9_render_plugin_name(void);
class IOAcceleratorES : public IOService {
	OSDeclareDefaultStructors(IOAcceleratorES);
public:
 virtual OSPtr<OSObject> copyProperty(const char *key) const APPLE_KEXT_OVERRIDE {
  if(key&&sweet_board_x9()&&(!strcmp(key,"MetalPluginName")||!strcmp(key,"MetalPluginClassName"))&&
     sweet_x9_render_discovery_ready()) {
   char name[32]={0};proc_selfname(name,sizeof(name));
   const char *plugin=sweet_x9_render_plugin_name();
   if(!plugin)return nullptr;
   const char *value=!strcmp(key,"MetalPluginName")?plugin:"X9GeneralMetalFactory";
   IOLog("X9 RENDER DISCOVERY: caller=%s property=%s value=%s native desktop Metal\n",name,key,value);
   return OSString::withCString(value);
  }

  if(key&&sweet_board_x9()&&sweet_x9_metal_discovery_ready()&&
     (!strcmp(key,"MetalPluginName")||!strcmp(key,"MetalPluginClassName"))) {
   char name[32]={0};(void)proc_selfname(name,sizeof(name));
   if(!x9_metal_render_owner(name)) {
    IOLog("X9 METAL DISCOVERY: property=%s caller=%s system-compute\n",key,name);
    return OSString::withCString(!strcmp(key,"MetalPluginName")?"X9MetalCompute168":"X9Device");
   }
  }
  return IOService::copyProperty(key);
 }
};

OSDefineMetaClassAndStructors(IOAcceleratorES, IOService);

/*
 * SweetMaliUserClient for Mali-G57 Metal accelerator.
 * Handles IOServiceOpen from Metal.framework / QuartzCore and memory mapping.
 */
class SweetMaliUserClient : public IOUserClient {
	OSDeclareDefaultStructors(SweetMaliUserClient);
public:
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE { terminate(); return kIOReturnSuccess; }
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target,
	    void *reference) APPLE_KEXT_OVERRIDE {
		return kIOReturnSuccess;
	}
	IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options,
	    IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE {
		if (memory == NULL) return kIOReturnBadArgument;
		*memory = NULL;
		if (!sweet_board_x676c()) return kIOReturnUnsupported;
		if (type == 0) {
			/* Type 0: Map ARM Mali-G57 physical MMIO registers (0x13000000, 64KB) */
			IOMemoryDescriptor *desc = IOMemoryDescriptor::withPhysicalAddress(0x13000000, 0x10000, kIODirectionInOut);
			if (desc != NULL) {
				*memory = desc;
				return kIOReturnSuccess;
			}
			return kIOReturnNoMemory;
		} else if (type == 1) {
			/* Type 1: Map shared command ringbuffer / job descriptors */
			IOBufferMemoryDescriptor *buf = IOBufferMemoryDescriptor::inTaskWithOptions(
			    kernel_task, kIODirectionInOut, 1024 * 1024, page_size);
			if (buf != NULL) {
				*memory = buf;
				return kIOReturnSuccess;
			}
			return kIOReturnNoMemory;
		}
		return kIOReturnUnsupported;
	}
};

OSDefineMetaClassAndStructors(SweetMaliUserClient, IOUserClient);

/*
 * AppleM2ScalerCSCDriver stub service and user client.
 *
 * IOSurfaceAccelerator in userland matches "AppleM2ScalerCSCDriver" to perform
 * hardware image scaling / color space conversion. Providing this service
 * satisfies IOServiceMatching("AppleM2ScalerCSCDriver"), eliminating the
 * "IOServiceMatchingfailed for: AppleM2ScalerCSCDriver" log spam and enabling
 * clean fallback / progression of the CoreAnimation display pipeline.
 */
class AppleM2ScalerCSCDriver : public IOService {
	OSDeclareDefaultStructors(AppleM2ScalerCSCDriver);
public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleM2ScalerCSCDriver, IOService);

class AppleM2ScalerCSCDriverUserClient : public IOUserClient {
	OSDeclareDefaultStructors(AppleM2ScalerCSCDriverUserClient);
public:
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE { terminate(); return kIOReturnSuccess; }
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target,
	    void *reference) APPLE_KEXT_OVERRIDE {
		/*
		 * No scaler is implemented. Reporting success made every
		 * IOSurfaceAccelerator transfer "complete" with the destination
		 * untouched (blank or stale surfaces). Refuse instead so callers take
		 * their failure/fallback path. sweet_scaler_stub=1 restores the old
		 * always-success behaviour.
		 */
		static int stub = -1;
		if (stub < 0) {
			int arg = 0;
			PE_parse_boot_argn("sweet_scaler_stub", &arg, sizeof(arg));
			stub = arg != 0 || !sweet_board_x9();  /* other boards keep the stub */
		}
		static unsigned logged;
		if (sweet_board_x9() && logged++ < 16) {
			IOLog("SWEET SCALER: sel %u in %u/%u out %u/%u -> %s\n", selector,
			    args->scalarInputCount, args->structureInputSize,
			    args->scalarOutputCount, args->structureOutputSize,
			    stub ? "success (stub)" : "unsupported");
		}
		return stub ? kIOReturnSuccess : kIOReturnUnsupported;
	}
};

OSDefineMetaClassAndStructors(AppleM2ScalerCSCDriverUserClient, IOUserClient);

bool
AppleM2ScalerCSCDriver::start(IOService *provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "AppleM2ScalerCSCDriverUserClient");
	registerService();
	IOLog("SWEET SCALER: AppleM2ScalerCSCDriver registered\n");
	return true;
}
OSDefineMetaClassAndStructors(AppleCLCD, IOMobileFramebuffer);

class IOMobileFramebufferUserClient : public IOUserClient {
	OSDeclareDefaultStructors(IOMobileFramebufferUserClient);

public:
	AppleCLCD          *fFB;
	task_t              fTask;
	mach_port_t         fPort;
	io_user_reference_t fFunc[SWEET_FB_NOTIFY_TYPES];
	io_user_reference_t fRefcon[SWEET_FB_NOTIFY_TYPES];

	bool initWithTask(task_t owningTask, void *securityID, UInt32 type) APPLE_KEXT_OVERRIDE;
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn registerNotificationPort(mach_port_t port, UInt32 type,
	    io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target,
	    void *reference) APPLE_KEXT_OVERRIDE;
	void notify(uint32_t type, const uint64_t *values, uint32_t count);
};

OSDefineMetaClassAndStructors(IOMobileFramebufferUserClient, IOUserClient);

bool
AppleCLCD::start(IOService *provider)
{
	PE_Video video;

	if (!IOService::start(provider)) {
		return false;
	}
	if (sweet_board_qcom()) {
		topaz_modem_init();
	}
	sweet_busy_init();
	PE_current_console(&video);
	fPhys = video.v_baseAddr;
	fWidth = (uint32_t)video.v_width;
	fHeight = (uint32_t)video.v_height;
	fRowBytes = (uint32_t)video.v_rowBytes;
	if (sweet_board_x9() && (video.v_baseAddr != 0xfc910000ULL ||
	    video.v_width != 1256 || video.v_height != 2760 || video.v_rowBytes != 5120)) {
		IOLog("SWEET FB: invalid X9 boot framebuffer\n");
		return false;
	}
	if (!sweet_fb_layout_valid(video.v_baseAddr, UINT64_MAX, video.v_width,
	    video.v_height, video.v_rowBytes)) {
		IOLog("SWEET FB: no valid boot framebuffer\n");
		return false;
	}
	int native = 0;
	PE_parse_boot_argn("sweetfb_native", &native, sizeof(native));
	fLogicalW = fWidth;
	fLogicalH = fHeight;
	if (!native && fWidth <= SWEET_FB_LOGICAL_W && fHeight <= SWEET_FB_LOGICAL_H) {
		fLogicalW = SWEET_FB_LOGICAL_W;
		fLogicalH = SWEET_FB_LOGICAL_H;
	}
	/* x rounded down to 4 pixels: the panel window must start 16-byte aligned (MDP fetch) */
	fOffX = ((int32_t)(fLogicalW - fWidth) / 2) & ~3;
	fOffY = (int32_t)(fLogicalH - fHeight) / 2;
	fLock = IOLockAlloc();
	fClients = OSArray::withCapacity(4);
	fSurface = sweet_surface_create_physical(fPhys, fWidth, fHeight, fRowBytes, SWEET_FB_FORMAT_BGRA);
	fPixels = fSurface != NULL ? fSurface->kernelAddress() : 0;
	int x9_render = 0;
	if (sweet_board_x9() && PE_parse_boot_argn("x9_metal_render", &x9_render, sizeof(x9_render)) &&
	    x9_render == 1 && fSurface != NULL && fPixels != 0) {
		SweetSurface *target = sweet_surface_create_buffer(fWidth, fHeight, fRowBytes, SWEET_FB_FORMAT_BGRA);
		uintptr_t targetPixels = target != NULL ? target->kernelAddress() : 0;
		if (targetPixels != 0) {
			memcpy((void *)targetPixels, (const void *)fPixels, (size_t)fRowBytes * fHeight);
			/* The published physical surface remains retained by the ID table,
			 * keeping the panel CPU mapping alive; fPixels remains scanout. */
			fSurface->release(); fSurface = target;
			IOLog("X9 RENDER DISPLAY: RAM default surface %u; panel copied through existing LK path, no GPU panel mapping\n", fSurface->id);
		} else {
			OSSafeReleaseNULL(target);
			IOLog("X9 RENDER DISPLAY: RAM surface allocation failed; framebuffer setup refused\n");
			fPixels = 0;
		}
	}

	fWorkLoop = IOWorkLoop::workLoop();
	if (fLock == NULL || fClients == NULL || fPixels == 0 || fWorkLoop == NULL) {
		IOLog("SWEET FB: setup failed\n");
		OSSafeReleaseNULL(fSurface);    /* stays published: also held by the table */
		OSSafeReleaseNULL(fWorkLoop);
		return false;
	}
	fVsync = IOTimerEventSource::timerEventSource(this, &AppleCLCD::vsyncFired);
	if (fVsync == NULL || fWorkLoop->addEventSource(fVsync) != kIOReturnSuccess) {
		IOLog("SWEET FB: no vsync timer\n");
		return false;
	}
	if (sweet_board_x9()) {
		fX9SubmitPending=fX9ObservePending=false;
		fX9PresentTimer=IOTimerEventSource::timerEventSource(this,&AppleCLCD::x9PresentFired);
		if(!fX9PresentTimer || fWorkLoop->addEventSource(fX9PresentTimer)!=kIOReturnSuccess) {
			IOLog("X9 DISPLAY: no asynchronous TE timer;setup refused\n");return false;
		}
	}
	/*
	 * Qualcomm MDP/DSI0 (SM6225). x676c: MediaTek DSI0 and DISP_MUTEX0.
	 */
	if (sweet_board_x676c()) {
		fMtkDsi = (volatile uint32_t *)ml_io_map(0x14013000, 0x1000);
		fMtkMutex = (volatile uint32_t *)ml_io_map(0x14001000, 0x1000);
		sMtkDsi = fMtkDsi;
		sMtkMutex = fMtkMutex;
		IOLog("SWEET FB: MT6789 DSI0 (0x14013000) and MUTEX0 (0x14001000) mapped for HW scanout\n");
	} else if (sweet_board_qcom()) {
		fVig0 = (volatile uint32_t *)ml_io_map(0x05e05000, 0x1000);
		fCtl0 = (volatile uint32_t *)ml_io_map(0x05e02000, 0x1000);
		fDsi = (volatile uint8_t *)ml_io_map(0x05e94000, 0x1000);
	}
	fDsiBuf = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
	    kIODirectionInOut | kIOMemoryPhysicallyContiguous, page_size, 0x00000000fffff000ull);
	fDsiBufPhys = fDsiBuf ? fDsiBuf->getPhysicalSegment(0, NULL, kIOMemoryMapperNone) : 0;
	fBlLock = IOLockAlloc();
	fBlLevel = 0x133;                       /* panel init level (stock DT) */
	fBlSent = ~0u;
	fBlCall = thread_call_allocate(&AppleCLCD::blWork, this);
	fX9Bl = sweet_board_x9() && x9_lk_gui_busy() >= 0;
	fX9BlSent = fX9BlBusy = fX9BlFail = 0;
	if (fX9Bl) {
		fBlLevel = X9_BL_DBV_DEFAULT;
		IOLog("X9 BL: DCS 0x51 via inherited LK DSI0 (0x3e730000), LP long write, DBV %u..%u\n",
		    X9_BL_DBV_MIN, X9_BL_DBV_MAX);
	} else if (sweet_board_x9()) {
		IOLog("X9 BL: LK DSI path not active; no backlight service\n");
	}
	if (blReady()) {
		if (fDsi != NULL) {
			IOLog("SWEET FB: DSI0 ctrl %#x, brightness via DCS 0x51\n", *(volatile uint32_t *)(fDsi + 0x004));
		}
		/*
		 * Off unless sweetbl=1: with a backlight service CoreBrightness also
		 * runs digital dimming, a colour filter over every frame, which the
		 * CPU renderer cannot afford (the UI dropped to under 1 fps).
		 * X9 too: rev250 (23.txt) published it by default and got a purple,
		 * near-frozen UI (swap_id 13 at 100 s vs ~1000, extra full-screen
		 * offscreen surfaces, AegirPoster/GradientPoster abort loops) while
		 * CoreBrightness never sent a single level. X9 keeps the DCS 0x51
		 * transport and sends one default DBV from the heartbeat instead.
		 */
		uint32_t blOn = 0;
		(void)PE_parse_boot_argn("sweetbl", &blOn, sizeof(blOn));
		AppleARMBacklight *bl = blOn ? OSTypeAlloc(AppleARMBacklight) : NULL;
		if (fDsi != NULL || bl != NULL) {
			setProperty("IOMFBBrightnessLevel", (unsigned long long)(200u << 16), 32);
		}
		if (bl != NULL && bl->init()) {
			bl->fFB = this;
			bl->setProperty("backlight-control", true);
			bl->setProperty("brightness-nits", (unsigned long long)200, 32);
			bl->setProperty("max-nits", (unsigned long long)SWEET_FB_MAX_NITS, 32);
			if (bl->attach(this)) {
				bl->registerService();
				IOLog("SWEET FB: AppleARMBacklight published\n");
			}
		}
		OSSafeReleaseNULL(bl);
	}
	uint32_t accelOn = 1;
	(void)PE_parse_boot_argn("sweetaccel", &accelOn, sizeof(accelOn));
	/* hackios: a passive accelerator class ends CoreAnimation's wait; X9
	 * gets no driver properties or MT6789 power/MMIO initialization. */
	IOAcceleratorES *accel = accelOn ? OSTypeAlloc(IOAcceleratorES) : NULL;
	if (accel != NULL && accel->init()) {
		accel->setName("sgx");
		if (accel->attach(this)) {
			if (sweet_board_qcom() || sweet_board_x9()) {
				accel->registerService();
				IOLog("SWEET FB: empty IOAcceleratorES published (no 30 s wait in CAMetalContextCreate)\n");
			}
#if defined(ARM64_BOARD_CONFIG_SWEET)
			if (sweet_board_x676c()) {
				IOLog("SWEET GPU: powering on MT6789 Mali-G57 subsystem...\n");

				/* 0. Power on Mali GPU PMIC rails (VGPU + VSRAM_GPU) via MT6789 PWRAP at 0x10026000 */
				volatile uint32_t *pwrap = (volatile uint32_t *)ml_io_map(0x10026000, 0x1000);
				if (pwrap != NULL) {
					auto pwrap_wait_idle = [](volatile uint32_t *p) -> bool {
						for (int retry = 0; retry < 10000; retry++) {
							uint32_t rdata = p[0x0ca8 / 4];
							uint32_t fsm = (rdata >> 16) & 0x7;
							if (fsm == 0) {
								return true;
							}
							if (fsm == 6) {
								p[0x0ca4 / 4] = 1;
							}
							IODelay(1);
						}
						return false;
					};
					auto pwrap_write = [pwrap_wait_idle](volatile uint32_t *p, uint32_t reg, uint32_t val) -> bool {
						if (!pwrap_wait_idle(p)) return false;
						p[0x0c80 / 4] = (1u << 31) | (((reg >> 1) & 0x7fff) << 16) | (val & 0xffff);
						return pwrap_wait_idle(p);
					};
					auto pwrap_read = [pwrap_wait_idle](volatile uint32_t *p, uint32_t reg) -> uint32_t {
						if (!pwrap_wait_idle(p)) return ~0u;
						p[0x0c80 / 4] = (((reg >> 1) & 0x7fff) << 16);
						for (int retry = 0; retry < 10000; retry++) {
							uint32_t rdata = p[0x0ca8 / 4];
							if (((rdata >> 16) & 0x7) == 6) {
								uint32_t val = rdata & 0xffff;
								p[0x0ca4 / 4] = 1;
								return val;
							}
							IODelay(1);
						}
						return ~0u;
					};

					uint32_t vgpu_init = pwrap_read(pwrap, 0x1508);
					uint32_t vsram_init = pwrap_read(pwrap, 0x1bc8);
					IOLog("SWEET GPU: PWRAP initial PMIC rails: VGPU_CON0=0x%04x VSRAM_CON0=0x%04x\n",
					    vgpu_init, vsram_init);

					/* Set VGPU target voltage to 0.800V (ELR0 = 0x30) */
					pwrap_write(pwrap, 0x1514, 0x0030);
					/* Enable VGPU Buck regulator (bit 0 = 1) */
					pwrap_write(pwrap, 0x1508, 0x0001);
					/* Enable VSRAM_GPU LDO regulator (bit 0 = 1) */
					pwrap_write(pwrap, 0x1bc8, 0x0001);
					IODelay(500); /* 500us rail ramp-up time */

					uint32_t vgpu_post = pwrap_read(pwrap, 0x1508);
					uint32_t vsram_post = pwrap_read(pwrap, 0x1bc8);
					IOLog("SWEET GPU: PWRAP post-enable PMIC rails: VGPU_CON0=0x%04x VSRAM_CON0=0x%04x\n",
					    vgpu_post, vsram_post);
				}

				/*
				 * MT6789 Mali-G57 power-on, reverse-engineered from the stock
				 * mtk_gpufreq_mt6789.ko (GPUEB off => AP-direct MTCMOS path):
				 * __gpufreq_power_control + __gpufreq_mfg[0-3]_control +
				 * __gpufreq_external_cg_control. Bases: SPM 0x10006000,
				 * infracfg_ao 0x10001000, MFG_TOP 0x13fbf000, topckgen 0x10000000.
				 * The power-domain PWR_CON registers live in the SPM bank (NOT in
				 * MFG_TOP as the earlier bring-up wrongly assumed); status is read
				 * from SPM PWR_STATUS (0x16c) and PWR_STATUS_2ND (0x170).
				 */
				volatile uint32_t *infra_ao = (volatile uint32_t *)ml_io_map(0x10001000, 0x1000);
				volatile uint32_t *topck = (volatile uint32_t *)ml_io_map(0x10000000, 0x1000);
				volatile uint32_t *spm = (volatile uint32_t *)ml_io_map(0x10006000, 0x1000);
				volatile uint32_t *mfg_top = (volatile uint32_t *)ml_io_map(0x13fbf000, 0x1000);

				/* 1. Enable the MFG reference-clock mux in topckgen (CLK_CFG_3). */
				if (topck != NULL) {
					topck[0x0058 / 4] = (1u << 18) | (1u << 7); /* CLK_CFG_3_CLR: ungate mfg mux */
					topck[0x0004 / 4] = (1u << 3);              /* CLK_CFG_UPDATE: mfg */
					IODelay(20);
				}

				/*
				 * 2. MTCMOS power-domain bring-up in the SPM bank. PWR_CON bits:
				 *    0=RST_B 1=ISO 2=PWR_ON 3=PWR_ON_2ND 4=CLK_DIS
				 *    8=SRAM_PDN 12=SRAM_PDN_ACK. Per-domain status bit in
				 *    PWR_STATUS(0x16c)+PWR_STATUS_2ND(0x170): MFG0=0x4 MFG1=0x8
				 *    MFG2=0x10 MFG3=0x20. CON offsets 0x308/0x30c/0x310/0x314.
				 */
				if (spm != NULL) {
					volatile uint32_t *sta  = &spm[0x016c / 4];
					volatile uint32_t *sta2 = &spm[0x0170 / 4];
					auto mfg_power_on = [&](uint32_t con_off, uint32_t sta_bit) {
						volatile uint32_t *con = &spm[con_off / 4];
						*con |= (1u << 2);                  /* PWR_ON */
						*con |= (1u << 3);                  /* PWR_ON_2ND */
						for (int i = 0; i < 2000; i++) {
							if ((*sta & sta_bit) && (*sta2 & sta_bit)) break;
							IODelay(10);
						}
						IODelay(100);                       /* power settle */
						*con &= ~(1u << 4);                 /* clear CLK_DIS */
						*con &= ~(1u << 1);                 /* clear ISO */
						*con |= (1u << 0);                  /* RST_B */
						*con &= ~(1u << 8);                 /* clear SRAM_PDN */
						for (int i = 0; i < 2000; i++) {
							if ((*con & (1u << 12)) == 0) break; /* SRAM_PDN_ACK clear */
							IODelay(10);
						}
					};

					IOLog("SWEET GPU: SPM PWR_STATUS pre=0x%08x/0x%08x MFG0_CON=0x%08x\n",
					    *sta, *sta2, spm[0x0308 / 4]);

					mfg_power_on(0x0308, 0x4);   /* MFG0 (async/bus) */
					mfg_power_on(0x030c, 0x8);   /* MFG1 (core) */

					/* MFG1 up -> release MFG bus protection in infracfg_ao. */
					if (infra_ao != NULL) {
						infra_ao[0x0718 / 4] = 0x80;
						infra_ao[0x02a4 / 4] = 0x600000;
						infra_ao[0x0718 / 4] = 0x60;
						infra_ao[0x02ac / 4] = 0x200000;
						IODelay(20);
					}

					mfg_power_on(0x0310, 0x10);  /* MFG2 (shader stack 0) */
					mfg_power_on(0x0314, 0x20);  /* MFG3 (shader stack 1) */

					IOLog("SWEET GPU: SPM PWR_STATUS post=0x%08x/0x%08x MFG_CON[0..3]=0x%08x 0x%08x 0x%08x 0x%08x\n",
					    *sta, *sta2, spm[0x0308 / 4], spm[0x030c / 4], spm[0x0310 / 4], spm[0x0314 / 4]);
				}

				/*
				 * 3. MFG_TOP external clock-gate control (__gpufreq_external_cg_control).
				 * Runs only after the MFG domains are powered (it writes the MFG_TOP
				 * bank which is itself inside the MFG power domain).
				 */
				if (mfg_top != NULL) {
					/*
					 * bg3d subsys clock gate (mfgcfg lives in syscon@0x13fbf000,
					 * confirmed from device-tree). mtk SETCLR gate reversed from
					 * clk-mt6789-mfgcfg.ko: {sta 0x0, clr 0x8, set 0x4}, shift 0.
					 * Enable = write bit0 to CLR; gated when sta bit0 == 1.
					 */
					mfg_top[0x0008 / 4] = (1u << 0);
					for (int i = 0; i < 1000; i++) {
						if ((mfg_top[0x0000 / 4] & (1u << 0)) == 0) break;
						IODelay(10);
					}

					mfg_top[0x0020 / 4] |= 0x03c00000;
					mfg_top[0x00b0 / 4] &= ~0x0600;
					mfg_top[0x00b4 / 4] |= 0x0010;
					mfg_top[0x00b0 / 4] = (mfg_top[0x00b0 / 4] & ~0x00080100u) | 0x80000;
					mfg_top[0x0024 / 4] |= 1;
					mfg_top[0x0f60 / 4] |= 1;
					IODelay(20);

					/* topckgen MFG clock-stable poll (bit 0x40000 @ CLK_CFG_3 0x50). */
					if (topck != NULL) {
						uint32_t ck = 0;
						for (int i = 0; i < 1000; i++) {
							ck = topck[0x0050 / 4];
							if (ck & 0x40000) break;
							IODelay(10);
						}
						IOLog("SWEET GPU: topckgen CLK_CFG_3=0x%08x (mfg stable=%d)\n",
						    ck, (ck & 0x40000) ? 1 : 0);
					}
				}

				/* 4. Read Mali hardware registers */
				volatile uint32_t *mali = (volatile uint32_t *)ml_io_map(0x13000000, 0x4000);
				if (mali != NULL) {
					uint32_t gpu_id = mali[0x0000 / 4];
					uint32_t l2_feat = mali[0x0004 / 4];
					uint32_t tiler_feat = mali[0x000c / 4];
					uint32_t mem_feat = mali[0x0010 / 4];
					uint32_t mmu_feat = mali[0x0014 / 4];
					uint32_t as_present = mali[0x0018 / 4];  /* kbase AS_PRESENT = 0x18 */
					uint32_t js_present = mali[0x001c / 4];  /* kbase JS_PRESENT = 0x1c */
					IOLog("SWEET GPU: Mali-G57 ID=0x%08x (Arch %u Rev %u) L2=0x%08x Tiler=0x%08x Mem=0x%08x MMU=0x%08x AS=0x%x JS=0x%x\n",
					    gpu_id, (gpu_id >> 16) & 0xff, (gpu_id >> 12) & 0xf,
					    l2_feat, tiler_feat, mem_feat, mmu_feat, as_present, js_present);

					bool hw_ok = (gpu_id != 0 && gpu_id != ~0u);
					if (hw_ok) {
						accel->setProperty("MetalPluginName", "MaliMetal");
						accel->setProperty("MetalPluginClassName", "MaliMetalDevice");
						accel->setProperty("IOGPUResourcePolicy", 1ULL, 32);
						accel->setProperty("vendor-id", 0x13b5ULL, 32);
						accel->setProperty("device-id", (unsigned long long)gpu_id, 32);
						accel->setProperty("gpu-core-count", 2ULL, 32);
						accel->setProperty(kIOUserClientClassKey, "SweetMaliUserClient");
						accel->registerService();
						IOLog("SWEET GPU: Mali-G57 registered as MaliMetal / MaliMetalDevice (vendor 0x13b5, device 0x%04x)\n",
						    gpu_id >> 16);
					} else {
						/* RADICAL HONESTY: never spoof device-id 0x9093 when hardware bus returns 0 */
						accel->setProperty("MetalPluginName", "MaliMetal");
						accel->setProperty("MetalPluginClassName", "MaliMetalDevice");
						accel->setProperty("IOGPUResourcePolicy", 1ULL, 32);
						accel->setProperty("vendor-id", 0x13b5ULL, 32);
						accel->setProperty("device-id", 0ULL, 32);
						accel->setProperty("gpu-hardware-status", "offline-stub");
						accel->setProperty(kIOUserClientClassKey, "SweetMaliUserClient");
						accel->registerService();
						IOLog("SWEET GPU: Mali hardware did not respond (ID=0x%08x). Registered software-stub MaliMetal (device-id 0, offline-stub)\n",
						    gpu_id);
					}
				}
			}
#endif
		}
	}
	OSSafeReleaseNULL(accel);
	/* dpu sm6115 catalog: intf_1 at mdp+0x6a800, mdp at 0x5e01000 */
	fIntf1 = sweet_board_qcom() ? (volatile uint32_t *)ml_io_map(0x05e6b000, 0x1000) : NULL;
	if (fIntf1 != NULL) {
		volatile uint32_t *r = fIntf1 + 0x800 / 4;
		r[0x0a8 / 4] = 0x3;     /* FRAME_LINE_COUNT_EN */
		IOLog("SWEET INTF1: te_en %#x config %#x hsync %#x vsync_period %#x hctl %#x active_hctl %#x vstart %#x vend %#x fmt %#x cmd_trig %#x\n",
		    r[0x000 / 4], r[0x004 / 4], r[0x008 / 4], r[0x00c / 4], r[0x03c / 4], r[0x040 / 4],
		    r[0x01c / 4], r[0x024 / 4], r[0x090 / 4], r[0x084 / 4]);
	}
	if (fVig0 != NULL && fCtl0 != NULL) {
		fBootSrc = fVig0[0x14 / 4];
		fBootStride = fVig0[0x24 / 4];
		fScanPhys = fBootSrc;
		fScanStride = fBootStride;
		IOLog("SWEET FB: MDP VIG0 src %#x stride %u (boot), direct scanout ready\n",
		    fBootSrc, fBootStride);
		sweet_scanout_set_geometry(fWidth, fHeight, fLogicalW, fLogicalH);
		vm_size_t fsz = (vm_size_t)fRowBytes * fHeight;
		for (int i = 0; i < 2; i++) {
			fFlip[i] = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
			    kIODirectionInOut | kIOMemoryPhysicallyContiguous, fsz, 0x00000000fffff000ull);
			fFlipPhys[i] = fFlip[i] ? fFlip[i]->getPhysicalSegment(0, NULL, kIOMemoryMapperNone) : 0;
		}
		fFadeOrig = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut, fsz, page_size);
		fCons = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
		    kIODirectionInOut | kIOMemoryPhysicallyContiguous, fsz, 0x00000000fffff000ull);
		if (fFlip[0] == NULL || fFlip[1] == NULL || fFadeOrig == NULL) {
			OSSafeReleaseNULL(fFlip[0]);
			OSSafeReleaseNULL(fFlip[1]);
			OSSafeReleaseNULL(fFadeOrig);
			OSSafeReleaseNULL(fCons);
		}
		if (fCons != NULL) {
			uint8_t *c = (uint8_t *)fCons->getBytesNoCopy();
			memcpy(c, (const void *)fPixels, fsz);  /* keep what the console drew */
			sweet_dcache_clean((vm_offset_t)(uintptr_t)c, (unsigned)fsz);   /* for panics */
			fConsPhys = fCons->getPhysicalSegment(0, NULL, kIOMemoryMapperNone);
			sweet_cons_fb = this;
			sweet_console_show_hook = sweet_cons_show;
			sweet_console_retarget((vm_offset_t)c);
			fConsShown = true;
			flipFrom(c, fRowBytes);
			IOLog("SWEET FB: console double buffered (shadow %#llx)\n", fConsPhys);
		} else {
			OSSafeReleaseNULL(fCons);
		}
	}
	uint32_t hz = sweet_board_x9() ? 60 : 10;
	PE_parse_boot_argn("sweetfb_hz", &hz, sizeof(hz));
	if (sweet_board_x9()) {
		// This paces notifications and LK submissions, not panel PHY timings.
		// Keep the default >=60; 120 must be requested explicitly by boot arg.
		fTickHz = hz == 120 ? 120 : 60;
		fFlushTicks = 1;
		fCadenceTicks=fCadenceCopies=fCadenceSubmits=fCadenceMissed=0;
		fCadenceGpuBusy=fCadenceDisplayBusy=fCadenceCopyNs=fCadenceMaxCopyNs=fCadenceMaxLateNs=0;
		fCadenceTEFrames=fCadenceTETimeouts=fCadenceTEWaitNs=fCadenceTEGapNs=0;
		nanoseconds_to_absolutetime((1000000000ULL + fTickHz/2) / fTickHz, &fTickPeriod);
		fCadenceStart = mach_absolute_time();
		fTickDeadline = fCadenceStart + fTickPeriod;
		fVsync->wakeAtTime(fTickDeadline);
		IOLog("X9 DISPLAY cadence: target=%u Hz absolute deadlines;panel mode inherited from LK,not reprogrammed\n", fTickHz);
	} else {
		fFlushTicks = (hz == 0 || hz >= 60) ? 1 : 60 / hz;
		fVsync->setTimeoutUS(SWEET_FB_VSYNC_US);
	}

	setProperty(kIOUserClientClassKey, "IOMobileFramebufferUserClient");
	setProperty("external", false);
	setProperty("DisplayPipePlaneBaseAlignment", 64ULL, 32);
	registerService();
	IOLog("SWEET FB: AppleCLCD %ux%u stride %u at 0x%llx, default surface %u, presented as %ux%u\n",
	    fWidth, fHeight, fRowBytes, fPhys, fSurface->id, fLogicalW, fLogicalH);
	return true;
}

void
AppleCLCD::scheduleVsync(IOTimerEventSource *sender)
{
	if (!sweet_board_x9()) { sender->setTimeoutUS(SWEET_FB_VSYNC_US); return; }
	uint64_t now = mach_absolute_time(), elapsedNs = 0;
	fTickDeadline += fTickPeriod;
	if (now >= fTickDeadline) {
		uint64_t missed = (now - fTickDeadline) / fTickPeriod + 1;
		fCadenceMissed += missed;
		fTickDeadline += missed * fTickPeriod;
	}
	absolutetime_to_nanoseconds(now - fCadenceStart, &elapsedNs);
	if (elapsedNs >= 2000000000ULL) {
		IOLog("X9 DISPLAY cadence: target=%u elapsed_ms=%llu ticks=%llu copies=%llu submits=%llu missed=%llu gpu_defer=%llu lk_defer=%llu copy_avg_us=%llu copy_max_us=%llu late_max_us=%llu;software counters,not panel fps\n",
		 fTickHz,elapsedNs/1000000,fCadenceTicks,fCadenceCopies,fCadenceSubmits,fCadenceMissed,
		 fCadenceGpuBusy,fCadenceDisplayBusy,fCadenceCopies?fCadenceCopyNs/fCadenceCopies/1000:0,
		 fCadenceMaxCopyNs/1000,fCadenceMaxLateNs/1000);
		IOLog("X9 DISPLAY TE: frames=%llu timeouts=%llu wait_avg_us=%llu poll_gap_max_us=%llu;fresh TE observed,not scanout completion\n",
		 fCadenceTEFrames,fCadenceTETimeouts,fCadenceTEFrames?fCadenceTEWaitNs/fCadenceTEFrames/1000:0,fCadenceTEGapNs/1000);
		fCadenceTEFrames=fCadenceTETimeouts=fCadenceTEWaitNs=fCadenceTEGapNs=0;
		fCadenceStart=now; fCadenceTicks=fCadenceCopies=fCadenceSubmits=fCadenceMissed=0;
		fCadenceGpuBusy=fCadenceDisplayBusy=fCadenceCopyNs=fCadenceMaxCopyNs=fCadenceMaxLateNs=0;
	}
	// Skip missed periods rather than burst; processing time does not accumulate
	// into every next period as with setTimeoutUS at the end of a frame.
	sender->wakeAtTime(fTickDeadline);
}

// Same serialized workloop as vsync, but not quantized to the16.7ms
// notification tick. No GPU owner is held while waiting for panel TE.
void AppleCLCD::x9QueuePresent(void) {
	if(fX9SubmitPending)return;
	fX9PresentBegan=fX9LastPoll=mach_absolute_time();
	int result=x9_lk_gui_present();
	fX9SubmitPending=result==0;
	if(result==0)fX9PresentTimer->setTimeoutUS(100);
	else if(result==1){fCadenceSubmits++;fX9ObservePending=true;fX9BusyTicks=0;}
	else IOLog("X9 DISPLAY TE: submit unavailable=%d;no blind START\n",result);
}
void AppleCLCD::x9PresentFired(OSObject *owner,IOTimerEventSource *sender) {
	auto *fb=(AppleCLCD *)owner;
	if(!fb->fX9SubmitPending)return;
	uint64_t now=mach_absolute_time(),elapsed=0,gap=0;
	absolutetime_to_nanoseconds(now-fb->fX9PresentBegan,&elapsed);
	absolutetime_to_nanoseconds(now-fb->fX9LastPoll,&gap);fb->fX9LastPoll=now;
	if(gap>fb->fCadenceTEGapNs)fb->fCadenceTEGapNs=gap;
	IOLockLock(fb->fLock);bool off=fb->fPowerOff;IOLockUnlock(fb->fLock);
	if(off || elapsed>=50000000ULL) {
		if(!x9_lk_gui_cancel()){sender->setTimeoutUS(100);return;}
		fb->fX9SubmitPending=false;
		if(!off){fb->fCadenceTETimeouts++;IOLog("X9 DISPLAY TE: no fresh TE within50ms;frame not started,no unsynchronized fallback\n");x9_lk_gui_probe();}
		return;
	}
	int result=x9_lk_gui_present();
	if(result==0){sender->setTimeoutUS(100);return;}
	fb->fX9SubmitPending=false;
	if(result==1) {
		fb->fCadenceTEFrames++;fb->fCadenceTEWaitNs+=elapsed;fb->fCadenceSubmits++;
		fb->fX9ObservePending=true;fb->fX9BusyTicks=0;
	} else {x9_lk_gui_cancel();IOLog("X9 DISPLAY TE: unavailable=%d;frame not started\n",result);}
}

/* Boot token x9lo (see x9_lk_scan_overlays in pe_serial.c): LK leaves its
 * orange-state layer enabled, a 1256x2760 buffer at a 5024-byte pitch that
 * x9_lk_layer() rejects because it demands the 5120 boot stride. Nothing ever
 * refreshes that layer, so LK's banner keeps blending over every frame we
 * push. Zeroing its memory makes it contribute transparent black; no display
 * register is touched. IOMemoryDescriptor::withPhysicalAddress is the route
 * the boot scanout already uses; a frame-sized ml_io_map_wcomb() from pexpert
 * would take io_map()'s permanent 8 MiB io_submap and panic (rev247 lo:
 * kmem_alloc(14139392) failed with KERN_NO_SPACE 10 s into boot).
 * rev248 lo proved the target (the banner vanished) but also logged
 * "creating writable mapping from IOMemoryDescriptor(kIODirectionOut)": the
 * direction is kIODirectionInOut now, like sweet_surface_create_physical().
 * x9lo=2 reads and reports the same statistics without writing, so one binary
 * can answer whether the clear itself is what changes the run. */
static void
sweet_x9_report_lk_overlay(const char *stage, unsigned engine, uint64_t address,
    const volatile uint32_t *pixels, uint32_t pitch, uint32_t height)
{
	const uint32_t row_words = pitch / 4;
	uint32_t hash = 0;
	unsigned sampled = 0, ink = 0, opaque_black = 0, transparent = 0, other_alpha = 0;
	unsigned min_x = 0xffffffffu, max_x = 0, min_y = 0xffffffffu, max_y = 0;
	const uint64_t started = mach_absolute_time();
	for (unsigned y = 0; y < 256; y++) {
		const uint32_t row = (uint32_t)((uint64_t) y * height / 256);
		for (unsigned x = 0; x < 256; x++) {
			const uint32_t col = (uint32_t)((uint64_t) x * row_words / 256);
			const uint32_t word = pixels[(uint64_t) row * row_words + col];
			hash = (hash << 5) | (hash >> 27);
			hash ^= word;
			sampled++;
			const unsigned alpha = word >> 24;
			if (alpha == 0) {
				transparent++;
			} else if (word == 0xff000000u) {
				opaque_black++;
			} else if (word & 0xffffffu) {
				ink++;
				if (col < min_x) min_x = col;
				if (col > max_x) max_x = col;
				if (row < min_y) min_y = row;
				if (row > max_y) max_y = row;
			} else {
				other_alpha++;
			}
		}
	}
	const uint64_t read_us = (mach_absolute_time() - started) / 1000;
	IOLog("X9 LK overlay %s: i=%u pa=%llx %ux%u grid=%u ink=%u opaque_black=%u "
	    "transparent=%u other_alpha=%u bbox=%d..%d,%d..%d checksum=%08x in %llu us\n",
	    stage, engine, address, row_words, height, sampled, ink, opaque_black,
	    transparent, other_alpha, ink ? (int) min_x : -1, ink ? (int) max_x : -1,
	    ink ? (int) min_y : -1, ink ? (int) max_y : -1, hash, read_us);
}

static void
sweet_x9_clear_lk_overlays(void)
{
	static bool done;
	if (done) {
		return;
	}
	done = true;
	const int level = x9_lk_overlay_level();
	unsigned queued = 0;
	for (unsigned index = 0; ; index++) {
		uint64_t address = 0, bytes = 0;
		unsigned engine = 0;
		uint32_t pitch = 0, height = 0;
		if (!x9_lk_overlay(index, &address, &bytes, &engine, &pitch, &height)) {
			break;
		}
		queued++;
		IOMemoryDescriptor *memory = IOMemoryDescriptor::withPhysicalAddress(
		    address, bytes, kIODirectionInOut);
		if (memory == NULL) {
			IOLog("X9 LK overlay: i=%u pa=%llx no descriptor\n", engine, address);
			continue;
		}
		IOMemoryMap *map = NULL;
		volatile uint32_t *pixels = NULL;
		if (memory->prepare(kIODirectionInOut) == kIOReturnSuccess) {
			map = memory->map(kIOMapWriteCombineCache);
			if (map != NULL) {
				pixels = (volatile uint32_t *)(uintptr_t)map->getVirtualAddress();
			}
		}
		if (pixels == NULL) {
			IOLog("X9 LK overlay: i=%u pa=%llx not mapped; banner left alone\n", engine, address);
		} else if (level != 1) {
			sweet_x9_report_lk_overlay("observe", engine, address, pixels, pitch, height);
			IOLog("X9 LK overlay: i=%u pa=%llx nothing written (x9lo=%d)\n", engine, address, level);
		} else {
			sweet_x9_report_lk_overlay("before", engine, address, pixels, pitch, height);
			const uint64_t words = bytes / 4;
			const uint64_t started = mach_absolute_time();
			for (uint64_t k = 0; k < words; k++) {
				pixels[k] = 0;
				if (!(k & 0x3ffffu)) {
					sweet_watchdog_pet();
				}
			}
			__asm__ volatile("dsb sy" ::: "memory");
			const uint64_t clear_us = (mach_absolute_time() - started) / 1000;
			sweet_x9_report_lk_overlay("after", engine, address, pixels, pitch, height);
			IOLog("X9 LK overlay: i=%u pa=%llx %llu B zeroed in %llu us\n",
			    engine, address, (unsigned long long) bytes, (unsigned long long) clear_us);
		}
		if (map != NULL) {
			map->release();
		}
		memory->complete(kIODirectionInOut);
		memory->release();
	}
	IOLog("X9 LK overlay: %u rejected layer buffer(s) queued (x9lo=%d)\n", queued, level);
}

void
AppleCLCD::vsyncFired(OSObject *owner, IOTimerEventSource *sender)
{
	AppleCLCD *fb = (AppleCLCD *)owner;
	uint64_t now = mach_absolute_time();
	if (sweet_board_x9()) {
		fb->fCadenceTicks++;
		uint64_t late=0;
		if(now>fb->fTickDeadline)absolutetime_to_nanoseconds(now-fb->fTickDeadline,&late);
		if(late>fb->fCadenceMaxLateNs)fb->fCadenceMaxLateNs=late;
	}
	uint64_t count = ++fb->fVsyncCount;
	uint64_t values[6] = {
		(uint32_t)count, (uint32_t)(count >> 32),
		(uint32_t)now, (uint32_t)(now >> 32),
		0, 0,
	};

	IOLockLock(fb->fLock);
	for (unsigned int i = 0; i < fb->fClients->getCount(); i++) {
		IOMobileFramebufferUserClient *client =
		    (IOMobileFramebufferUserClient *)fb->fClients->getObject(i);
		client->notify(SWEET_FB_NOTIFY_VSYNC, values, 6);
	}
	if (fb->fIntf1 != NULL && (count % 300) == 0) {
		uint32_t frames = fb->fIntf1[(0x800 + 0x0ac) / 4];
		IOLog("SWEET INTF1: %u frames in 5 s (line %u, te_en %#x)\n",
		    frames - fb->fIntfFrames, fb->fIntf1[(0x800 + 0x0b0) / 4], fb->fIntf1[0x800 / 4]);
		fb->fIntfFrames = frames;
	}
	if (fb->fPowerOff) {
		fb->fX9ComposeValid = false;  /* the fade below writes the panel directly */
		if(sweet_board_x9()) {
			// Do not let an armed GUI transfer start after a power-off request,
			// or fade memory while DSI is still reading the previous picture.
			if(fb->fX9SubmitPending && x9_lk_gui_cancel()) {
				fb->fX9SubmitPending=false;fb->fX9PresentTimer->cancelTimeout();
			}
			if(fb->fX9SubmitPending || x9_lk_gui_busy()!=0) {
				IOLockUnlock(fb->fLock);fb->scheduleVsync(sender);return;
			}
		}
		/*
		 * topaz: fade in cached memory and flip whole frames to the MDP.
		 * Fading the uncached boot framebuffer in place crawled down the panel.
		 */
		if (fb->fDsi != NULL) {
			/* the panel is dark (DCS 0x51 0); keep the last frame */
			IOLockUnlock(fb->fLock);
			fb->scheduleVsync(sender);
			return;
		}
		if (fb->fFadeOrig != NULL) {
			uint32_t step = fb->fFadeStep;
			if (step <= SWEET_FB_FADE_STEPS) {
				fb->fFadeStep = step + 1;
			}
			bool front = fb->fFlip[0] != NULL && fb->fScanPhys == fb->fFlipPhys[fb->fFlipIdx];
			const uint8_t *src = front ? (const uint8_t *)fb->fFlip[fb->fFlipIdx]->getBytesNoCopy() :
			    fb->fLastVA ? fb->fLastVA :
			    fb->fConsShown ?
			    (const uint8_t *)fb->fCons->getBytesNoCopy() : (const uint8_t *)fb->fPixels;
			uint32_t srcStride = (!front && fb->fLastVA) ? fb->fLastStride : fb->fRowBytes;
			IOLockUnlock(fb->fLock);
			if (step <= SWEET_FB_FADE_STEPS) {
				fb->fadeStep(src, srcStride, step);
			}
			fb->scheduleVsync(sender);
			return;
		}
		if (fb->fVig0 != NULL && fb->fScanPhys != fb->fBootSrc) {
			fb->scanFrom(fb->fBootSrc, fb->fBootStride);
		}
		/* Fade out: halve every channel each vsync, then black, then idle. */
		uint32_t step = fb->fFadeStep;
		if (step <= 6) {
			fb->fFadeStep = step + 1;
		}
		IOLockUnlock(fb->fLock);
		if (step <= 6) {
			for (uint32_t y = 0; y < fb->fHeight; y++) {
				volatile uint32_t *row = (volatile uint32_t *)(fb->fPixels + (uint64_t)y * fb->fRowBytes);
				for (uint32_t x = 0; x < fb->fWidth; x++) {
					row[x] = step < 6 ? (row[x] >> 1) & 0x7f7f7f7f : 0;
				}
			}
		}
		fb->scheduleVsync(sender);
		return;
	}
	bool flush = fb->fDirty && (count % fb->fFlushTicks) == 0;
	int displayOwner=0;
	if(flush && sweet_board_x9()) {
		// fLock -> GPU owner is TRY-only; never wait in the inverse order.
		// Hold native GPU ownership from before taking the pending frame through
		// its CPU copy. Keep dirty+IOSurface holds intact when either engine is busy.
		/* A/B (boot-arg token x9qd, with the frontend's threaded submit): the
		 * steady-state present is a CPU copy of a held IOSurface (GPU staging
		 * only runs for the first frames), so do not compete with GPU submits
		 * for metal_service_lock. rev245 x9q1: gpu_defer rose 2.9 -> 10.1/s. */
		static int lockFree=-1;
		if(lockFree<0){int v=0;lockFree=PE_parse_boot_argn("x9qd",&v,sizeof(v))?1:0;
			IOLog("X9 DISPLAY: present %s the GPU owner lock (x9qd)\n",lockFree?"without":"with");}
		displayOwner=lockFree?0:sweet_x9_render_display_try_enter();
		if(displayOwner<0) {fb->fCadenceGpuBusy++;flush=false;}
		else if(fb->fX9SubmitPending || x9_lk_gui_busy()==1) {fb->fCadenceDisplayBusy++;flush=false;}
	}
	uint8_t *rec = NULL;
	SweetSurface *taken[4] = {};
	if (flush) {
		rec = (uint8_t *)IOMallocData(SWEET_FB_SWAP_REC_SIZE);
		if (rec != NULL) {
			memcpy(rec, fb->fPending, SWEET_FB_SWAP_REC_SIZE);
			fb->fDirty = false;
			memcpy(taken, fb->fPendHold, sizeof(taken));
			bzero(fb->fPendHold, sizeof(fb->fPendHold));
		}
	}
	if(displayOwner>0 && rec==NULL) {sweet_x9_render_owner_leave();displayOwner=0;}
	/* frames replaced at the previous vsync have been latched by now */
	SweetSurface *retire[4];
	memcpy(retire, fb->fRetireHold, sizeof(retire));
	bzero(fb->fRetireHold, sizeof(fb->fRetireHold));
	IOLockUnlock(fb->fLock);
	sweet_hold_set(retire, false);
	if (fb->fConsShown && sweet_console_copy_dirty(fb->backBuffer())) {
		fb->flipBack();
	}
	if (rec != NULL) {
		uint64_t began=mach_absolute_time();
		fb->fX9OwnerHeld=displayOwner>0||!sweet_board_x9();
		fb->present(rec);
		fb->fX9OwnerHeld=false;
		if(displayOwner>0) {sweet_x9_render_owner_leave();displayOwner=0;}
		if(sweet_board_x9()) {
			uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time()-began,&ns);
			fb->fCadenceCopies++;fb->fCadenceCopyNs+=ns;
			if(ns>fb->fCadenceMaxCopyNs)fb->fCadenceMaxCopyNs=ns;
		}
		IOFreeData(rec, SWEET_FB_SWAP_REC_SIZE);
		IOLockLock(fb->fLock);
		memcpy(fb->fRetireHold, fb->fShowHold, sizeof(taken));
		memcpy(fb->fShowHold, taken, sizeof(taken));
		IOLockUnlock(fb->fLock);
	} else {
		IOLockLock(fb->fLock);
		memcpy(fb->fRetireHold, fb->fShowHold, sizeof(fb->fRetireHold));
		bzero(fb->fShowHold, sizeof(fb->fShowHold));
		IOLockUnlock(fb->fLock);
	}
	/* Retry a busy GUI submission on the same serialized timer workloop.
	 * No timeout/reset or clock/PHY changes. Latest composed pixels win. */
	if (sweet_board_x9()) {
		IOLockLock(fb->fLock);
		bool publish_allowed=!fb->fPowerOff&&!fb->fConsShown&&!fb->fDirty&&!fb->fX9SubmitPending;
		IOLockUnlock(fb->fLock);
		if(sweet_x9_desktop_frame_poll((uint8_t *)fb->fPixels,fb->fRowBytes,fb->fWidth,fb->fHeight,x9_fb_present_seq,
		    publish_allowed&&x9_lk_gui_busy()==0)) {
			fb->fX9ComposeValid=false;  // GPU result was copied into the panel
			fb->x9QueuePresent();
			IOLog("X9 DESKTOP FRAME: generation=%llu LK asynchronous fresh-TE submit\n",x9_fb_present_seq);
		}
		static unsigned heartbeat;
		if (++heartbeat % 600 == 0) {
			IOLog("X9 GUI heartbeat: swap_id=%u dirty=%u retry=%u power_off=%u busy=%d\n",
			 fb->fSwapID,fb->fDirty,fb->fX9SubmitPending,fb->fPowerOff,x9_lk_gui_busy());
			x9_lk_gui_probe();
			sweet_x9_clear_lk_overlays();
			/* Transport check without CoreBrightness: once the desktop is
			 * composing, send the default DBV once (with 0x52/0x0A readback). */
			static bool blProbe;
			if (fb->fX9Bl && !blProbe && fb->fSwapID >= 60 && fb->fBlCall != NULL) {
				blProbe = true;
				thread_call_enter(fb->fBlCall);
			}
		}
		if (fb->fX9ObservePending) {
			int busy=x9_lk_gui_busy();
			if (busy!=1 || ++fb->fX9BusyTicks==120) {
				fb->fX9ObservePending=false;
				static unsigned observations;
				if (observations++<12) {
					IOLog("X9 GUI status: ticks=%u busy=%d (idle is not scanout proof)\n",fb->fX9BusyTicks,busy);
					x9_lk_gui_probe();
					sweet_x9_clear_lk_overlays();
				}
			}
		}
	}
	if (sweet_board_x676c()) {
		fb->mtkDsiPush();
	}
	fb->scheduleVsync(sender);
}

void
AppleCLCD::addClient(IOMobileFramebufferUserClient *client)
{
	IOLockLock(fLock);
	fClients->setObject(client);
	IOLockUnlock(fLock);
}

void
AppleCLCD::removeClient(IOMobileFramebufferUserClient *client)
{
	IOLockLock(fLock);
	unsigned int i = fClients->getNextIndexOfObject(client, 0);
	if (i != (unsigned int)-1) {
		fClients->removeObject(i);
	}
	IOLockUnlock(fLock);
}

static uint32_t
get32(const uint8_t *p, uint32_t off)
{
	uint32_t v;
	memcpy(&v, p + off, 4);
	return v;
}

static uint32_t sweet_fb_swaps_logged;
struct x9_fb_sample { uint32_t samples,nonblack,opaque,checksum; };
static struct x9_fb_sample
x9_fb_sample_grid(const uint8_t *pixels,uint32_t width,uint32_t height,uint32_t stride,uint32_t grid)
{
 struct x9_fb_sample sample={};
 if(!pixels||!width||!height||stride<(uint64_t)width*4)return sample;
 if(grid<2||grid>128)grid=32;
 for(uint32_t y=0;y<grid;y++)for(uint32_t x=0;x<grid;x++){
  uint32_t v;uint64_t off=(uint64_t)((uint64_t)y*(height-1)/(grid-1))*stride+(uint64_t)((uint64_t)x*(width-1)/(grid-1))*4;
  memcpy(&v,pixels+off,sizeof(v));sample.samples++;
  if(v&0x00ffffffU)sample.nonblack++;
  if(v>>24)sample.opaque++;
  sample.checksum=(sample.checksum*16777619U)^v;
 }
 return sample;
}

static unsigned x9_fb_probe_count;
static uint64_t sweet_fb_swap_count;

/* Copy each enabled layer's surface into the panel memory (no scaling). */
/*
 * A swap only records the frame: copying 10 MB into the panel on every
 * swap (backboardd presents at 60 Hz) took the whole CPU. The vsync timer
 * shows the latest frame every fFlushTicks ticks (boot-arg sweetfb_hz,
 * default 60 Hz on X9, with explicit 120 Hz pacing; 10 Hz elsewhere).
 */
void
AppleCLCD::swap(const uint8_t *rec)
{
	SweetSurface *held[4] = {};
	uint32_t mask = get32(rec, 0x104);
	for (uint32_t i = 0; i < 4 && i < SWEET_FB_LAYERS; i++) {
		uint32_t sid = get32(rec, 0x54 + 4 * i);
		if ((sweet_board_x9() || (mask & (1u << i))) && sid != 0 && (held[i] = sweet_surface_copy(sid)) != NULL) {
			sweet_hold(held[i], true);
		}
	}
	IOLockLock(fLock);
	memcpy(fPending, rec, SWEET_FB_SWAP_REC_SIZE);
	fDirty = true;
	SweetSurface *dropped[4];
	memcpy(dropped, fPendHold, sizeof(dropped));      /* replaced before shown */
	memcpy(fPendHold, held, sizeof(held));
	IOLockUnlock(fLock);
	sweet_hold_set(dropped, false);
	if ((++sweet_fb_swap_count % 1000) == 0) {
		IOLog("SWEET FB: %llu swaps\n", sweet_fb_swap_count);
	}
}

void
AppleCLCD::scanFrom(uint64_t pa, uint32_t stride)
{
	fVig0[0x14 / 4] = (uint32_t)pa;
	fVig0[0x24 / 4] = stride;
	__asm__ volatile("dsb sy" ::: "memory");
	fCtl0[0x18 / 4] = 0x20001;      /* flush VIG0 + CTL: latched at the next vsync */
	__asm__ volatile("dsb sy" ::: "memory");
	fScanPhys = pa;
	fScanStride = stride;
}

/*
 * DCS long write through the DSI command DMA path (topaz-woa: the TPG FIFO
 * path is a no-op on this DSI 2.4.1). The packet lives in a contiguous
 * buffer below 4 GiB; header = word count, data type 0x39, flags last|long.
 */
static inline uint32_t dsi_rd(volatile uint8_t *b, uint32_t o) { return *(volatile uint32_t *)(b + o); }
static inline void dsi_wr(volatile uint8_t *b, uint32_t o, uint32_t v)
{
	__asm__ volatile ("dsb sy" ::: "memory");
	*(volatile uint32_t *)(b + o) = v;
	__asm__ volatile ("dsb sy" ::: "memory");
}

bool
AppleCLCD::dsiDcsLong(const uint8_t *payload, uint32_t len)
{
	if (fDsi == NULL || fDsiBuf == NULL || len > 60) {
		return false;
	}
	uint8_t hdr[4] = { (uint8_t)len, (uint8_t)(len >> 8), 0x39, 0xc0 };   /* long write, last|long */
	return dsiSend(hdr, payload, len);
}

bool
AppleCLCD::dsiSend(const uint8_t hdr[4], const uint8_t *payload, uint32_t len)
{
	if (fDsi == NULL || fDsiBuf == NULL || len > 60) {
		return false;
	}
	uint8_t *p = (uint8_t *)fDsiBuf->getBytesNoCopy();
	uint32_t size = (4 + len + 3) & ~3u;
	memset(p, 0xff, 64);
	memcpy(p, hdr, 4);
	if (len) {
		memcpy(p + 4, payload, len);
	}
	sweet_dcache_clean((vm_offset_t)(uintptr_t)p, 64);
	uint32_t ctrl = dsi_rd(fDsi, 0x004), trig = dsi_rd(fDsi, 0x084);
	if ((trig & 7) != 4) {
		dsi_wr(fDsi, 0x084, (trig & ~7u) | 4);  /* DMA trigger: software */
	}
	dsi_wr(fDsi, 0x004, ctrl | 4 | 1);              /* CMD_MODE_EN | DSI_EN */
	dsi_wr(fDsi, 0x110, dsi_rd(fDsi, 0x110) | 1);   /* clear CMD_DMA_DONE */
	dsi_wr(fDsi, 0x15c, 0);                          /* no test pattern */
	dsi_wr(fDsi, 0x048, (uint32_t)fDsiBufPhys);
	dsi_wr(fDsi, 0x04c, size);
	uint32_t dma = dsi_rd(fDsi, 0x03c);
	dsi_wr(fDsi, 0x03c, dma & ~(1u << 26));          /* high speed */
	dsi_wr(fDsi, 0x090, 1);                          /* SW trigger */
	bool done = false;
	for (int i = 0; i < 100 && !done; i++) {
		done = (dsi_rd(fDsi, 0x110) & 1) != 0;
		if (!done) {
			IODelay(200);
		}
	}
	dsi_wr(fDsi, 0x110, dsi_rd(fDsi, 0x110) | 1);
	dsi_wr(fDsi, 0x03c, dma);
	dsi_wr(fDsi, 0x004, ctrl);
	if (trig != dsi_rd(fDsi, 0x084)) {
		dsi_wr(fDsi, 0x084, trig);
	}
	return done;
}

/* DCS 0x28 display off / 0x29 display on (short write, no parameter) */
void
AppleCLCD::setPanelOn(bool on)
{
	uint8_t hdr[4] = { (uint8_t)(on ? 0x29 : 0x28), 0, 0x05, 0x80 };
	IOLockLock(fBlLock);
	bool done = dsiSend(hdr, NULL, 0);
	IOLockUnlock(fBlLock);
	IOLog("SWEET FB: panel display %s: %s\n", on ? "on" : "off", done ? "done" : "NO DONE");
}

void
AppleCLCD::setPanelLevel(uint32_t level)
{
	uint8_t cmd[3] = { 0x51, (uint8_t)(level >> 8), (uint8_t)level };   /* MSB first */
	if (fX9Bl) {
		/*
		 * Only between frames: x9_lk_panel_dcs returns 0 while a frame is in
		 * flight or the submit lock is held. Runs on fBlCall, so sleeping is
		 * fine. The first two sends read back 0x52 (DBV) and 0x0A (0x9C =
		 * healthy per the panel's ESD config).
		 */
		uint32_t rd[4] = {};
		int r = 0;
		unsigned tries = 0;
		IOLockLock(fBlLock);
		bool readback = fX9BlSent < 2;
		while (tries < 100 && (r = x9_lk_panel_dcs(cmd, sizeof(cmd), readback, rd)) == 0) {
			tries++;
			IOSleep(1);
		}
		if (r == 1) {
			fX9BlSent++;
		} else {
			fX9BlFail++;
		}
		fX9BlBusy += tries;
		uint32_t sent = fX9BlSent, fail = fX9BlFail, busy = fX9BlBusy;
		IOLockUnlock(fBlLock);
		if (sent <= 8 || r != 1 || (sent % 32) == 0) {
			IOLog("X9 BL: DBV %u (0x%03x) r=%d retries=%u sent=%u fail=%u busy_total=%u cmdq0=%08x con=%08x\n",
			    level, level, r, tries, sent, fail, busy, rd[2], rd[3]);
		}
		if (readback && r == 1) {
			IOLog("X9 BL: readback 0x52 di=%02x dbv=%02x%02x, 0x0A di=%02x mode=%02x (0x9c healthy)\n",
			    rd[0] & 0xff, (rd[0] >> 8) & 0xff, (rd[0] >> 16) & 0xff, rd[1] & 0xff, (rd[1] >> 8) & 0xff);
		}
		return;
	}
	IOLockLock(fBlLock);
	bool done = dsiDcsLong(cmd, sizeof(cmd));
	IOLockUnlock(fBlLock);
	static uint32_t logs;
	if (logs++ < 8 || !done) {
		IOLog("SWEET FB: panel level %#x: %s\n", level, done ? "done" : "NO DONE");
	}
}

/*
 * CoreBrightness (SetIOMFBBrightness) sets IOMFBBrightnessLevel on the
 * framebuffer: nits in 16.16. It also animates the dim before the display
 * turns off, so the panel dims while iOS keeps drawing, as on an iPhone.
 */
IOReturn
AppleCLCD::setProperties(OSObject *properties)
{
	OSDictionary *d = OSDynamicCast(OSDictionary, properties);
	sweet_bl_log("iomfb", d);
	OSNumber *n = d ? OSDynamicCast(OSNumber, d->getObject("IOMFBBrightnessLevel")) : NULL;
	if (n == NULL || !blReady()) {
		return kIOReturnUnsupported;
	}
	uint64_t v = n->unsigned64BitValue();
	setProperty("IOMFBBrightnessLevel", v, 32);
	uint64_t level;
	if (fX9Bl) {
		/* 0..SWEET_FB_MAX_NITS nits -> DBV 2..3515 (linear; no HBM) */
		level = X9_BL_DBV_MIN + (v * (X9_BL_DBV_MAX - X9_BL_DBV_MIN)) / ((uint64_t)SWEET_FB_MAX_NITS << 16);
		if (level > X9_BL_DBV_MAX) {
			level = X9_BL_DBV_MAX;
		}
	} else {
		/* 0..SWEET_FB_MAX_NITS nits -> DBV 1..0x7ff */
		level = (v * 0x7ffull) / ((uint64_t)SWEET_FB_MAX_NITS << 16);
		if (level > 0x7ff) {
			level = 0x7ff;
		}
		if (level < 1) {
			level = 1;
		}
	}
	IOLockLock(fLock);
	fBlLevel = (uint32_t)level;
	IOLockUnlock(fLock);
	/*
	 * CoreBrightness runs inside backboardd and ramps the level in many small
	 * synchronous steps; waiting for the DSI here stalled rendering. Only note
	 * the level; a thread call sends the latest one.
	 */
	if (fBlCall != NULL) {
		thread_call_enter(fBlCall);
	}
	return kIOReturnSuccess;
}

void
AppleCLCD::blWork(thread_call_param_t p0, __unused thread_call_param_t p1)
{
	AppleCLCD *fb = (AppleCLCD *)p0;
	IOLockLock(fb->fLock);
	uint32_t level = fb->fBlLevel;
	bool off = fb->fPowerOff;
	IOLockUnlock(fb->fLock);
	if (!off && level != fb->fBlSent) {
		fb->fBlSent = level;
		fb->setPanelLevel(level);
	}
}

static void
sweet_bl_log(const char *who, OSDictionary *d)
{
	static uint32_t logs;
	if (d == NULL || logs >= 24) {
		return;
	}
	OSCollectionIterator *it = OSCollectionIterator::withCollection(d);
	OSSymbol *k;
	while (it != NULL && (k = OSDynamicCast(OSSymbol, it->getNextObject())) != NULL) {
		OSObject *v = d->getObject(k);
		OSNumber *n = OSDynamicCast(OSNumber, v);
		if (logs++ < 24) {
			IOLog("SWEET BL: %s set %s = %s %llu\n", who, k->getCStringNoCopy(),
			    v ? v->getMetaClass()->getClassName() : "null", n ? n->unsigned64BitValue() : 0);
		}
	}
	OSSafeReleaseNULL(it);
}

IOReturn
AppleARMBacklight::setProperties(OSObject *properties)
{
	OSDictionary *d = OSDynamicCast(OSDictionary, properties);
	sweet_bl_log("backlight", d);
	OSNumber *n = d ? OSDynamicCast(OSNumber, d->getObject("brightness-nits")) : NULL;
	if (n != NULL && fFB != NULL) {
		OSDictionary *m = OSDictionary::withCapacity(1);
		/* same 16.16 nits scale as IOMFBBrightnessLevel */
		uint64_t v = n->unsigned64BitValue();
		if (v < 0x10000) {
			v <<= 16;               /* plain nits */
		}
		OSNumber *nn = OSNumber::withNumber(v, 64);
		m->setObject("IOMFBBrightnessLevel", nn);
		fFB->setProperties(m);
		nn->release();
		m->release();
		setProperty("brightness-nits", n);
	}
	return kIOReturnSuccess;
}

/* panic: show the console frame (MMIO only, no locks) */
static void
sweet_cons_show(void)
{
	AppleCLCD *fb = sweet_cons_fb;
	if (fb != NULL && fb->fVig0 != NULL && fb->fScanPhys != fb->fConsPhys) {
		fb->scanFrom(fb->fConsPhys, fb->fRowBytes);
	}
	mtk_dsi_push_hw();
}

void
AppleCLCD::mtkDsiPush(void)
{
	mtk_dsi_push_hw();
}

void
AppleCLCD::fadeStep(const uint8_t *src, uint32_t srcStride, uint32_t step)
{
	uint8_t *orig = (uint8_t *)fFadeOrig->getBytesNoCopy();
	uint8_t *out = backBuffer();
	if (step == 0) {
		/* snapshot what is on screen (one slow read of the frame) */
		for (uint32_t y = 0; y < fHeight; y++) {
			memcpy(orig + (uint64_t)y * fRowBytes, src + (uint64_t)y * srcStride, (size_t)fWidth * 4);
		}
	}
	/* linear fade: each channel * m / 32, m from 32 down to 0 */
	uint32_t m = 32 - (step * 32 + SWEET_FB_FADE_STEPS / 2) / SWEET_FB_FADE_STEPS;
	for (uint32_t y = 0; y < fHeight; y++) {
		const uint32_t *in = (const uint32_t *)(orig + (uint64_t)y * fRowBytes);
		uint32_t *o = (uint32_t *)(out + (uint64_t)y * fRowBytes);
		for (uint32_t x = 0; x < fWidth; x++) {
			uint32_t p = in[x];
			uint32_t lo = (((p & 0x00ff00ffu) * m) >> 5) & 0x00ff00ffu;
			uint32_t hi = ((((p >> 8) & 0x00ff00ffu) * m) >> 5) & 0x00ff00ffu;
			o[x] = lo | (hi << 8);
		}
	}
	flipBack();
}

/* Clean the back buffer and point the MDP at it (latched at its vsync). */
void
AppleCLCD::flipBack(void)
{
	uint32_t idx = fFlipIdx ^ 1;
	sweet_dcache_clean((vm_offset_t)(uintptr_t)fFlip[idx]->getBytesNoCopy(), (unsigned)fFlip[idx]->getLength());
	scanFrom(fFlipPhys[idx], fRowBytes);
	fFlipIdx = idx;
	if (sweet_board_x676c()) {
		if (fPixels != 0 && fFlip[idx] != NULL) {
			memcpy((void *)fPixels, fFlip[idx]->getBytesNoCopy(), (size_t)fRowBytes * fHeight);
			sweet_dcache_clean((vm_offset_t)fPixels, (unsigned)((size_t)fRowBytes * fHeight));
		}
		mtkDsiPush();
	}
}

void
AppleCLCD::flipFrom(const uint8_t *src, uint32_t srcStride)
{
	uint8_t *dst = backBuffer();
	if (srcStride == fRowBytes) {
		memcpy(dst, src, (size_t)fRowBytes * fHeight);
	} else {
		for (uint32_t y = 0; y < fHeight; y++) {
			memcpy(dst + (uint64_t)y * fRowBytes, src + (uint64_t)y * srcStride, (size_t)fWidth * 4);
		}
	}
	flipBack();
}

#include "X9DisplayBackend.inc"

/*
 * topaz: a swap that shows one full-screen surface from the scanout pool is
 * presented by pointing the MDP at it (no 10 MB CPU copy). Returns false to
 * fall back to the copy path.
 */
bool
AppleCLCD::directPresent(const uint8_t *rec)
{
	if (fVig0 == NULL) {
		return false;
	}
	uint32_t mask = get32(rec, 0x104), layer = 32, n = 0;
	for (uint32_t i = 0; i < SWEET_FB_LAYERS; i++) {
		if ((mask & (1u << i)) && get32(rec, 0x54 + 4 * i) != 0) {
			layer = i;
			n++;
		}
	}
	if (n != 1) {
		return false;
	}
	SweetSurface *s = sweet_surface_copy(get32(rec, 0x54 + 4 * layer));
	if (s == NULL) {
		return false;
	}
	bool ok = false;
	x9_display::Surface surface = {
		.scanout_phys = s->scanoutPhys,
		.offset = s->offset,
		.memory_length = s->memory ? s->memory->getLength() : 0,
		.kernel_address = 0,
		.pixel_format = s->pixelFormat,
		.bytes_per_element = s->bytesPerElement,
		.element_width = s->elementWidth,
		.element_height = s->elementHeight,
		.width = s->width,
		.height = s->height,
		.bytes_per_row = s->bytesPerRow,
	};
	x9_display::Scanout scanout;
	if (x9_display::validate(surface, true, fWidth, fHeight, fLogicalW, fLogicalH,
	    fOffX, fOffY, &scanout, NULL)) {
		const uintptr_t va = s->kernelAddress();
		if (va != 0) {
			x9_display::mapped_addresses(surface, va, &scanout);
			/*
			 * Copy into our back buffer and flip: CoreAnimation may start the
			 * next frame in this surface at once, so scanning it directly let
			 * the panel show frames half drawn. Cached to cached, a few ms.
			 */
			if (fFlip[0] != NULL) {
				flipFrom((const uint8_t *)scanout.kernel_address, scanout.stride);
			} else {
				sweet_dcache_clean((vm_offset_t)scanout.cache_address, (unsigned)((uint64_t)fHeight * scanout.stride));
				scanFrom(scanout.phys, scanout.stride);
			}
			ok = true;
			IOLockLock(fLock);
			if (fLastSurf != s) {
				s->retain();
				if (fLastSurf != NULL) {
					fLastSurf->release();
				}
				fLastSurf = s;
			}
			fLastVA = (const uint8_t *)scanout.kernel_address;
			fLastStride = scanout.stride;
			IOLockUnlock(fLock);
		}
	}
	s->release();
	return ok;
}

/*
 * X9 layer drawing. IOMFB layers are stacked bottom-up by index; CoreAnimation
 * surfaces carry premultiplied alpha. The bottom drawn layer is copied as is
 * (premultiplied over black is the same pixels); upper layers are blended.
 * Source formats other than BGRA are converted or refused rather than copied
 * as if they were BGRA. sweetfb_blend=0 restores the old opaque copies.
 * The primitives live in X9DisplayBackend.inc; the call sites below keep their
 * previous names and argument order so this extraction changes no behavior.
 */
enum {
	X9_FMT_BGRA = x9_display::FormatBGRA,
	X9_FMT_RGBA = x9_display::FormatRGBA,
	X9_FMT_ARGB = x9_display::FormatARGB,
	X9_FMT_UNSUPPORTED = x9_display::FormatUnsupported,
};

static int
x9_layer_format(uint32_t format)
{
	return (int)x9_display::format(format);
}

static bool
x9_blend_enabled(void)
{
	return x9_display::blend_enabled();
}

static void
x9_layer_row(uint8_t *dst, const uint8_t *src, uint32_t count, int format, bool blend)
{
	x9_display::row(dst, src, count, (x9_display::Format)format, blend);
}

static void
x9_layer_scaled(uint8_t *dst, uint32_t dstStride, uint32_t panelW, uint32_t panelH,
    const uint8_t *src, uint32_t srcStride, int32_t sx, int32_t sy, int32_t sw, int32_t sh,
    int32_t dx, int32_t dy, int32_t dw, int32_t dh, int format, bool blend)
{
	x9_display::scaled(dst, dstStride, panelW, panelH, src, srcStride, sx, sy, sw, sh,
	    dx, dy, dw, dh, (x9_display::Format)format, blend);
}


void
AppleCLCD::present(const uint8_t *rec)
{
	if (sweet_board_x9()) {static unsigned entered; if (entered++ < 2) IOLog("X9 DISPLAY: pan168 present entered;CPU first-frame,staged GPU output opt-in\n");}
	uint32_t mask = get32(rec, 0x104);
	bool x9_probe = false;
	if (sweet_board_x9()) {
		uint64_t seq = ++x9_fb_present_seq;
		x9_probe = seq <= 8 || seq % (fTickHz * 2) == 0;
	}
	static int console_only = -1;

	/* boot-arg sweetfb_console=1: debugging, the panel keeps showing the kernel console */
	if (console_only < 0) {
		console_only = 0;
		PE_parse_boot_argn("sweetfb_console", &console_only, sizeof(console_only));
	}
	if (console_only > 0 && fConsShown) {
		(void)mask;
		return;
	}
	fConsShown = false;
	if (directPresent(rec)) {
		if (!fConsolePaused) {
			fConsolePaused = true;
			int force_console = 0;
			PE_parse_boot_argn("sweetfb_console", &force_console, sizeof(force_console));
			if (!force_console) {
				sweet_console_muted = 1;
				IOLog("SWEET FB: GUI takeover, text console muted\n");
				if (sweet_board_x676c() && fPixels != 0) {
					bzero((void *)fPixels, (size_t)fRowBytes * fHeight);
				}
			}
		}
		return;
	}
	IOLockLock(fLock);
	fLastVA = NULL;
	IOLockUnlock(fLock);
	/*
	 * copy path (several layers): compose into the cached back buffer and
	 * flip, like the direct path. Composing into the scanned, uncached boot
	 * framebuffer showed every multi-layer frame (dims, apps) drawn top-down.
	 */
	uint8_t *dst = fFlip[0] != NULL ? backBuffer() : (uint8_t *)fPixels;
	bool x9_staged=false,x9_default_surface=false;
	if(sweet_board_x9()&&fSurface)for(unsigned layer=0;layer<SWEET_FB_LAYERS;layer++)
		if(get32(rec,0x54+4*layer)==fSurface->id)x9_default_surface=true;
	/* In-place userspace drawing into the default panel surface cannot be
	 * protected by this driver's swap-generation counter. Keep it on CPU. */
	if (sweet_board_x9() && fX9OwnerHeld && !x9_default_surface && fConsolePaused && !fPowerOff && dst==(uint8_t *)fPixels && x9_lk_gui_busy()==0) {
		uint8_t *stage=sweet_x9_desktop_frame_begin(dst,fRowBytes,fWidth,fHeight,x9_fb_present_seq);
		if(stage) {dst=stage;x9_staged=true;}
	}
	if (fFlip[0] == NULL && fVig0 != NULL && fScanPhys != fBootSrc) {
		scanFrom(fBootSrc, fBootStride);
	}


	if (!fConsolePaused) {
		fConsolePaused = true;
		int force_console = 0;
		PE_parse_boot_argn("sweetfb_console", &force_console, sizeof(force_console));
		if (!force_console) {
			sweet_console_muted = 1;
			IOLog("SWEET FB: GUI takeover, text console muted\n");
			if (sweet_board_x676c() && fPixels != 0) {
				bzero((void *)fPixels, (size_t)fRowBytes * fHeight);
			}
		}
	}

	/* X9 with more than one drawable layer: compose in the cached shadow and
	 * copy it to the panel once, so blending never reads the write-combined
	 * panel. A layer that is the panel itself (userspace draws in place)
	 * makes the shadow stale, so that frame stays on the direct path. */
	uint8_t *x9_panel = NULL;
	unsigned x9_layers = 0;
	bool x9_inplace = false;
	if (sweet_board_x9() && dst != NULL && x9_blend_enabled()) {
		for (uint32_t layer = 0; layer < SWEET_FB_LAYERS; layer++) {
			uint32_t id = get32(rec, 0x54 + 4 * layer);
			SweetSurface *s = id != 0 ? sweet_surface_copy(id) : NULL;
			if (s == NULL) {
				continue;
			}
			if (s == fSurface && s->kernelAddress() == fPixels) {
				x9_inplace = true;
			} else {
				x9_layers++;
			}
			s->release();
		}
		if (x9_layers > 1 && !x9_inplace && dst == (uint8_t *)fPixels) {
			if (fX9Compose == NULL) {
				fX9Compose = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
				    kIODirectionInOut, (vm_size_t)fRowBytes * fHeight, page_size);
				IOLog("X9 COMPOSE: shadow %ux%u stride %u %s\n", fWidth, fHeight, fRowBytes,
				    fX9Compose != NULL ? "allocated" : "allocation failed;opaque copies");
			}
			if (fX9Compose != NULL) {
				uint8_t *shadow = (uint8_t *)fX9Compose->getBytesNoCopy();
				if (!fX9ComposeValid) {
					/* One read of the panel to keep retained pixels. */
					memcpy(shadow, (const void *)fPixels, (size_t)fRowBytes * fHeight);
				}
				x9_panel = dst;
				dst = shadow;
			}
		}
	}
	bool x9_first_drawn = true;

	/*
	 * Diagnostic (x9vf): the layer rectangles CoreAnimation presented and
	 * their union — the only damage region the kernel can observe. Logged
	 * when the set changes and every 240 identical frames so a static
	 * desktop does not fill the log. Read-only.
	 */
	if (x9_diag_logging() && sweet_board_x9()) {
		static uint64_t last_sig;
		static unsigned same;
		int32_t ux0 = 0x7fffffff, uy0 = 0x7fffffff;
		int32_t ux1 = -0x7fffffff, uy1 = -0x7fffffff;
		uint32_t count = 0;
		uint64_t sig = 1469598103934665603ULL;
		for (uint32_t layer = 0; layer < SWEET_FB_LAYERS; layer++) {
			uint32_t id = get32(rec, 0x54 + 4 * layer);
			if (!(mask & (1u << layer)) || id == 0)
				continue;
			const uint8_t *b = rec + 0xc4 + 16 * layer;
			int32_t dx = (int32_t)get32(b, 0), dy = (int32_t)get32(b, 4);
			int32_t w = (int32_t)get32(b, 8), h = (int32_t)get32(b, 12);
			count++;
			if (w > 0 && h > 0) {
				ux0 = MIN(ux0, dx); uy0 = MIN(uy0, dy);
				ux1 = MAX(ux1, dx + w); uy1 = MAX(uy1, dy + h);
			}
			sig = (sig ^ ((uint64_t)id << 32 | layer)) * 1099511628211ULL;
			sig = (sig ^ ((uint64_t)(uint32_t)dx << 32 | (uint32_t)dy)) * 1099511628211ULL;
			sig = (sig ^ ((uint64_t)(uint32_t)w << 32 | (uint32_t)h)) * 1099511628211ULL;
		}
		if (count == 0) {
			ux0 = uy0 = ux1 = uy1 = 0;
		}
		sig = (sig ^ mask) * 1099511628211ULL;
		sig = (sig ^ get32(rec, 0x50)) * 1099511628211ULL;
		if (sig != last_sig || ++same >= 240) {
			const bool covers = count != 0 && ux0 <= 0 && uy0 <= 0 &&
			    ux1 >= (int32_t)fLogicalW && uy1 >= (int32_t)fLogicalH;
			IOLog("X9 DAMAGE: seq=%llu id=%u layers=%u union=%d,%d,%dx%d panel=%ux%u covers=%u mask=%08x\n",
			    x9_fb_present_seq, get32(rec, 0x50), count, ux0, uy0, ux1 - ux0, uy1 - uy0,
			    fLogicalW, fLogicalH, covers, mask);
			for (uint32_t layer = 0; layer < SWEET_FB_LAYERS; layer++) {
				uint32_t id = get32(rec, 0x54 + 4 * layer);
				if (!(mask & (1u << layer)) || id == 0)
					continue;
				const uint8_t *a = rec + 0x64 + 16 * layer;
				const uint8_t *b = rec + 0xc4 + 16 * layer;
				IOLog("X9 LAYER: seq=%llu l=%u id=%u src=%d,%d,%dx%d dst=%d,%d,%dx%d\n",
				    x9_fb_present_seq, layer, id,
				    (int32_t)get32(a, 0), (int32_t)get32(a, 4), (int32_t)get32(a, 8), (int32_t)get32(a, 12),
				    (int32_t)get32(b, 0), (int32_t)get32(b, 4), (int32_t)get32(b, 8), (int32_t)get32(b, 12));
			}
			last_sig = sig;
			same = 0;
		}
	}

	/* Reference: hackios c84b4634277a70beac73921029b1fb6afc2530af,
	 * MiniIOMobileFramebuffer::show (20F66). The 20H392 request also has
	 * surface IDs at +0x54, set bits at +0x104 and cleared bits at +0x108.
	 * X9 has no flip buffer: its default surface aliases dst. Do not erase
	 * that surface before presenting it, or interpret set bits as enable.
	 * Only an explicit removal of layer zero clears the retained picture.
	 * No MT6993 MMIO or page-size-dependent ABI changes here.
	 */
	if (dst != NULL && (!sweet_board_x9() ||
	    ((mask & 1u) && get32(rec, 0x54) == 0))) {
		if(x9_staged)for(unsigned y=0;y<fHeight;y++)bzero(dst+(uint64_t)y*fRowBytes,fWidth*4);
		else bzero(dst, (size_t)fRowBytes * fHeight);
	}
	if (sweet_board_x9()) {
		static unsigned records;
		if (x9_probe) {
			records++;
			IOLog("X9 SWAP record: id=%u set=%08x clear=%08x surfaces=%u,%u,%u,%u\n",
			    get32(rec, 0x50), mask, get32(rec, 0x108),
			    get32(rec, 0x54), get32(rec, 0x58), get32(rec, 0x5c), get32(rec, 0x60));
		}
	}

	for (uint32_t layer = 0; layer < SWEET_FB_LAYERS; layer++) {
		uint32_t id = get32(rec, 0x54 + 4 * layer);
		const uint8_t *a = rec + 0x64 + 16 * layer;
		const uint8_t *b = rec + 0xc4 + 16 * layer;
		int32_t sx = (int32_t)get32(a, 0), sy = (int32_t)get32(a, 4);
		int32_t dx = (int32_t)get32(b, 0), dy = (int32_t)get32(b, 4);
		int32_t w = (int32_t)get32(b, 8), h = (int32_t)get32(b, 12);

		if ((!sweet_board_x9() && !(mask & (1u << layer))) || id == 0) {
			continue;
		}
		SweetSurface *s = sweet_surface_copy(id);
		if (sweet_fb_swaps_logged < 16) {
			sweet_fb_swaps_logged++;
			IOLog("SWEET FB: swap %u layer %u surface %u %ux%u fmt 0x%x bpr %u a %d,%d %ux%u b %d,%d %dx%d\n",
			    get32(rec, 0x50), layer, id, s ? s->width : 0, s ? s->height : 0,
			    s ? s->pixelFormat : 0, s ? s->bytesPerRow : 0, sx, sy, get32(a, 8),
			    get32(a, 12), dx, dy, w, h);
		}
		if (s == NULL || (s == fSurface && s->kernelAddress() == fPixels)) {
			if (sweet_board_x9()) {
				static unsigned skipped;
				if (skipped++ < 24) IOLog("X9 SWAP layer: id=%u layer=%u %s\n", id, layer,
				    s == NULL ? "missing surface" : "default surface retained in place");
			}
			OSSafeReleaseNULL(s);
			continue;
		}
		uintptr_t src = s->kernelAddress();
		uint32_t bpe = s->bytesPerElement ? s->bytesPerElement : 4;
		int x9_format = X9_FMT_BGRA;
		bool x9_blend = false;
		if (sweet_board_x9()) {
			/* sweetfb_blend=0 is the whole old compositor: raw BGRA copies. */
			x9_format = x9_blend_enabled() ? x9_layer_format(s->pixelFormat) : X9_FMT_BGRA;
			if (x9_format == X9_FMT_UNSUPPORTED && bpe == 4) {
				static unsigned refused;
				if (refused++ < 16) IOLog("X9 COMPOSE: layer %u surface %u format 0x%x not drawn;no BGRA conversion\n",
				    layer, id, s->pixelFormat);
				s->release();
				continue;
			}
			/* Blend only into a cached target; never read the panel back. */
			x9_blend = !x9_first_drawn && x9_blend_enabled() && dst != (uint8_t *)fPixels;
			/* Scale only when the source rect is plain pixels inside the
			 * surface; anything else keeps the unscaled copy below. */
			int32_t aw = (int32_t)get32(a, 8), ah = (int32_t)get32(a, 12);
			if (bpe == 4 && src != 0 && x9_blend_enabled() && w > 0 && h > 0 && aw > 0 && ah > 0 &&
			    sx >= 0 && sy >= 0 && (int64_t)sx + aw <= (int64_t)s->width &&
			    (int64_t)sy + ah <= (int64_t)s->height && (aw != w || ah != h) &&
			    s->elementWidth == 1 && s->elementHeight == 1 && s->memory != NULL) {
				uint64_t slast = (uint64_t)s->offset + (uint64_t)(sy + ah - 1) * s->bytesPerRow +
				    (uint64_t)(sx + aw) * 4;
				if (slast <= s->memory->getLength()) {
					int32_t pdx = (dx < 0 || dy < 0) ? 0 : dx, pdy = (dx < 0 || dy < 0) ? 0 : dy;
					x9_layer_scaled(dst, fRowBytes, fWidth, fHeight, (const uint8_t *)(src + s->offset),
					    s->bytesPerRow, sx, sy, aw, ah, pdx - fOffX, pdy - fOffY, w, h, x9_format, x9_blend);
					x9_first_drawn = false;
					static unsigned scaled;
					if (scaled++ < 8) IOLog("X9 COMPOSE: layer %u surface %u scaled %dx%d -> %dx%d blend=%u\n",
					    layer, id, aw, ah, w, h, x9_blend ? 1 : 0);
				}
				s->release();
				continue;
			}
		}
		if (w <= 0 || h <= 0) {
			w = (int32_t)s->width;
			h = (int32_t)s->height;
		}
		if (sx < 0 || sy < 0) {
			sx = sy = 0;
		}
		if (dx < 0 || dy < 0) {
			dx = dy = 0;
		}
		/* Logical (presented) display coordinates -> panel coordinates. */
		dx -= fOffX;
		dy -= fOffY;
		if (dx < 0) {
			sx -= dx;
			w += dx;
			dx = 0;
		}
		if (dy < 0) {
			sy -= dy;
			h += dy;
			dy = 0;
		}
		if (w > (int32_t)s->width - sx) {
			w = (int32_t)s->width - sx;
		}
		if (h > (int32_t)s->height - sy) {
			h = (int32_t)s->height - sy;
		}
		if (w > (int32_t)fWidth - dx) {
			w = (int32_t)fWidth - dx;
		}
		if (h > (int32_t)fHeight - dy) {
			h = (int32_t)fHeight - dy;
		}
		uint64_t last = (uint64_t)s->offset + (uint64_t)(sy + h - 1) * s->bytesPerRow +
		    (uint64_t)(sx + w) * 4;
		if (w <= 0 || h <= 0 || s->elementWidth != 1 || s->elementHeight != 1 ||
		    s->memory == NULL || last > s->memory->getLength()) {
			if (sweet_board_x676c() || (sweet_board_x9() && x9_probe)) {
				IOLog("SWEET FB: copy skipped: w=%d h=%d mem=%p len=%llu last=%llu\n",
				    w, h, s->memory, s->memory ? (unsigned long long)s->memory->getLength() : 0, (unsigned long long)last);
			}
			src = 0;
		}
		if (src != 0 && bpe == 4) {
			if (x9_probe && sx == 0 && sy == 0 && dx == 0 && dy == 0 && w == (int32_t)fWidth && h == (int32_t)fHeight) {
				struct x9_fb_sample z = x9_fb_sample_grid((const uint8_t *)(src + s->offset), fWidth, fHeight, s->bytesPerRow, x9_diag_logging() ? 64 : 32);
				IOLog("X9 FRAME source seq=%llu id=%u sample=%u nonblack=%u alpha=%u checksum=%08x\n", x9_fb_present_seq, id, z.samples, z.nonblack, z.opaque, z.checksum);
			}
			if (sweet_board_x9()) {
				for (int32_t y = 0; y < h; y++) {
					x9_layer_row(dst + (uint64_t)(dy + y) * fRowBytes + (uint64_t)dx * 4,
					    (const uint8_t *)(src + s->offset + (uint64_t)(sy + y) * s->bytesPerRow + (uint64_t)sx * 4),
					    (uint32_t)w, x9_format, x9_blend);
				}
				x9_first_drawn = false;
			} else {
				for (int32_t y = 0; y < h; y++) {
					memcpy((void *)(dst + (uint64_t)(dy + y) * fRowBytes + (uint64_t)dx * 4),
					    (const void *)(src + s->offset + (uint64_t)(sy + y) * s->bytesPerRow + (uint64_t)sx * 4),
					    (size_t)w * 4);
				}
			}

		}
		s->release();
	}
	if (x9_panel != NULL) {
		memcpy(x9_panel, dst, (size_t)fRowBytes * fHeight);
		dst = x9_panel;
		fX9ComposeValid = true;
		static unsigned composed;
		if (composed++ < 4) IOLog("X9 COMPOSE: %u layers blended in shadow, copied to panel\n", x9_layers);
	} else if (sweet_board_x9()) {
		fX9ComposeValid = false;  /* the panel was written directly or not at all from the shadow */
	}
	if (sweet_board_x9() && dst != NULL) {
		struct x9_fb_sample z = x9_fb_sample_grid(dst, fWidth, fHeight, fRowBytes, x9_diag_logging() ? 64 : 32);
		static int previous_black=-1;
		int black=z.samples && !z.nonblack;
		bool transition=black!=previous_black;
		previous_black=black;
		if (transition) {
			x9_probe=true;
			IOLog("X9 FRAME black-transition: seq=%llu id=%u black=%d set=%08x clear=%08x surfaces=%u,%u,%u,%u\n",
			 x9_fb_present_seq,get32(rec,0x50),black,mask,get32(rec,0x108),
			 get32(rec,0x54),get32(rec,0x58),get32(rec,0x5c),get32(rec,0x60));
		}
		if(x9_probe) {
		IOLog("X9 FRAME destination seq=%llu sample=%u nonblack=%u alpha=%u checksum=%08x mask=%08x\n", x9_fb_present_seq, z.samples, z.nonblack, z.opaque, z.checksum, mask);
		x9_fb_probe_count++;
		}
	}
	if(x9_staged) {
		if(sweet_x9_desktop_frame_queue(dst,x9_fb_present_seq)) {
			IOLog("X9 DESKTOP FRAME: generation=%llu queued;LK submit deferred;previous image retained\n",x9_fb_present_seq);
			return;
		}
		for(unsigned y=0;y<fHeight;y++)memcpy((uint8_t *)fPixels+(uint64_t)y*fRowBytes,dst+(uint64_t)y*fRowBytes,fWidth*4);
		dst=(uint8_t *)fPixels;
		IOLog("X9 DESKTOP FRAME: queue rejected before GPU exposure;CPU stage copied to panel\n");
	}
	if (fFlip[0] != NULL) {
		flipBack();
	} else if (sweet_board_x676c()) {
		mtkDsiPush();
	} else if (sweet_board_x9() && dst != NULL && dst == (uint8_t *)fPixels) {
		x9QueuePresent();
		if (x9_probe) IOLog("X9 FRAME submit seq=%llu pending_fresh_TE=%u\n", x9_fb_present_seq, fX9SubmitPending);
		static unsigned logged;
		if (logged < 4) {
			IOLog("X9 GUI LK submit: fresh TE after completed copy;100us timer,50ms bound,no GPU lock while waiting\n");
			logged++;
		}
	}
}

bool
IOMobileFramebufferUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type)) {
		return false;
	}
	fTask = owningTask;
	return true;
}

bool
IOMobileFramebufferUserClient::start(IOService *provider)
{
	fFB = OSDynamicCast(AppleCLCD, provider);
	if (fFB == NULL || !IOUserClient::start(provider)) {
		return false;
	}
	fFB->addClient(this);
	return true;
}

IOReturn
IOMobileFramebufferUserClient::clientClose(void)
{
	if (fFB != NULL) {
		fFB->removeClient(this);
		IOLockLock(fFB->fLock);
		mach_port_t old = fPort;
		fPort = MACH_PORT_NULL;
		IOLockUnlock(fFB->fLock);
		if (old != MACH_PORT_NULL) {
			releaseNotificationPort(old);
		}
	}
	terminate();
	return kIOReturnSuccess;
}

IOReturn
IOMobileFramebufferUserClient::registerNotificationPort(mach_port_t port, __unused UInt32 type,
    __unused io_user_reference_t refCon)
{
	mach_port_t old;
	IOLockLock(fFB->fLock);
	old = fPort;
	fPort = port;
	IOLockUnlock(fFB->fLock);
	if (old != MACH_PORT_NULL) {
		releaseNotificationPort(old);
	}
	return kIOReturnSuccess;
}

/* Called with the framebuffer lock held. */
void
IOMobileFramebufferUserClient::notify(uint32_t type, const uint64_t *values, uint32_t count)
{
	OSAsyncReference64 ref;

	if (type >= SWEET_FB_NOTIFY_TYPES || fFunc[type] == 0 || fPort == MACH_PORT_NULL) {
		return;
	}
	bzero(ref, sizeof(ref));
	setAsyncReference64(ref, fPort, (mach_vm_address_t)fFunc[type], fRefcon[type], fTask);
	sendAsyncResult64WithOptions(ref, kIOReturnSuccess, (io_user_reference_t *)values, count,
	    kIOUserNotifyOptionCanDrop);
}

static uint64_t sweet_fb_logged;        /* selectors already logged (< 64) */

IOReturn
IOMobileFramebufferUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target,
    __unused void *reference)
{
	IOReturn ret = kIOReturnSuccess;
	uint64_t out[2] = { 0, 0 };
	uint32_t outCount = 0;

	switch (selector) {
	case kSweetFBGetLayerDefaultSurface:
		out[0] = sweet_surface_port_in_task(fFB->fSurface, fTask);
		outCount = 1;
		break;
	case kSweetFBSwapBegin:
		IOLockLock(fFB->fLock);
		out[0] = ++fFB->fSwapID;
		IOLockUnlock(fFB->fLock);
		outCount = 1;
		break;
	case kSweetFBSwapEnd: {
		uint8_t rec[SWEET_FB_SWAP_REC_SIZE];
		if (args->structureInputDescriptor != NULL ||
		    args->structureInputSize < SWEET_FB_SWAP_REC_SIZE) {
			ret = kIOReturnBadArgument;
			break;
		}
		memcpy(rec, args->structureInput, sizeof(rec));
		fFB->swap(rec);
		break;
	}
	case kSweetFBGetID:
		out[0] = 0;
		outCount = 1;
		break;
	case kSweetFBGetDisplaySize:
		out[0] = fFB->fLogicalW;
		out[1] = fFB->fLogicalH;
		outCount = 2;
		break;
	case kSweetFBIsMainDisplay:
		out[0] = 1;
		outCount = 1;
		break;
	case kSweetFBGetDotPitch:
		out[0] = SWEET_FB_DOT_PITCH;
		outCount = 1;
		break;
	case kSweetFBNotifications:
		if (args->scalarInputCount >= 3 && args->scalarInput[2] < SWEET_FB_NOTIFY_TYPES) {
			uint32_t type = (uint32_t)args->scalarInput[2];
			IOLockLock(fFB->fLock);
			fFunc[type] = args->scalarInput[0];
			fRefcon[type] = args->scalarInput[1];
			IOLockUnlock(fFB->fLock);
			char name[32];
			proc_selfname(name, sizeof(name));
			IOLog("SWEET FB: %s notifications type %u %s\n", name, type,
			    args->scalarInput[0] ? "on" : "off");
		}
		break;
	case kSweetFBRequestPowerChange:
		if (args->scalarInputCount >= 1) {
			bool off = args->scalarInput[0] == 0;
			/*
			 * topaz: sweetfb_stayon=1 ignores display-off requests (auto-lock)
			 * while there are no buttons or touch to wake the panel again.
			 */
			int stayon = 0;
			if (off && PE_parse_boot_argn("sweetfb_stayon", &stayon, sizeof(stayon)) && stayon) {
				IOLog("SWEET FB: display off ignored (sweetfb_stayon)\n");
				break;
			}
			IOLockLock(fFB->fLock);
			bool changed = off != fFB->fPowerOff;
			if (changed) {
				fFB->fPowerOff = off;
				fFB->fFadeStep = 0;
				fFB->fDirty = true;     /* on: redraw the latest frame */
			}
			uint32_t level = fFB->fBlLevel;
			IOLockUnlock(fFB->fLock);
			if (changed && fFB->fDsi != NULL) {
				if (off) {
					fFB->setPanelOn(false);
				} else {
					fFB->setPanelOn(true);
					fFB->fBlSent = level;
					fFB->setPanelLevel(level);
				}
			}
			IOLog("SWEET FB: display %s\n", off ? "off" : "on");
		}
		break;
	case kSweetFBSwapWait:
	default:
		break;
	}

	for (uint32_t i = 0; i < args->scalarOutputCount; i++) {
		args->scalarOutput[i] = i < outCount ? out[i] : 0;
	}
	if (args->structureOutputDescriptor == NULL && args->structureOutput != NULL) {
		bzero(args->structureOutput, args->structureOutputSize);
	}
	if (selector >= 64 || !(sweet_fb_logged & (1ULL << selector))) {
		if (selector < 64) {
			sweet_fb_logged |= 1ULL << selector;
		}
		char name[32];
		proc_selfname(name, sizeof(name));
		IOLog("SWEET FB: %s sel %u in %u/%u out %u/%u -> 0x%x\n", name, selector,
		    args->scalarInputCount, args->structureInputSize,
		    args->scalarOutputCount, args->structureOutputSize, ret);
	}
	return ret;
}
#endif /* SWEET */
