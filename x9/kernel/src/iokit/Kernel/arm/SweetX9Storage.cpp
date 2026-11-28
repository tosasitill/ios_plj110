/*
 * SweetX9Storage.cpp -- APFS P0: IOKit storage publication for the X9
 * extent root disk.
 *
 * The extent block layer (bsd/dev/sweet_x9_ufs.c) presents the root
 * window as the BSD block device ufsroot0; that path is what the
 * kernel boots from (rd=ufsroot0, built-in HFS) and it is not touched
 * by this file.  SWEET historically had no IOStorageFamily and
 * therefore no IOMedia objects, which is the one structural blocker
 * for APFS: the APFS container scheme matches on IOMedia.
 *
 * This file publishes the same extent window as an IOBlockStorageDevice
 * so that the prelinked IOStorageFamily (a prelink root since P0, see
 * x9/build/scripts/build-usb-prelink.py) can attach IOBlockStorageDriver
 * and generate an IOMedia for it.  Publication is gated by the boot
 * argument x9_iomedia=1 (default off, decided in sweet_x9_ufs_rootdev);
 * with the gate off this code never runs, registers nothing and
 * references no family symbols, so the HFS boot path is unchanged.
 *
 * Why the class below is not a C++ subclass of IOBlockStorageDevice:
 *
 *  - The kernel link is self-contained and XnuPrelink only ever binds
 *    kext -> kernel references (native_prelink.plan rebases addresses
 *    inside the selected kext segments).  A C++ subclass would leave
 *    the base class's metaclass, constructor and non-pure virtuals
 *    undefined at kernel link time; ld correctly refuses them.
 *  - Defining a same-named local base class is not an option either:
 *    OSMetaClass registration panics on a duplicate class name in the
 *    non-OSX configuration (libkern/c++/OSMetaClass.cpp), and the
 *    family's OSDynamicCast uses pointer-form metaCast against its
 *    own gMetaClass, which a local metaclass can never satisfy.
 *  - The family module in the 20H392 kernelcache is fully stripped
 *    (LC_SYMTAB with nsyms=0), so there is no link-time binding data
 *    to recover even in principle.
 *
 * The binding used here is therefore runtime and ABI-level, the same
 * level IOKit itself uses across kext boundaries:
 *
 *  - The class derives from IOService in C++ but declares the exact
 *    virtual table of IOBlockStorageDevice, in the family's order,
 *    taken from the vendored iOS-normalized header in IOKit/storage/
 *    next to this file (the four TARGET_OS_OSX-only virtuals are the
 *    macOS layout and are excluded there).  Every slot is implemented
 *    locally: the pure virtuals with the real extent behaviour, the
 *    family's own defaults with their stock semantics.
 *  - getMetaClass() returns the family's registered IOBlockStorageDevice
 *    metaclass, looked up by name (OSMetaClass::getMetaClassWithName)
 *    at publish time.  OSObject's constructor ignores its metaclass
 *    argument and identity in IOKit is exactly this virtual, so
 *    catalogue matching (name walk) and the driver's OSDynamicCast
 *    (pointer walk) both see a genuine IOBlockStorageDevice, while no
 *    class is ever registered by this file and no family symbol is
 *    referenced at link time.
 *
 * I/O goes through the C accessors in sweet_x9_ufs.c, which share the
 * strategy path's transport mutex, ready/failed gating and write
 * contract.  Requests are executed synchronously through a bounce
 * buffer and completed inline (the IOStorage completion
 * helper is a family symbol;
 * its entire body is the forwarder open-coded below).  The block size
 * published to the family is 4096, the extent/physical granularity
 * (xu.bytes is 4096-aligned), not the BSD layer's 512-byte sector.
 */

#include <IOKit/storage/IOBlockStorageDevice.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSKext.h>

extern "C" {
int sweet_x9_media_info(uint64_t *bytes, int *writable);
int sweet_x9_media_read(uint64_t offset, void *data, size_t length);
int sweet_x9_media_write(uint64_t offset, const void *data, size_t length);
int sweet_x9_media_sync(void);
}

#define X9_MEDIA_BLOCK_SIZE 4096ULL
#define X9_MEDIA_BOUNCE_BYTES (128ULL * 1024ULL)

/* The family's IOBlockStorageDevice metaclass, bound at publish time.
 * Also what getMetaClass() reports, so it must be set before any
 * instance is created. */
static const OSMetaClass *sweet_storage_family_meta;

class SweetX9BlockStorageDevice : public IOService
{
public:
	SweetX9BlockStorageDevice(const OSMetaClass *meta);
	virtual ~SweetX9BlockStorageDevice();
	virtual const OSMetaClass *getMetaClass() const APPLE_KEXT_OVERRIDE;

	/* Overrides from IORegistryEntry/IOService -- same slots, in the
	 * same order, as in IOBlockStorageDevice. */
	virtual bool init(OSDictionary *properties) APPLE_KEXT_OVERRIDE;
	virtual OSObject *getProperty(const OSSymbol *key) const
	    APPLE_KEXT_OVERRIDE;
	virtual IOReturn setProperties(OSObject *properties)
	    APPLE_KEXT_OVERRIDE;

	/* IOBlockStorageDevice virtuals, in the family's declaration
	 * order (see the vendored header). */
	virtual IOReturn doEjectMedia(void);
	virtual IOReturn doFormatMedia(UInt64 byteCapacity);
	virtual UInt32 doGetFormatCapacities(UInt64 *capacities,
	    UInt32 capacitiesMaxCount) const;
	virtual IOReturn doSynchronizeCache(void);
	virtual char *getVendorString(void);
	virtual char *getProductString(void);
	virtual char *getRevisionString(void);
	virtual char *getAdditionalDeviceInfoString(void);
	virtual IOReturn reportBlockSize(UInt64 *blockSize);
	virtual IOReturn reportEjectability(bool *isEjectable);
	virtual IOReturn reportMaxValidBlock(UInt64 *maxBlock);
	virtual IOReturn reportMediaState(bool *mediaPresent,
	    bool *changedState = 0);
	virtual IOReturn reportRemovability(bool *isRemovable);
	virtual IOReturn reportWriteProtection(bool *isWriteProtected);
	virtual IOReturn getWriteCacheState(bool *enabled);
	virtual IOReturn setWriteCacheState(bool enabled);
	virtual IOReturn doAsyncReadWrite(IOMemoryDescriptor *buffer,
	    UInt64 block, UInt64 nblks, IOStorageAttributes *attributes,
	    IOStorageCompletion *completion);
	virtual IOReturn requestIdle(void);
	virtual IOReturn doUnmap(IOBlockStorageDeviceExtent *extents,
	    UInt32 extentsCount, IOStorageUnmapOptions options = 0);
	virtual IOReturn doSetPriority(IOBlockStorageDeviceExtent *extents,
	    UInt32 extentsCount, IOStoragePriority priority);
	virtual IOReturn doSynchronize(UInt64 block, UInt64 nblks,
	    IOStorageSynchronizeOptions options = 0);
	virtual IOReturn doGetProvisionStatus(UInt64 block, UInt64 nblks,
	    UInt32 *extentsCount,
	    IOBlockStorageProvisionDeviceExtent *extents,
	    IOStorageGetProvisionStatusOptions options = 0);

	OSMetaClassDeclareReservedUsed(SweetX9BlockStorageDevice, 0);
	OSMetaClassDeclareReservedUsed(SweetX9BlockStorageDevice, 1);
	OSMetaClassDeclareReservedUsed(SweetX9BlockStorageDevice, 2);
	OSMetaClassDeclareReservedUsed(SweetX9BlockStorageDevice, 3);
	OSMetaClassDeclareReservedUnused(SweetX9BlockStorageDevice, 4);
	OSMetaClassDeclareReservedUnused(SweetX9BlockStorageDevice, 5);
	OSMetaClassDeclareReservedUnused(SweetX9BlockStorageDevice, 6);
	OSMetaClassDeclareReservedUnused(SweetX9BlockStorageDevice, 7);
	OSMetaClassDeclareReservedUnused(SweetX9BlockStorageDevice, 8);
	OSMetaClassDeclareReservedUnused(SweetX9BlockStorageDevice, 9);
	OSMetaClassDeclareReservedUnused(SweetX9BlockStorageDevice, 10);
	OSMetaClassDeclareReservedUnused(SweetX9BlockStorageDevice, 11);
	OSMetaClassDeclareReservedUnused(SweetX9BlockStorageDevice, 12);
	OSMetaClassDeclareReservedUnused(SweetX9BlockStorageDevice, 13);
	OSMetaClassDeclareReservedUnused(SweetX9BlockStorageDevice, 14);
	OSMetaClassDeclareReservedUnused(SweetX9BlockStorageDevice, 15);
	OSMetaClassDeclareReservedUnused(SweetX9BlockStorageDevice, 16);
	OSMetaClassDeclareReservedUnused(SweetX9BlockStorageDevice, 17);
	OSMetaClassDeclareReservedUnused(SweetX9BlockStorageDevice, 18);
	OSMetaClassDeclareReservedUnused(SweetX9BlockStorageDevice, 19);
	OSMetaClassDeclareReservedUnused(SweetX9BlockStorageDevice, 20);
	OSMetaClassDeclareReservedUnused(SweetX9BlockStorageDevice, 21);
	OSMetaClassDeclareReservedUnused(SweetX9BlockStorageDevice, 22);
	OSMetaClassDeclareReservedUnused(SweetX9BlockStorageDevice, 23);
	OSMetaClassDeclareReservedUnused(SweetX9BlockStorageDevice, 24);
	OSMetaClassDeclareReservedUnused(SweetX9BlockStorageDevice, 25);
	OSMetaClassDeclareReservedUnused(SweetX9BlockStorageDevice, 26);
	OSMetaClassDeclareReservedUnused(SweetX9BlockStorageDevice, 27);
	OSMetaClassDeclareReservedUnused(SweetX9BlockStorageDevice, 28);
	OSMetaClassDeclareReservedUnused(SweetX9BlockStorageDevice, 29);
	OSMetaClassDeclareReservedUnused(SweetX9BlockStorageDevice, 30);
	OSMetaClassDeclareReservedUnused(SweetX9BlockStorageDevice, 31);

protected:
	/* Mirrors IOBlockStorageDevice's single instance variable, so
	 * the instance layout matches as well.  Nothing outside this
	 * class reads it (the family's own methods never run here). */
	struct ExpansionData { /* */ };
	ExpansionData *_expansionData;

private:
	IOReturn mediaInfo(UInt64 *bytes, bool *writable) const;
};

OSMetaClassDefineReservedUsed(SweetX9BlockStorageDevice, 0);
OSMetaClassDefineReservedUsed(SweetX9BlockStorageDevice, 1);
OSMetaClassDefineReservedUsed(SweetX9BlockStorageDevice, 2);
OSMetaClassDefineReservedUsed(SweetX9BlockStorageDevice, 3);
OSMetaClassDefineReservedUnused(SweetX9BlockStorageDevice, 4);
OSMetaClassDefineReservedUnused(SweetX9BlockStorageDevice, 5);
OSMetaClassDefineReservedUnused(SweetX9BlockStorageDevice, 6);
OSMetaClassDefineReservedUnused(SweetX9BlockStorageDevice, 7);
OSMetaClassDefineReservedUnused(SweetX9BlockStorageDevice, 8);
OSMetaClassDefineReservedUnused(SweetX9BlockStorageDevice, 9);
OSMetaClassDefineReservedUnused(SweetX9BlockStorageDevice, 10);
OSMetaClassDefineReservedUnused(SweetX9BlockStorageDevice, 11);
OSMetaClassDefineReservedUnused(SweetX9BlockStorageDevice, 12);
OSMetaClassDefineReservedUnused(SweetX9BlockStorageDevice, 13);
OSMetaClassDefineReservedUnused(SweetX9BlockStorageDevice, 14);
OSMetaClassDefineReservedUnused(SweetX9BlockStorageDevice, 15);
OSMetaClassDefineReservedUnused(SweetX9BlockStorageDevice, 16);
OSMetaClassDefineReservedUnused(SweetX9BlockStorageDevice, 17);
OSMetaClassDefineReservedUnused(SweetX9BlockStorageDevice, 18);
OSMetaClassDefineReservedUnused(SweetX9BlockStorageDevice, 19);
OSMetaClassDefineReservedUnused(SweetX9BlockStorageDevice, 20);
OSMetaClassDefineReservedUnused(SweetX9BlockStorageDevice, 21);
OSMetaClassDefineReservedUnused(SweetX9BlockStorageDevice, 22);
OSMetaClassDefineReservedUnused(SweetX9BlockStorageDevice, 23);
OSMetaClassDefineReservedUnused(SweetX9BlockStorageDevice, 24);
OSMetaClassDefineReservedUnused(SweetX9BlockStorageDevice, 25);
OSMetaClassDefineReservedUnused(SweetX9BlockStorageDevice, 26);
OSMetaClassDefineReservedUnused(SweetX9BlockStorageDevice, 27);
OSMetaClassDefineReservedUnused(SweetX9BlockStorageDevice, 28);
OSMetaClassDefineReservedUnused(SweetX9BlockStorageDevice, 29);
OSMetaClassDefineReservedUnused(SweetX9BlockStorageDevice, 30);
OSMetaClassDefineReservedUnused(SweetX9BlockStorageDevice, 31);

SweetX9BlockStorageDevice::SweetX9BlockStorageDevice(
    const OSMetaClass *meta)
	: IOService(meta), _expansionData(0)
{
}

SweetX9BlockStorageDevice::~SweetX9BlockStorageDevice()
{
}

const OSMetaClass *
SweetX9BlockStorageDevice::getMetaClass() const
{
	return sweet_storage_family_meta;
}

bool
SweetX9BlockStorageDevice::init(OSDictionary *properties)
{
	if (!IOService::init(properties))
		return false;
	/* Replicates IOBlockStorageDevice::init's one job: publish the
	 * device-type property the generic IOBlockStorageDriver
	 * personality matches on (IOPropertyMatch device-type=Generic). */
	setProperty(kIOBlockStorageDeviceTypeKey,
	    kIOBlockStorageDeviceTypeGeneric);
	return true;
}

OSObject *
SweetX9BlockStorageDevice::getProperty(const OSSymbol *key) const
{
	return IORegistryEntry::getProperty(key);
}

IOReturn
SweetX9BlockStorageDevice::setProperties(OSObject *properties)
{
	return IOService::setProperties(properties);
}

IOReturn
SweetX9BlockStorageDevice::mediaInfo(UInt64 *bytes, bool *writable) const
{
	uint64_t b = 0;
	int w = 0;

	if (sweet_x9_media_info(&b, &w) != 0)
		return kIOReturnNotAttached;
	if (bytes)
		*bytes = b;
	if (writable)
		*writable = (w != 0);
	return kIOReturnSuccess;
}

IOReturn
SweetX9BlockStorageDevice::doEjectMedia(void)
{
	/* The media is the root window file inside userdata; it cannot
	 * be ejected.  Stock answer for fixed internal media. */
	return kIOReturnUnsupported;
}

IOReturn
SweetX9BlockStorageDevice::doFormatMedia(UInt64 byteCapacity)
{
	(void)byteCapacity;
	return kIOReturnUnsupported;
}

UInt32
SweetX9BlockStorageDevice::doGetFormatCapacities(UInt64 *capacities,
    UInt32 capacitiesMaxCount) const
{
	UInt64 bytes = 0;

	if (mediaInfo(&bytes, NULL) != kIOReturnSuccess)
		return 0;
	if (capacities && capacitiesMaxCount > 0)
		capacities[0] = bytes;
	return 1;
}

IOReturn
SweetX9BlockStorageDevice::doSynchronizeCache(void)
{
	return (sweet_x9_media_sync() == 0) ? kIOReturnSuccess :
	    kIOReturnIOError;
}

char *
SweetX9BlockStorageDevice::getVendorString(void)
{
	return (char *)"X9";
}

char *
SweetX9BlockStorageDevice::getProductString(void)
{
	/* Cosmetic (ioreg / IOMedia properties): the "device" is a file
	 * window over the userdata partition, not a physical drive. */
	return (char *)"Extent Root Window";
}

char *
SweetX9BlockStorageDevice::getRevisionString(void)
{
	return (char *)"1.0";
}

char *
SweetX9BlockStorageDevice::getAdditionalDeviceInfoString(void)
{
	return (char *)"ufsroot0 extent window";
}

IOReturn
SweetX9BlockStorageDevice::reportBlockSize(UInt64 *blockSize)
{
	/* Task contract and physical truth: publish the 4096-byte extent
	 * granularity, not the BSD layer's 512-byte logical sector. */
	*blockSize = X9_MEDIA_BLOCK_SIZE;
	return kIOReturnSuccess;
}

IOReturn
SweetX9BlockStorageDevice::reportEjectability(bool *isEjectable)
{
	*isEjectable = false;
	return kIOReturnSuccess;
}

IOReturn
SweetX9BlockStorageDevice::reportMaxValidBlock(UInt64 *maxBlock)
{
	UInt64 bytes = 0;
	IOReturn status = mediaInfo(&bytes, NULL);

	if (status != kIOReturnSuccess)
		return status;
	if (bytes < X9_MEDIA_BLOCK_SIZE)
		return kIOReturnNotAttached;
	*maxBlock = bytes / X9_MEDIA_BLOCK_SIZE - 1;
	return kIOReturnSuccess;
}

IOReturn
SweetX9BlockStorageDevice::reportMediaState(bool *mediaPresent,
    bool *changedState)
{
	/* Present exactly while the extent root is live (the same
	 * ready && !failed condition the BSD bopen uses).  The window
	 * never appears or disappears at runtime, so no change events. */
	*mediaPresent = (mediaInfo(NULL, NULL) == kIOReturnSuccess);
	if (changedState)
		*changedState = false;
	return kIOReturnSuccess;
}

IOReturn
SweetX9BlockStorageDevice::reportRemovability(bool *isRemovable)
{
	*isRemovable = false;
	return kIOReturnSuccess;
}

IOReturn
SweetX9BlockStorageDevice::reportWriteProtection(bool *isWriteProtected)
{
	bool writable = false;
	IOReturn status = mediaInfo(NULL, &writable);

	if (status != kIOReturnSuccess)
		return status;
	/* Mirrors the BSD write contract (boot-arg ufsrw + ADT contract
	 * + protected extents), as captured by xu.writable. */
	*isWriteProtected = !writable;
	return kIOReturnSuccess;
}

IOReturn
SweetX9BlockStorageDevice::getWriteCacheState(bool *enabled)
{
	(void)enabled;
	/* The extent transport has no device write cache to report;
	 * this is the base class's stock answer. */
	return kIOReturnUnsupported;
}

IOReturn
SweetX9BlockStorageDevice::setWriteCacheState(bool enabled)
{
	(void)enabled;
	return kIOReturnUnsupported;
}

/* The IOStorage completion helper, open-coded: it is a family
 * symbol and its
 * entire body is this forwarder. */
static void
sweet_storage_complete(IOStorageCompletion *completion, IOReturn status,
    UInt64 actualByteCount)
{
	if (completion && completion->action)
		completion->action(completion->target, completion->parameter,
		    status, actualByteCount);
}

IOReturn
SweetX9BlockStorageDevice::doAsyncReadWrite(IOMemoryDescriptor *buffer,
    UInt64 block, UInt64 nblks, IOStorageAttributes *attributes,
    IOStorageCompletion *completion)
{
	(void)attributes;	/* adjustedOffset is consumed by
				 * IOBlockStorageDriver before the device
				 * is called; blocks arrive in device
				 * block units. */
	UInt64 offset = block * X9_MEDIA_BLOCK_SIZE;
	UInt64 total = nblks * X9_MEDIA_BLOCK_SIZE;
	UInt64 done = 0;
	IOReturn status = kIOReturnSuccess;
	/* The descriptor carries the direction: kIODirectionIn is a
	 * device read (data flows into the buffer). */
	bool reading = (buffer->getDirection() == kIODirectionIn);
	uint8_t *bounce = NULL;

	if (total > 0)
		bounce = (uint8_t *)IOMalloc((size_t)X9_MEDIA_BOUNCE_BYTES);
	if (total > 0 && bounce == NULL)
		status = kIOReturnNoMemory;

	while (status == kIOReturnSuccess && done < total) {
		UInt64 chunk = total - done;
		if (chunk > X9_MEDIA_BOUNCE_BYTES)
			chunk = X9_MEDIA_BOUNCE_BYTES;
		if (reading) {
			if (sweet_x9_media_read(offset + done, bounce,
			    (size_t)chunk) != 0) {
				status = kIOReturnIOError;
				break;
			}
			if (buffer->writeBytes(done, bounce,
			    (size_t)chunk) != chunk) {
				status = kIOReturnIOError;
				break;
			}
		} else {
			if (buffer->readBytes(done, bounce,
			    (size_t)chunk) != chunk) {
				status = kIOReturnIOError;
				break;
			}
			if (sweet_x9_media_write(offset + done, bounce,
			    (size_t)chunk) != 0) {
				status = kIOReturnIOError;
				break;
			}
		}
		done += chunk;
	}

	if (bounce)
		IOFree(bounce, (size_t)X9_MEDIA_BOUNCE_BYTES);
	sweet_storage_complete(completion, status, done);
	return kIOReturnSuccess;
}

IOReturn
SweetX9BlockStorageDevice::requestIdle(void)
{
	return kIOReturnSuccess;
}

IOReturn
SweetX9BlockStorageDevice::doUnmap(IOBlockStorageDeviceExtent *extents,
    UInt32 extentsCount, IOStorageUnmapOptions options)
{
	(void)extents;
	(void)extentsCount;
	(void)options;
	/* The window is fully backed by the userdata file; there is no
	 * deallocation to offer.  Stock base-class answer. */
	return kIOReturnUnsupported;
}

IOReturn
SweetX9BlockStorageDevice::doSetPriority(
    IOBlockStorageDeviceExtent *extents, UInt32 extentsCount,
    IOStoragePriority priority)
{
	(void)extents;
	(void)extentsCount;
	(void)priority;
	return kIOReturnUnsupported;
}

IOReturn
SweetX9BlockStorageDevice::doSynchronize(UInt64 block, UInt64 nblks,
    IOStorageSynchronizeOptions options)
{
	(void)block;
	(void)nblks;
	(void)options;
	/* The extent transport has a single flush for the whole window
	 * (x9u_sync_image), matching the BSD DKIOCSYNCHRONIZECACHE path. */
	return doSynchronizeCache();
}

IOReturn
SweetX9BlockStorageDevice::doGetProvisionStatus(UInt64 block,
    UInt64 nblks, UInt32 *extentsCount,
    IOBlockStorageProvisionDeviceExtent *extents,
    IOStorageGetProvisionStatusOptions options)
{
	(void)block;
	(void)nblks;
	(void)extents;
	(void)options;
	if (extentsCount)
		*extentsCount = 0;
	return kIOReturnUnsupported;
}

/* The family's IOBlockStorageDevice metaclass, looked up by name;
 * NULL while IOStorageFamily has not been loaded. */
static const OSMetaClass *
x9_storage_family_meta_by_name(void)
{
	const OSSymbol *name = OSSymbol::withCString("IOBlockStorageDevice");
	const OSMetaClass *family = name ?
	    OSMetaClass::getMetaClassWithName(name) : NULL;
	if (name)
		name->release();
	return family;
}

static bool x9_storage_published;

extern "C" void
sweet_x9_storage_publish(void)
{
	uint64_t bytes = 0;
	int writable = 0;

	if (x9_storage_published)
		return;
	x9_storage_published = true;

	if (sweet_x9_media_info(&bytes, &writable) != 0 || bytes == 0) {
		IOLog("X9 IOMEDIA: extent media not ready; not publishing\n");
		return;
	}

	/* Bind to the family's registered metaclass by name.  If the
	 * family is not in this kernel image, publish nothing: the BSD
	 * root is already up and must not be disturbed. */
	const OSMetaClass *family = x9_storage_family_meta_by_name();
	if (!family) {
		/* The prelinked family's OSKext object is created during
		 * InitIOKit (KLDBootstrap::readPrelinkedExtensions), but
		 * creating the object does not load it, and in this port
		 * nothing ever requests the load: every IOStorageFamily
		 * personality matches on an IOMedia/IOBlockStorageDevice
		 * provider, no such nub exists before this publish (the
		 * root storage is a BSD extent device, not an IOKit nub),
		 * and checking for the metaclass first would wait forever
		 * -- the check-then-publish order deadlocks.  Stock
		 * systems break the cycle with a real storage controller
		 * nub; here the load must be requested explicitly.
		 * allowDefer=false keeps the request in-kernel: a
		 * prelinked kext loads synchronously, and a missing OSKext
		 * object fails at once instead of deferring to a kextd
		 * request that is never answered.  The log line separates
		 * the two failure modes on the next boot: object absent =
		 * the prelink creation path never ran; object present but
		 * metaclass still missing after a successful load = the
		 * family's own structors did not register it. */
		OSKext *kext = OSKext::lookupKextWithIdentifier(
		    "com.apple.iokit.IOStorageFamily");  // must release
		OSReturn loadResult = OSKext::loadKextWithIdentifier(
		    "com.apple.iokit.IOStorageFamily", false);
		IOLog("X9 IOMEDIA: IOStorageFamily kext object %s; "
		    "explicit load result 0x%x\n",
		    kext ? "present" : "absent", (unsigned)loadResult);
		if (kext)
			kext->release();
		family = x9_storage_family_meta_by_name();
	}
	if (!family) {
		IOLog("X9 IOMEDIA: IOBlockStorageDevice metaclass not "
		    "registered (IOStorageFamily absent?); not publishing\n");
		return;
	}
	sweet_storage_family_meta = family;

	SweetX9BlockStorageDevice *device =
	    new SweetX9BlockStorageDevice(family);
	IOService *root = IOService::getServiceRoot();
	if (!device || !root || !device->init(0) || !device->attach(root) ||
	    !device->start(root)) {
		if (device) {
			if (device->getProvider() == root)
				device->detach(root);
			device->release();
		}
		IOLog("X9 IOMEDIA: publish failed\n");
		return;
	}
	device->setName("X9ExtentRoot");
	device->setProperty("x9-extent-media", true);
	device->registerService(kIOServiceSynchronous);
	/* The device object is intentionally never released: it lives
	 * for the life of the kernel, like the BSD device it mirrors. */
	IOLog("X9 IOMEDIA: published IOBlockStorageDevice X9ExtentRoot "
	    "bytes=%llu writable=%d; IOBlockStorageDriver/IOMedia "
	    "should attach above it\n",
	    (unsigned long long)bytes, writable);
}
