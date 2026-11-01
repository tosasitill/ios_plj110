/* X9 gauge bridge: no synthetic SOC, capacity, temperature or charger state.
 * Reference: hackios c84b4634 iokit/Kernel/hakios.battery.cpp (power source API).
 * Native sampling is deliberately separate from power management publication.
 */
#include <device/device_types.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/pwr_mgt/IOPMPowerSource.h>
#include "SweetBoard.h"
#include "X9BatterySample.h"
class SweetX9Battery : public IOPMPowerSource {
    OSDeclareDefaultStructors(SweetX9Battery);
public:
    void number(const char *key, uint32_t value) {
        const OSSymbol *s = OSSymbol::withCString(key);
        OSNumber *n = OSNumber::withNumber(value, 32);
        if (s && n) setPSProperty(s, n);
        OSSafeReleaseNULL(s); OSSafeReleaseNULL(n);
    }
    void flag(const char *key, bool value) {
        const OSSymbol *s = OSSymbol::withCString(key);
        if (s) setPSProperty(s, value ? kOSBooleanTrue : kOSBooleanFalse);
        OSSafeReleaseNULL(s);
    }
    void invalidate(void) {
        flag("X9BatterySampleValid", false);
        setIsCharging(false); flag("FullyCharged", false);
        setTimeRemaining(-1); updateStatus();
    }
    void report(const X9BatterySample &b) {
        setBatteryInstalled(b.present != 0);
        setCurrentCapacity(b.percent); setMaxCapacity(100);
        setVoltage(b.voltage_mv); setAmperage(b.current_ma);
        setExternalConnected(b.external != 0);
        setExternalChargeCapable(b.charge_capable != 0);
        setIsCharging(b.external && b.charge_capable && b.status == 1);
        flag("FullyCharged", b.status == 4);
        flag("AppleRawExternalConnected", b.external != 0);
        flag("X9BatterySampleValid", true);
        number("Temperature", (uint32_t)b.temperature_centi);
        number("InstantAmperage", (uint32_t)b.current_ma);
        number("AppleRawCurrentCapacity", b.remaining_mah);
        number("AppleRawMaxCapacity", b.full_mah);
        number("NominalChargeCapacity", b.full_mah);
        setCycleCount(b.cycles);
        setTimeRemaining(-1);
        setAtWarnLevel(false); setAtCriticalLevel(false);
        updateStatus();
    }
};
OSDefineMetaClassAndStructors(SweetX9Battery, IOPMPowerSource);
static SweetX9Battery *source;
/* Single native sampling worker owns this entrypoint, including invalidation. */
extern "C" int X9BatteryReport(const X9BatterySample *b, unsigned size) {
    if (!sweet_board_x9() || !b || size != sizeof(*b) ||
        b->size != sizeof(*b) || b->version != 1) return -1;
    if (b->percent < 0 || b->percent > 100 || b->voltage_mv < 2000 ||
        b->voltage_mv > 6000 || b->current_ma < -30000 || b->current_ma > 30000 ||
        b->temperature_centi < -4000 || b->temperature_centi > 10000 ||
        b->present != 1 || b->external < 0 || b->external > 1 ||
        b->charge_capable < 0 || b->charge_capable > 1 ||
        b->status < 1 || b->status > 4 || b->remaining_mah < 0 ||
        b->full_mah <= 0 || b->full_mah > 20000 ||
        b->remaining_mah > b->full_mah || b->cycles < 0 ||
        (b->charge_capable && !b->external) ||
        (b->status == 1 && (!b->external || !b->charge_capable))) return -1;
    if (!source) {
        IOService *provider = IOService::getPlatform();
        if (!provider) return -2;
        SweetX9Battery *s = OSTypeAlloc(SweetX9Battery);
        if (!s || !s->init()) { OSSafeReleaseNULL(s); return -2; }
        if (!s->attach(provider)) { s->release(); return -2; }
        if (!s->start(provider)) { s->detach(provider); s->release(); return -2; }
        s->setName("X9Battery");
        s->report(*b);
        source = s;
        s->registerService();
        IOLog("X9 BATTERY: native source registered, SOC=%d%%\n", b->percent);
    } else source->report(*b);
    return 0;
}
extern "C" void X9BatteryInvalidate(void) {
    if (!source) return;
    source->invalidate();
}

#include "X9Gauge.inc"
