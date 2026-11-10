/* iOS transport status and bounded raw-stream echo. No root filesystem writes,
 * pairing bypass, automatic ambiguous retries, or transport payload logging. */
#include "x9usb.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
static volatile sig_atomic_t stop;
static void interrupted(int signo) { (void)signo; stop = 1; }
static double now(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + (double)t.tv_nsec / 1e9; }
static void print_status(const struct x9_usb_status *s)
{
    const char *states[] = { "off", "initializing", "ready", "configured", "fault" };
    printf("ABI=%u state=%s error=%d mode=%u generation=%u speed=%u config=%u alt=%u link=%u\n",
        s->version, s->state < 5 ? states[s->state] : "unknown", s->error, s->mode,
        s->generation, s->speed, s->configuration, s->data_alt, s->link);
    printf("QMU version=%04x error=%08x completed=%llu errors=%llu resets=%llu raw=%u queued tx=%u rx=%u\n",
        s->trunk_version, s->qmu_error, (unsigned long long)s->dma_completions,
        (unsigned long long)s->dma_errors, (unsigned long long)s->resets,
        s->raw_enabled, s->tx_pending, s->rx_pending);
    printf("ECM frames tx=%llu rx=%llu; bulk bytes tx=%llu rx=%llu dropped=%llu native-Apple-stack=%u\n",
        (unsigned long long)s->net_tx_frames, (unsigned long long)s->net_rx_frames,
        (unsigned long long)s->raw_tx_bytes, (unsigned long long)s->raw_rx_bytes,
        (unsigned long long)s->dropped, s->native_apple_stack);
    printf("bus forced-off=%u cycling=%u raw-draining=%u\n", !!(s->reserved & X9_USB_BUS_FORCED_OFF),
        !!(s->reserved & X9_USB_BUS_CYCLING), !!(s->reserved & X9_USB_RAW_DRAINING));
}
int main(int argc, char **argv)
{
    if ((argc == 2 && !strcmp(argv[1], "--description")) ||
        (argc == 3 && !strcmp(argv[1], "--set-product"))) {
        io_connect_t c;
        kern_return_t e = x9usb_open(false, &c);
        if (e) { fprintf(stderr, "USB status open failed %#x\n", e); return 1; }
        struct x9_usb_description_status s;
        e = x9usb_get_description(c, &s); IOServiceClose(c);
        if (e) { fprintf(stderr, "description snapshot failed %#x\n", e); return 1; }
        if (argc == 2) {
            printf("description-ABI=%u revision=%u mode=%u forced-off=%u VID:PID=%04x:%04x bcdDevice=%04x\n",
                s.version, s.revision, s.mode, s.forced_off, s.description.vendor_id,
                s.description.product_id, s.description.device_id);
            puts("Read-only metadata snapshot; serial omitted; not host enumeration or native mux.");
            return 0;
        }
        size_t n = strlen(argv[2]);
        if (!n || n >= X9_USB_STRING_CAPACITY) {
            fprintf(stderr, "product must be 1..63 printable ASCII bytes\n"); return 2;
        }
        memset(s.description.product, 0, sizeof(s.description.product));
        memcpy(s.description.product, argv[2], n);
        e = x9usb_set_description(&s.description);
        if (e) { fprintf(stderr, "original IOKit description update failed %#x; NOT retried (explicit bus-off + root + entitlement required)\n", e); return 1; }
        puts("Metadata committed by MTU3 worker while off-bus; no automatic reconnect or pairing.");
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--native-status")) {
        struct x9usb_native_status s;
        kern_return_t e = x9usb_native_status(&s);
        if (e) { fprintf(stderr, "native registry discovery failed %#x\n", e); return 1; }
        printf("original-IOKit-controller-API=%u controllers=%u interfaces=%u multiplexors=%u\n",
            s.controller_api_available, s.controllers, s.interfaces, s.multiplexors);
        puts("Registry presence is not USB enumeration, mux traffic, Trust, or a lockdown session.");
        return 0;
    }
    if (argc >= 2 && (!strcmp(argv[1], "--bus-off") || !strcmp(argv[1], "--bus-on") ||
        !strcmp(argv[1], "--bus-cycle") || !strcmp(argv[1], "--bus-cycle-delay"))) {
        bool custom = !strcmp(argv[1], "--bus-cycle-delay");
        unsigned operation = !strcmp(argv[1], "--bus-off") ? X9_USB_BUS_OFF :
            !strcmp(argv[1], "--bus-on") ? X9_USB_BUS_ON : X9_USB_BUS_CYCLE;
        uint32_t delay = operation == X9_USB_BUS_CYCLE ? X9_USB_NATIVE_CYCLE_MS : 0;
        if (custom && argc == 3) {
            char *end = NULL; unsigned long n = strtoul(argv[2], &end, 10);
            if (!*argv[2] || *end || n > 1000) { fprintf(stderr, "cycle delay must be 0..1000 ms\n"); return 2; }
            delay = (uint32_t)n;
        } else if (custom || argc != 2) {
            fprintf(stderr, "usage: usbctl --bus-off | --bus-on | --bus-cycle | --bus-cycle-delay <0..1000 ms>\n"); return 2;
        }
        kern_return_t e = custom ? x9usb_cycle_delayed(delay) : x9usb_controller_command(operation, delay);
        if (e) { fprintf(stderr, "original IOKit bus command failed %#x; NOT retried\n", e); return 1; }
        puts(custom ? "X9-only delayed cycle completed; not a native IOKit delay parameter or pairing." :
            "Original IOKit command completed the MTU3 bus/STOP barrier (native cycle=50ms), not mux or pairing.");
        return 0;
    }
    bool tracked = argc >= 2 && !strcmp(argv[1], "--echo-tracked");
    bool echo = tracked || (argc >= 2 && !strcmp(argv[1], "--echo"));
    unsigned seconds = 60;
    if (echo && argc == 3) {
        char *end = NULL; unsigned long n = strtoul(argv[2], &end, 10);
        if (!*argv[2] || *end || n < 1 || n > 600) { fprintf(stderr, "duration must be 1..600 seconds\n"); return 2; }
        seconds = (unsigned)n;
    } else if ((echo && argc > 3) || (!echo && (argc > 2 || (argc == 2 && strcmp(argv[1], "--status"))))) {
        fprintf(stderr, "usage: usbctl [--status | --native-status | --description | --set-product <ASCII> | --echo [1..600 seconds] | --echo-tracked [1..600 seconds] | --bus-off | --bus-on | --bus-cycle | --bus-cycle-delay <0..1000 ms>]\n"); return 2;
    }
    io_connect_t c; kern_return_t e = x9usb_open(echo, &c);
    if (e) { fprintf(stderr, "USB open failed %#x (x9_usb=1/2; echo needs 2 + root + exclusive lease)\n", e); return 1; }
    struct x9_usb_status s;
    e = x9usb_status(c, &s);
    if (e) { fprintf(stderr, "USB status failed %#x\n", e); IOServiceClose(c); return 1; }
    print_status(&s);
    int result = 0;
    if (echo) {
        signal(SIGINT, interrupted); signal(SIGTERM, interrupted);
        unsigned generation = s.generation;
        double deadline = now() + seconds;
        unsigned long long bytes = 0;
        unsigned long long completed = 0;
        while (!stop && now() < deadline) {
            uint8_t data[X9_USB_CHUNK]; size_t n = sizeof(data);
            e = x9usb_receive(c, generation, data, &n);
            if (e == kIOReturnNotReady || e == kIOReturnOffline) {
                /* Allow first enumeration; no data has been consumed/sent yet.
                 * Once a transfer starts, any generation change aborts. */
                struct x9_usb_status current;
                if (!bytes && !x9usb_status(c, &current)) generation = current.generation;
                usleep(1000); continue;
            }
            if (e == kIOReturnAborted && !bytes && !x9usb_status(c, &s)) { generation = s.generation; continue; }
            if (e) { fprintf(stderr, "receive failed %#x; stop at generation %u\n", e, generation); result = 1; break; }
            /* EAGAIN is a proven pre-enqueue rejection. All other errors are
             * terminal; a reset must not echo stale data to a successor bus. */
            uint64_t request = 0;
            do {
                e = tracked ? x9usb_send_tracked(c, generation, data, n, &request) :
                    x9usb_send(c, generation, data, n);
                if (e == kIOReturnNotReady) usleep(1000);
            }
            while (e == kIOReturnNotReady && !stop && now() < deadline);
            if (e) { fprintf(stderr, "enqueue failed %#x\n", e); result = 1; break; }
            bytes += n;
            if (tracked) {
                struct x9_usb_tx_result tx = {0};
                while (!stop && now() < deadline) {
                    e = x9usb_tx_result(c, generation, request, true, &tx);
                    if (e || tx.state >= X9_USB_TX_COMPLETE) break;
                    usleep(1000);
                }
                if (e || tx.state != X9_USB_TX_COMPLETE || tx.error || tx.actual != n) {
                    fprintf(stderr, "TX request=%llu result=%#x state=%u errno=%d; NOT replayed (cancel/timeout may be partial)\n",
                        (unsigned long long)request, e, tx.state, tx.error);
                    result = 1; break;
                }
                completed += tx.actual;
            }
        }
        fprintf(stderr, "echo queued %llu bytes; this is not a hardware completion count\n", bytes);
        if (tracked) fprintf(stderr, "echo QMU-completed %llu bytes (HWO/CPR + STOP); host readback still required\n", completed);
        if (!x9usb_status(c, &s)) print_status(&s);
    }
    IOServiceClose(c); return result;
}
