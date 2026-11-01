/*
 * Minimal device-tree platform expert for Redmi Note 10 Pro (sweet).
 * Uses XNU's normal platform, power-domain and nub initialization. Hardware
 * interrupt and power-management services are supplied by separate drivers.
 */
#include <pexpert/arm64/board_config.h>

#if defined(SWEET)
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSString.h>

extern "C" int sweet_early_is_x9(void);
#include <arm64/sweet_early.h>

class SweetPlatformExpert : public IODTPlatformExpert {
	OSDeclareDefaultStructors(SweetPlatformExpert);

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	OSString *createSystemSerialNumberString(OSData *property) APPLE_KEXT_OVERRIDE;
	void processTopLevel(IORegistryEntry *root) APPLE_KEXT_OVERRIDE;
	const char *deleteList(void) APPLE_KEXT_OVERRIDE;
	const char *excludeList(void) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(SweetPlatformExpert, IODTPlatformExpert);

bool
SweetPlatformExpert::start(IOService *provider)
{
	IOLog("SWEET: starting device-tree platform expert\n");
	if (!IODTPlatformExpert::start(provider)) {
		return false;
	}
	registerService();
	/* dali0210: publish readiness; inherited software-time fallback remains. */
	if(sweet_early_is_x9())publishResource("IORTC",this);
	if (sweet_early_is_x9()) {
		char product[64] = {}, board[64] = {};
		getProductName(product, sizeof(product));
		getTargetName(board, sizeof(board));
		IORegistryEntry *entry = IORegistryEntry::fromPath("IODeviceTree:/product");
		OSData *description = entry ? OSDynamicCast(OSData, entry->getProperty("product-description")) : NULL;
		OSData *model = OSDynamicCast(OSData, provider->getProperty("model-number"));
		IOLog("X9 IDENTITY: product=%s board=%s hardware=MT6993; description=%.*s model=%.*s\n",
		    product, board, description ? (int)description->getLength() : 0,
		    description ? (const char *)description->getBytesNoCopy() : "",
		    model ? (int)model->getLength() : 0, model ? (const char *)model->getBytesNoCopy() : "");
		OSSafeReleaseNULL(entry);
	}
	IOLog("SWEET: platform expert registered\n");
	return true;
}

/* X9 serial-number is the iBoot-style ASCII string. The inherited Mac
 * decoder rearranges fixed-position fields and cannot preserve a custom serial. */
OSString *
SweetPlatformExpert::createSystemSerialNumberString(OSData *property)
{
    if (!sweet_early_is_x9()) {
        return IODTPlatformExpert::createSystemSerialNumberString(property);
    }
    if (!property || !property->getLength() || property->getLength() > 32) {
        return NULL;
    }
    const char *bytes = (const char *)property->getBytesNoCopy();
    unsigned length = 0;
    while (length < property->getLength() && bytes[length]) {
        length++;
    }
    if (!length || length >= 32) {
        return NULL;
    }
    char value[32];
    for (unsigned i = 0; i < length; i++) {
        value[i] = bytes[i];
    }
    value[length] = 0;
    OSString *serial = OSString::withCString(value);
    if (serial) {
        IOLog("X9 IDENTITY: IOPlatformSerialNumber=%s\n", value);
    }
    return serial;
}

const char *
SweetPlatformExpert::deleteList(void)
{
	return "()";
}

const char *
SweetPlatformExpert::excludeList(void)
{
	return "('chosen', 'defaults', 'cpus', 'options')";
}

void
SweetPlatformExpert::processTopLevel(IORegistryEntry *root)
{
	/*
	 * The stock implementation requires an Apple NVRAM /options node.
	 * SWEET has no NVRAM controller; publish real CPU/device nodes without
	 * inventing one. createNubs adds the topology's logical CPU/cluster IDs.
	 */
	IORegistryEntry *cpus = root->childFromPath("cpus", gIODTPlane);
	if (cpus != NULL) {
		createNubs(this, IODTFindMatchingEntries(cpus, kIODTExclusive, NULL));
		cpus->release();
	}
	createNubs(this, IODTFindMatchingEntries(root, kIODTExclusive, excludeList()));

	/*
	 * No AppleARMIO driver publishes the /arm-io children: do it here so
	 * built-in drivers (the display, AppleCLCD) can match them. The GIC is
	 * driven by the platform code and gets no nub. The nubs hang off this
	 * platform expert: IOPlatformDevice::compareName asks its provider.
	 */
	IORegistryEntry *armio = root->childFromPath("arm-io", gIODTPlane);
	if (armio != NULL) {
		createNubs(this, IODTFindMatchingEntries(armio, kIODTExclusive, "('gic')"));
		armio->release();
	}
}

/*
 * hw.target / hw.product come from IODTPlatformExpert: the first root
 * "compatible" string and "model" (D22AP / iPhone10,3 in the ADT), which
 * is what the iPhone userland (MobileGestalt) expects.
 */
#endif /* SWEET */
