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
#include <IOKit/IOLocks.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSString.h>

extern "C" int sweet_early_is_x9(void);
#include <arm64/sweet_early.h>
extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

class SweetPlatformExpert : public IODTPlatformExpert {
	OSDeclareDefaultStructors(SweetPlatformExpert);

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	OSString *createSystemSerialNumberString(OSData *property) APPLE_KEXT_OVERRIDE;
	void processTopLevel(IORegistryEntry *root) APPLE_KEXT_OVERRIDE;
	const char *deleteList(void) APPLE_KEXT_OVERRIDE;
	const char *excludeList(void) APPLE_KEXT_OVERRIDE;
	long getGMTTimeOfDay(void) APPLE_KEXT_OVERRIDE;
	void setGMTTimeOfDay(long secs) APPLE_KEXT_OVERRIDE;
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

/*
 * MT6687 PMIC RTC over PMIF/SPMI, read-only. Without this the calendar
 * clock starts at epoch 0 (1970 + uptime) on every boot: the stock
 * IOPlatformExpert::getGMTTimeOfDay returns 0 and nothing else in the
 * kernel reads a hardware clock. The real RTC lives in the MT6687 PMIC
 * and Android keeps it set, so iOS only ever needs to read it.
 *
 * Protocol ported from the Android tree
 * (~/HelioDarwin/oppo-source/modules/kernel/kernel_device_modules-6.12):
 *  - drivers/spmi/spmi-mtk-pmif-core.c, mt6993_pmif_arb: PMIF MMIO
 *    0x1c01a000 (DTS reg-names "pmif", path 0), software channel
 *    soc_chan = 2 -> SWINF_2 register block; a transfer is
 *    wait-IDLE, (write WDATA), send ACC = cmd<<30 | sid<<24 |
 *    bc<<16 | addr, wait-WFVLDCLR, read RDATA, clear VLDCLR.
 *  - MT6687 is USID 9 on path 0 (mt6687.dtsi reg = <0x9 SPMI_USID>,
 *    spmi-dev-mask 0x55a8 has bit 9 clear).
 *  - drivers/rtc/rtc-mt6685.c (mediatek,mt6687-rtc binds mt6687_rtc_data,
 *    the MT6685 register layout) + mt6687.dtsi base = 0x580: power the
 *    RTC clocks via SCK_TOP_CKPDN_CON0_L CLR/SET, reload the live
 *    counter into TC with BBPU KEY|RELOAD + WRTGR trigger, then read
 *    the 7 u16 TC registers (year raw + 1968, month 1-based).
 * Writes are left to Android this build; setGMTTimeOfDay only logs.
 */
#define RTC_PMIF_BASE       0x1c01a000UL
#define RTC_PMIF_MAP_SIZE   0x1000UL

/* SWINF channel 2 (mt6993 soc_chan = 2), mt6xxx_regs offsets */
#define RTC_PMIF_ACC        0x0880u
#define RTC_PMIF_WDATA      0x0884u
#define RTC_PMIF_RDATA      0x0894u
#define RTC_PMIF_VLDCLR     0x08A4u
#define RTC_PMIF_STA_REG    0x08A8u

#define RTC_PMIF_CMD_LONG   3u   /* PMIF_CMD_EXT_REG_LONG: 16-bit PMIC addr */
#define RTC_PMIF_SID        9u   /* MT6687 USID */
#define RTC_PMIF_STA(v)     ((((v) >> 1) & 0x7))
#define RTC_PMIF_STA_IDLE   0u
#define RTC_PMIF_STA_VLDCLR 6u
#define RTC_PMIF_ERR(v)     (((v) >> 18) & 0x1)
#define RTC_PMIF_TRIES      400000u

/* RTC block, MT6685 layout at PMIC base 0x580 (absolute PMIC addresses) */
#define RTC_SCK_PDN_SET     0x0515u  /* SCK_TOP_CKPDN_CON0_L_SET   */
#define RTC_SCK_PDN_CLR     0x0516u  /* SCK_TOP_CKPDN_CON0_L_CLR   */
#define RTC_CK_MCLK_32K     0x0030u  /* MCLK bit4 + 32k bit5       */
#define RTC_BBPU            0x0588u
#define RTC_BBPU_CBUSY      0x0040u
#define RTC_BBPU_KEY        0x4300u
#define RTC_BBPU_RELOAD     0x0020u
#define RTC_WRTGR           0x05C2u
#define RTC_TC_SEC          0x0592u  /* u16 x7: SEC MIN HOU DOM DOW MTH YEA */
#define RTC_TC_COUNT        7u
#define RTC_MIN_YEAR        1968     /* mt6685/rtc.h: raw year + 1968 */

static const uint16_t rtc_tc_mask[RTC_TC_COUNT] = { 0x3f, 0x3f, 0x1f, 0x1f, 0x7, 0xf, 0x7f };

static volatile uint32_t *rtc_pmif;
static IORecursiveLock *rtc_lock;
extern "C" int X9PMIFAcquire(void);
extern "C" void X9PMIFRelease(void);

/* Wait for the SWINF FSM to reach IDLE, draining a stale WFVLDCLR left
 * by an aborted transfer (spmi-mtk-pmif-core.c error path). */
static bool
rtc_pmif_idle(void)
{
	unsigned n;

	for (n = 0; n < RTC_PMIF_TRIES; n++) {
		uint32_t st = RTC_PMIF_STA(rtc_pmif[RTC_PMIF_STA_REG / 4]);

		if (st == RTC_PMIF_STA_IDLE) {
			return true;
		}
		if (st == RTC_PMIF_STA_VLDCLR) {
			rtc_pmif[RTC_PMIF_VLDCLR / 4] = 1;
		}
	}
	return false;
}

/* One 2-byte PMIC register transfer over the SWINF channel. */
static int
rtc_pmif_xfer_unlocked(uint32_t addr, uint16_t *val, bool write)
{
	unsigned n;
	uint32_t v;

	if (!rtc_pmif_idle()) {
		return -1;
	}
	if (write) {
		rtc_pmif[RTC_PMIF_WDATA / 4] = *val;
	}
	rtc_pmif[RTC_PMIF_ACC / 4] = (RTC_PMIF_CMD_LONG << 30) |
	    (RTC_PMIF_SID << 24) | (1u << 16) | (addr & 0xffffu);
	for (n = 0; n < RTC_PMIF_TRIES; n++) {
		v = rtc_pmif[RTC_PMIF_STA_REG / 4];
		if (RTC_PMIF_STA(v) == RTC_PMIF_STA_VLDCLR) {
			break;
		}
	}
	if (n == RTC_PMIF_TRIES) {
		return -1;
	}
	v = rtc_pmif[RTC_PMIF_RDATA / 4];
	rtc_pmif[RTC_PMIF_VLDCLR / 4] = 1;
	if (RTC_PMIF_ERR(v)) {
		return -1;
	}
	if (!write) {
		*val = (uint16_t)(v & 0xffffu);
	}
	return 0;
}

/* rtc_lock serializes calendar operations, not SWINF2. The same PMIF-M
 * channel also carries keys/modem/camera; lock each complete transaction,
 * not the IOSleep/polling calendar sequence. */
static int
rtc_pmif_xfer(uint32_t addr, uint16_t *val, bool write)
{
	if (X9PMIFAcquire()) {
		return -1;
	}
	int err = rtc_pmif_xfer_unlocked(addr, val, write);
	X9PMIFRelease();
	return err;
}

/* Gregorian date to days since 1970-01-01 (Howard Hinnant's algorithm). */
static long long
rtc_days_from_civil(unsigned y, unsigned m, unsigned d)
{
	long long yy = y;
	long long era;
	unsigned yoe, doy, doe;

	yy -= (m <= 2);
	era = (yy >= 0 ? yy : yy - 399) / 400;
	yoe = (unsigned)(yy - era * 400);
	doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + (long long)doe - 719468;
}

/* The full rtc-mt6685.c __mtk_rtc_read_time sequence: power the clocks,
 * reload the live counter into TC (BBPU reload + WRTGR trigger), read
 * the TC registers with a rollover guard, power the clocks back down. */
static long
rtc_read_gmt(void)
{
	uint16_t tc[RTC_TC_COUNT] = {};
	uint16_t w, sec2;
	uint16_t bbpu = 0;
	unsigned i, n, tries;
	unsigned year, mon, day, hour, minute, second;
	int err = 0;
	long epoch;

	if (!rtc_lock) {
		rtc_lock = IORecursiveLockAlloc();
		rtc_pmif = (volatile uint32_t *)ml_io_map(RTC_PMIF_BASE, RTC_PMIF_MAP_SIZE);
		if (!rtc_lock || !rtc_pmif) {
			IOLog("X9 RTC: map failed, calendar stays at epoch\n");
			return 0;
		}
	}
	IORecursiveLockLock(rtc_lock);

	/* power_on_mclk: ungate MCLK + 32k, settle */
	w = RTC_CK_MCLK_32K;
	err |= rtc_pmif_xfer(RTC_SCK_PDN_CLR, &w, true);
	IOSleep(1);

	/* reload the live counter into the TC registers */
	err |= rtc_pmif_xfer(RTC_BBPU, &bbpu, false);
	w = (uint16_t)(bbpu | RTC_BBPU_KEY | RTC_BBPU_RELOAD);
	err |= rtc_pmif_xfer(RTC_BBPU, &w, true);
	w = 1;                       /* mtk_rtc_write_trigger */
	err |= rtc_pmif_xfer(RTC_WRTGR, &w, true);
	for (n = 0; n < 100; n++) {  /* poll CBUSY clear */
		err |= rtc_pmif_xfer(RTC_BBPU, &bbpu, false);
		if (err || !(bbpu & RTC_BBPU_CBUSY)) {
			break;
		}
		IOSleep(1);
	}

	/* read TC, retried while the second counter advances mid-read */
	for (tries = 0; tries < 8; tries++) {
		for (i = 0; i < RTC_TC_COUNT && !err; i++) {
			w = 0;
			err |= rtc_pmif_xfer(RTC_TC_SEC + (uint32_t)i * 2, &w, false);
			tc[i] = (uint16_t)(w & rtc_tc_mask[i]);
		}
		if (err) {
			break;
		}
		w = 0;
		err |= rtc_pmif_xfer(RTC_TC_SEC, &w, false);
		sec2 = (uint16_t)(w & 0x3f);
		if (!err && sec2 >= (tc[0] & 0x3f)) {
			break;
		}
	}

	/* power_down_mclk */
	w = RTC_CK_MCLK_32K;
	err |= rtc_pmif_xfer(RTC_SCK_PDN_SET, &w, true);

	IORecursiveLockUnlock(rtc_lock);

	if (err) {
		IOLog("X9 RTC: PMIF transfer failed, calendar stays at epoch\n");
		return 0;
	}
	year   = (unsigned)tc[6] + RTC_MIN_YEAR;
	mon    = (unsigned)tc[5];
	day    = (unsigned)tc[3];
	hour   = (unsigned)tc[2];
	minute = (unsigned)tc[1];
	second = (unsigned)tc[0];
	if (year < 2024 || year > 2099 || mon < 1 || mon > 12 ||
	    day < 1 || day > 31 || hour > 23 || minute > 59 || second > 59) {
		IOLog("X9 RTC: implausible %04u-%02u-%02u %02u:%02u:%02u, "
		    "calendar stays at epoch\n", year, mon, day, hour, minute, second);
		return 0;
	}
	epoch = rtc_days_from_civil(year, mon, day) * 86400LL +
	    hour * 3600L + minute * 60L + second;
	IOLog("X9 RTC: %04u-%02u-%02u %02u:%02u:%02u UTC = %ld s\n",
	    year, mon, day, hour, minute, second, epoch);
	return epoch;
}

long
SweetPlatformExpert::getGMTTimeOfDay(void)
{
	if (!sweet_early_is_x9()) {
		return IOPlatformExpert::getGMTTimeOfDay();
	}
	return rtc_read_gmt();
}

void
SweetPlatformExpert::setGMTTimeOfDay(long secs)
{
	static bool logged;

	if (!sweet_early_is_x9() || logged) {
		return;
	}
	logged = true;
	IOLog("X9 RTC: set(%ld) ignored, read-only this build\n", secs);
}

#endif /* SWEET */
