/*
 * Built-in AppleKeyStore for SWEET: a device without a passcode whose
 * system keybag is always loaded and unlocked.
 *
 * Apple's AppleKeyStore kext (closed, SEP-backed) owns the keybags behind
 * data protection. libaks (AppleKeyStore.framework, statically in keybagd)
 * opens IOServiceMatching("AppleKeyStore") with type 0, calls selector 0,
 * then one selector per aks_* function (iOS 16.7 numbering below). keybagd
 * --init asks for the system bag (14) and only loads systembag.kb when the
 * answer is kIOReturnNotFound; answering with a handle is the path a
 * userspace reboot takes, so no bag file is needed.
 *
 * Every call is logged (selector, caller, argument shapes, result) so the
 * next selectors the system needs show up on the console; unknown ones fail
 * with kIOReturnUnsupported.
 */
#include <pexpert/arm64/board_config.h>

#if defined(SWEET)
#include <IOKit/IOService.h>
#include <arm64/sweet_early.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSString.h>
#include <sys/proc.h>
#include <libkern/OSAtomic.h>

/* libaks selectors (AppleKeyStore.framework, iOS 16.7). */
enum {
	kAKSOpen             = 0,
	kAKSCreateBag        = 2,
	kAKSSaveBag          = 3,
	kAKSUnloadBag        = 4,
	kAKSSetSystem        = 5,
	kAKSLoadBag          = 6,
	kAKSGetLockState     = 7,
	kAKSUnlockDevice     = 9,
	kAKSWrapKey          = 10,
	kAKSUnwrapKey        = 11,
	kAKSUnlockBag        = 12,
	kAKSLockBag          = 13,
	kAKSGetSystem        = 14,
	kAKSGetDeviceState   = 17,
	kAKSGetConfiguration = 31,
	kAKSGetExtDeviceState = 35,     /* aks_get_extended_device_state */
	kAKSAssertHold       = 26,
	kAKSAssertDrop       = 27,
	kAKSUnloadSessionBags = 55,
	kAKSNotifyStateSet   = 102,
	kAKSNotifyStateIsSet = 103,
	kAKSAssertConsume    = 101,     /* aks_assert_consume */
};

/* The system (device) bag handle. */
#define SWEET_AKS_SYSTEM_HANDLE 0

/*
 * aks lock state bits (aks_get_lock_state): bit 2 is "unlocked since boot"
 * (MobileKeyBag MKBDeviceUnlockedSinceBoot); clear lock bits = unlocked.
 */
#define SWEET_AKS_LOCK_STATE    0x4

class AppleKeyStore : public IOService {
	OSDeclareDefaultStructors(AppleKeyStore);

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleKeyStore, IOService);

bool
AppleKeyStore::start(IOService *provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "AppleKeyStoreUserClient");
	registerService();
	IOLog("SWEET AKS: AppleKeyStore registered (no passcode, unlocked)\n");
	return true;
}

class AppleKeyStoreUserClient : public IOUserClient {
	OSDeclareDefaultStructors(AppleKeyStoreUserClient);

public:
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target,
	    void *reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleKeyStoreUserClient, IOUserClient);

IOReturn
AppleKeyStoreUserClient::clientClose(void)
{
	terminate();
	return kIOReturnSuccess;
}

/* Copy len bytes to the caller's output structure (inband or descriptor). */
static IOReturn
sweet_aks_struct_out(IOExternalMethodArguments *args, const void *buf, uint32_t len)
{
	if (args->structureOutputDescriptor != NULL) {
		if (len > args->structureOutputDescriptor->getLength()) {
			return kIOReturnNoSpace;
		}
		IOMemoryDescriptor *out = args->structureOutputDescriptor;
		IOReturn result = out->prepare();
		if (result != kIOReturnSuccess) return result;
		IOByteCount written = out->writeBytes(0, buf, len);
		IOReturn completed = out->complete();
		if (written != len) return kIOReturnUnderrun;
		if (completed != kIOReturnSuccess) return completed;
		args->structureOutputDescriptorSize = len;
		return kIOReturnSuccess;
	}
	if (args->structureOutput == NULL || len > args->structureOutputSize) {
		return kIOReturnNoSpace;
	}
	memcpy(args->structureOutput, buf, len);
	args->structureOutputSize = len;
	return kIOReturnSuccess;
}

/* Append SEQUENCE { UTF8String key, INTEGER value } (short forms only). */
static uint32_t
sweet_aks_der_pair(uint8_t *p, const char *key, uint8_t value)
{
	uint32_t klen = (uint32_t)strlen(key);
	uint32_t n = 0;
	p[n++] = 0x30;
	p[n++] = (uint8_t)(2 + klen + 3);
	p[n++] = 0x0c;
	p[n++] = (uint8_t)klen;
	memcpy(p + n, key, klen);
	n += klen;
	p[n++] = 0x02;
	p[n++] = 0x01;
	p[n++] = value;
	return n;
}

/*
 * Device state (selector 17): a DER SET of key/INTEGER pairs that libaks
 * decode_extended_state reads: ss state, sls lock state (MKBGetDeviceLockState:
 * 0 = unlocked), sb backoff, sfa failed attempts, sgs generation, sas
 * assertions, sgpe grace period, srcd recovery countdown.
 */
static IOReturn
sweet_aks_device_state(IOExternalMethodArguments *args)
{
	static const struct {
		const char *key;
		uint8_t value;
	} fields[] = {
		{ "ss", SWEET_AKS_LOCK_STATE }, { "sls", 0 }, { "sb", 0 }, { "sfa", 0 },
		{ "sgs", 0 }, { "sas", 0 }, { "sgpe", 0 }, { "srcd", 0 },
	};
	uint8_t buf[128];
	uint32_t n = 2;
	for (unsigned int i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
		n += sweet_aks_der_pair(buf + n, fields[i].key, fields[i].value);
	}
	buf[0] = 0x31;
	buf[1] = (uint8_t)(n - 2);
	return sweet_aks_struct_out(args, buf, n);
}

/* hackios selector31: software no-passcode configuration; no key material. */
static IOReturn
sweet_aks_configuration(IOExternalMethodArguments *args)
{
 uint8_t buf[64];uint32_t n=2;
 n+=sweet_aks_der_pair(buf+n,"bd",0);
 n+=sweet_aks_der_pair(buf+n,"gp",0);
 n+=sweet_aks_der_pair(buf+n,"mua",10);
 buf[0]=0x31;buf[1]=(uint8_t)(n-2);
 return sweet_aks_struct_out(args,buf,n);
}

/*
 * kAKSWrapKey / kAKSUnwrapKey, as HakiOS MiniAppleKeyStore (same iOS 16 D22
 * userland). securityd wraps every item's key with its class key; log 330
 * (unified log): every wrap was answered e00002c7, so no keychain item could
 * be stored and bluetoothd exited "restarting reason=130" after "Failed to
 * set new identity root" (SecItemAdd -25291), every 10 s. There is no SEP and
 * no secret: the wrapped key is the key mixed with a fixed pattern, 8 bytes
 * longer as with AES key wrap (RFC 3394) which callers size for. Fixed, so
 * items stored on the persistent root unwrap again after a reboot.
 */
static const uint8_t sweet_aks_wrap_mark[4] = { 'H', 'K', 'w', 'r' };
static void
sweet_aks_wrap_mix(uint8_t *bytes, uint32_t length)
{
	for (uint32_t i = 0; i < length; i++) {
		bytes[i] ^= (uint8_t)(0x5a + 37 * i);
	}
}
static IOReturn
sweet_aks_struct_in(IOExternalMethodArguments *args, uint8_t *buf, uint32_t cap, uint32_t *len)
{
	if (args->structureInputDescriptor != NULL) {
		IOMemoryDescriptor *in = args->structureInputDescriptor;
		IOByteCount n = in->getLength();
		if (n == 0 || n > cap) return kIOReturnBadArgument;
		IOReturn r = in->prepare();
		if (r != kIOReturnSuccess) return r;
		IOByteCount got = in->readBytes(0, buf, n);
		in->complete();
		if (got != n) return kIOReturnUnderrun;
		*len = (uint32_t)n;
		return kIOReturnSuccess;
	}
	if (args->structureInput == NULL || args->structureInputSize == 0 ||
	    args->structureInputSize > cap) {
		return kIOReturnBadArgument;
	}
	memcpy(buf, args->structureInput, args->structureInputSize);
	*len = args->structureInputSize;
	return kIOReturnSuccess;
}
static IOReturn
sweet_aks_wrap(IOExternalMethodArguments *args)
{
	uint8_t wrapped[8 + 64];
	uint32_t length = 0;
	if (args->scalarInputCount < 2) return kIOReturnBadArgument;
	IOReturn r = sweet_aks_struct_in(args, wrapped + 8, 64, &length);
	if (r != kIOReturnSuccess) return r;
	memcpy(wrapped, sweet_aks_wrap_mark, sizeof(sweet_aks_wrap_mark));
	wrapped[4] = (uint8_t)args->scalarInput[1];
	wrapped[5] = wrapped[6] = wrapped[7] = 0;
	sweet_aks_wrap_mix(wrapped + 8, length);
	r = sweet_aks_struct_out(args, wrapped, 8 + length);
	if (r != kIOReturnSuccess) return r;
	if (args->scalarOutputCount >= 1) {
		args->scalarOutput[0] = args->scalarInput[1];
		args->scalarOutputCount = 1;
	} else {
		args->scalarOutputCount = 0;
	}
	return kIOReturnSuccess;
}
static IOReturn
sweet_aks_unwrap(IOExternalMethodArguments *args)
{
	uint8_t wrapped[8 + 64];
	uint32_t length = 0;
	IOReturn r = sweet_aks_struct_in(args, wrapped, sizeof(wrapped), &length);
	if (r != kIOReturnSuccess) return r;
	if (length <= 8 || memcmp(wrapped, sweet_aks_wrap_mark, sizeof(sweet_aks_wrap_mark)) != 0) {
		return kIOReturnBadArgument;
	}
	sweet_aks_wrap_mix(wrapped + 8, length - 8);
	args->scalarOutputCount = 0;
	return sweet_aks_struct_out(args, wrapped + 8, length - 8);
}

static uint32_t
sweet_aks_struct_out_size(IOExternalMethodArguments *args)
{
	return args->structureOutputDescriptor != NULL ?
	       (uint32_t)args->structureOutputDescriptor->getLength() : args->structureOutputSize;
}

IOReturn
AppleKeyStoreUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target,
    __unused void *reference)
{
	IOReturn ret = kIOReturnSuccess;
	uint32_t in_scalars = args->scalarInputCount;
	uint32_t in_struct = args->structureInputDescriptor != NULL ?
	    (uint32_t)args->structureInputDescriptor->getLength() : args->structureInputSize;
	uint32_t out_scalars = args->scalarOutputCount;
	uint32_t out_struct = sweet_aks_struct_out_size(args);
	uint64_t arg0 = in_scalars > 0 ? args->scalarInput[0] : 0;

	switch (selector) {
	case kAKSOpen:
	case kAKSUnloadBag:
	case kAKSSetSystem:
	case kAKSUnlockDevice:
	case kAKSUnlockBag:
	case kAKSLockBag:
	case kAKSAssertHold:
	case kAKSAssertDrop:
	case kAKSAssertConsume:
	case kAKSUnloadSessionBags:
	case kAKSNotifyStateSet:
		args->scalarOutputCount = 0;
		break;
	case kAKSGetSystem:
		if (out_scalars < 1) {
			ret = kIOReturnBadArgument;
			break;
		}
		args->scalarOutput[0] = SWEET_AKS_SYSTEM_HANDLE;
		args->scalarOutputCount = 1;
		break;
	case kAKSGetLockState:
		if (out_scalars < 1) {
			ret = kIOReturnBadArgument;
			break;
		}
		args->scalarOutput[0] = SWEET_AKS_LOCK_STATE;
		args->scalarOutputCount = 1;
		break;
	case kAKSNotifyStateIsSet:
		/* Nothing was ever set: answer "not set". */
		if (out_scalars < 1) {
			ret = kIOReturnBadArgument;
			break;
		}
		args->scalarOutput[0] = 0;
		args->scalarOutputCount = 1;
		break;
	case kAKSGetConfiguration:
		args->scalarOutputCount = 0;
		ret = sweet_aks_configuration(args);
		break;
	case kAKSGetDeviceState:
	case kAKSGetExtDeviceState:
		args->scalarOutputCount = 0;
		ret = sweet_aks_device_state(args);
		break;
	case kAKSWrapKey:
		ret = sweet_aks_wrap(args);
		break;
	case kAKSUnwrapKey:
		ret = sweet_aks_unwrap(args);
		break;
	default:
		ret = kIOReturnUnsupported;
		break;
	}

	/* Selector 17 succeeds thousands of times during normal X9 UI work. */
	/* Wrap/unwrap run once per keychain item: log the first few successes. */
	static SInt32 wrap_logs;
	bool quiet = sweet_early_is_x9() && ret == kIOReturnSuccess &&
	    (selector == 17 || ((selector == kAKSWrapKey || selector == kAKSUnwrapKey) &&
	    OSIncrementAtomic(&wrap_logs) >= 8));
	if (!quiet) {
		char name[32];
		proc_selfname(name, sizeof(name));
		IOLog("SWEET AKS: %s sel %u arg0 %llu in %u/%u out %u/%u -> 0x%x\n", name, selector,
		    arg0, in_scalars, in_struct, out_scalars, out_struct, ret);
	}
	return ret;
}
#endif /* SWEET */
