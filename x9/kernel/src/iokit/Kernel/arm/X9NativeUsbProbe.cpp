/*
 * X9 native Apple USB stack probe (UniXNU UsbProbe-style staging).
 *
 * The kernel is built with the iOS kext C++ ABI and XnuPrelink keeps the
 * original 20H392 AppleUSBCommon, IOUSBDeviceFamily and AppleUSBDeviceMux in
 * __PRELINK_TEXT. This probe only runs when those prelinked kexts exist. After
 * the IOBSD resource is published it loads them through OSKext (no linking:
 * they are prelinked), then logs whether their classes registered, so one boot
 * shows the retained stack is alive before an MTU3 controller is attached.
 * It does not create a controller, touch MTU3 or change the USB mode.
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSKext.h>
#include <libkern/c++/OSMetaClass.h>
#include <kern/thread.h>

extern "C" int sweet_early_is_x9(void);

static const char *const kX9NativeKexts[] = {
	"com.apple.driver.usb.AppleUSBCommon",
	"com.apple.iokit.IOUSBDeviceFamily",
	"com.apple.driver.AppleUSBDeviceMux",
};
static const char *const kX9NativeClasses[] = {
	"IOUSBDeviceController", "IOUSBDevice", "IOUSBDeviceInterface",
	"IOUSBDeviceInterfaceUserClient", "AppleUSBDeviceMux",
};

static void
x9_native_usb_probe_thread(void *, wait_result_t)
{
	OSDictionary *matching = IOService::resourceMatching("IOBSD");
	IOService *bsd = matching ? IOService::waitForMatchingService(matching, 120ULL * kSecondScale) : NULL;
	OSSafeReleaseNULL(matching);
	OSSafeReleaseNULL(bsd);
	for (const char *id : kX9NativeKexts) {
		OSReturn r = OSKext::loadKextWithIdentifier(id, false /* allowDefer */);
		IOLog("X9 NATIVE USB: load %s -> 0x%x\n", id, r);
	}
	for (const char *name : kX9NativeClasses) {
		const OSSymbol *sym = OSSymbol::withCString(name);
		const OSMetaClass *meta = sym ? OSMetaClass::getMetaClassWithName(sym) : NULL;
		IOLog("X9 NATIVE USB: class %s %s size=%u super=%s\n", name, meta ? "registered" : "missing",
		    meta ? meta->getClassSize() : 0,
		    meta && meta->getSuperClass() ? meta->getSuperClass()->getClassName() : "-");
		OSSafeReleaseNULL(sym);
	}
	IOLog("X9 NATIVE USB: probe done (no controller attached)\n");
	thread_terminate(current_thread());
}

extern "C" void
x9_native_usb_probe_schedule(void)
{
	if (!sweet_early_is_x9()) {
		return;
	}
	OSKext *family = OSKext::lookupKextWithIdentifier("com.apple.iokit.IOUSBDeviceFamily"); /* retained */
	if (!family) {
		return; /* ordinary kernel without the XnuPrelink USB payload */
	}
	family->release();
	IOLog("X9 NATIVE USB: prelinked IOUSBDeviceFamily present; probe scheduled\n");
	thread_t thread = THREAD_NULL;
	if (kernel_thread_start(x9_native_usb_probe_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
