#include "X9Bootstrap16.h"
#include "../kernel/src/bsd/dev/x9_bluetooth.h"
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <errno.h>
#include <sys/sysctl.h>
#include <time.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void x9_bt_note(const char *format, ...)
{
    char text[192]; va_list args; va_start(args, format);
    vsnprintf(text, sizeof(text), format, args); va_end(args);
    fputs(text, stderr);
    size_t n = strlen(text); while (n && text[n-1] == '\n') text[--n] = 0;
    (void)sysctlbyname("kern.x9_bt_note", NULL, NULL, text, n + 1);
}

enum { POWER_AWAKE = 0, POWER_ASLEEP = 1, POWER_TRANSITION = 2 };
static int read_power(int *power)
{
    size_t size = sizeof(*power);
    if (sysctlbyname("kern.x9_bt_power", power, &size, NULL, 0)) return errno;
    if (size != sizeof(*power) || *power < POWER_AWAKE || *power > POWER_TRANSITION)
        return EPROTO;
    return 0;
}
static int read_boot(int *boot)
{
    size_t size = sizeof(*boot);
    if (sysctlbyname("kern.x9_bt_boot", boot, &size, NULL, 0)) return errno;
    return size == sizeof(*boot) ? 0 : EPROTO;
}
static int connect_status(io_connect_t *connection)
{
    io_service_t service = IOServiceGetMatchingService(MACH_PORT_NULL,
        IOServiceMatching("X9BluetoothTransport"));
    if (!service) return ENXIO;
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, connection);
    IOObjectRelease(service);
    if (kr) return kr == kIOReturnNotPermitted ? EPERM : EIO;
    return 0;
}
static int read_status(io_connect_t connection, bool *retry)
{
    if (retry) *retry = false;
    struct x9_bt_status status = {0}; size_t size = sizeof(status);
    kern_return_t kr = IOConnectCallStructMethod(connection, X9_BT_GET_STATUS,
                                   NULL, 0, &status, &size);
    if (kr) return EIO;
    if (size != sizeof(status) || status.version != X9_BT_ABI_VERSION) return EPROTO;
    if (status.state == X9_BT_FAILED) return status.error ? (int)status.error : EIO;
    if (status.state == X9_BT_INITIALIZING) {
        if (retry) *retry = true;
        return EINPROGRESS;
    }
    /* Sleep preserves the downloaded firmware, but is not radio-ready. The
     * worker's power state, not a non-READY service-name check, distinguishes
     * it from an unavailable initializer and from a transition in progress. */
    int power = POWER_TRANSITION;
    int error = read_power(&power);
    if (error) return error;
    if (power == POWER_ASLEEP) {
        int boot = ENXIO;
        error = read_boot(&boot);
        return error ? error : (boot == EHOSTDOWN ? EHOSTDOWN : EPROTO);
    }
    if (power == POWER_TRANSITION) {
        int boot = ENXIO;
        error = read_boot(&boot);
        if (error) return error;
        /* The worker first reserves serial admission, then publishes its
         * IOKit state. A READY snapshot may precede a power2/bootEBUSY read;
         * queued ownership may also precede reservation (power2/boot0 or
         * power2/bootEHOSTDOWN for a queued resume).
         * Neither is a terminal initializer error. */
        if (boot == EBUSY || boot == EHOSTDOWN || !boot) {
            if (retry) *retry = true;
            return EINPROGRESS;
        }
        /* A PASSIVE/unavailable board is terminal, not an ownership wait. */
        return boot;
    }
    if (status.state != X9_BT_READY) return ENXIO;
    if (status.error || status.firmware_ready != 1 || status.chip_id != 0x6653 ||
        status.baud != 24000000) return EPROTO;
    int boot = ENXIO;
    error = read_boot(&boot);
    if (error) return error;
    /* READY alone must not hide a serial worker fault. Both authoritative
     * surfaces must agree; metadata/IORegistry personality is insufficient. */
    return boot;
}
int x9_bootstrap16_status(void)
{
    io_connect_t connection = IO_OBJECT_NULL;
    int error = connect_status(&connection);
    if (error) return error;
    error = read_status(connection, NULL);
    IOServiceClose(connection);
    return error;
}

static int change_power(int target)
{
    int power = POWER_TRANSITION;
    int error = read_power(&power);
    if (error) return error;
    /* Kernel admission distinguishes an unavailable initializer from a
     * competing transition. Preserve its exact error. A timed-out write is
     * never retried here: ownership may already have changed. */
    if (sysctlbyname("kern.x9_bt_power", NULL, NULL, &target, sizeof(target)))
        return errno;
    error = read_power(&power);
    if (error) return error;
    if (power != target) return EPROTO;
    if (target == POWER_AWAKE) return x9_bootstrap16_status();
    int boot = ENXIO;
    error = read_boot(&boot);
    return error ? error : (boot == EHOSTDOWN ? 0 : EPROTO);
}
int x9_bootstrap16_resume(void) { return change_power(POWER_AWAKE); }
int x9_bootstrap16_sleep(void) { return change_power(POWER_ASLEEP); }

int x9_bootstrap16_wait(uint32_t seconds)
{
    struct timespec start, now, pause = {0, 100000000};
    if (clock_gettime(CLOCK_MONOTONIC, &start)) return errno;
    io_connect_t connection = IO_OBJECT_NULL;
    int error;
    for (;;) {
        bool retry = false;
        if (!connection) {
            error = connect_status(&connection);
            retry = error == ENXIO; /* service registration not yet available */
            if (error && !retry) break;
        }
        if (connection) error = read_status(connection, &retry);
        /* PASSIVE and FAILED are terminal. A completed sleep is EHOSTDOWN;
         * the launcher may then issue a separate, explicit resume request.
         * Keep one read-only user-client during initialization/ownership. */
        if (!retry) break;
        if (clock_gettime(CLOCK_MONOTONIC, &now)) { error = errno; break; }
        time_t elapsed = now.tv_sec - start.tv_sec;
        if (elapsed > (time_t)seconds ||
            (elapsed == (time_t)seconds && now.tv_nsec >= start.tv_nsec))
            { error = ETIMEDOUT; break; }
        struct timespec remaining = pause;
        while (nanosleep(&remaining, &remaining)) if (errno != EINTR) {
            error = errno; goto done;
        }
    }
done:
    if (connection) IOServiceClose(connection);
    return error;
}
