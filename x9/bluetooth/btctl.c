/* Own-code iOS 16 diagnostic client. No device identities/addresses printed. */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "../kernel/src/bsd/dev/x9_bluetooth.h"

static const char *state_name(uint32_t state)
{
    const char *names[] = {"disabled", "passive", "wait-power", "rom-query", "rom-ready", "failed"};
    return state < sizeof(names)/sizeof(names[0]) ? names[state] : "unknown";
}
int main(int argc, char **argv)
{
    const char *op = argc == 1 ? "--status" : argv[1];
    if (argc > 2 || (strcmp(op, "--status") && strcmp(op, "--sections") &&
        strcmp(op, "--query-uart") && strcmp(op, "--chip-id"))) {
        fprintf(stderr, "usage: btctl [--status|--sections|--query-uart|--chip-id]\n"); return 2;
    }
    io_service_t service = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching("X9BluetoothTransport"));
    if (!service) { fprintf(stderr, "X9BluetoothTransport absent (requires x9_bt=1)\n"); return 1; }
    io_connect_t connection = IO_OBJECT_NULL;
    kern_return_t ret = IOServiceOpen(service, mach_task_self(), 0, &connection);
    IOObjectRelease(service);
    if (ret) { fprintf(stderr, "open: 0x%x\n", ret); return 1; }
    struct x9_bt_status s = {0}; size_t size = sizeof(s);
    ret = IOConnectCallStructMethod(connection, X9_BT_GET_STATUS, NULL, 0, &s, &size);
    if (ret || size != sizeof(s) || s.version != X9_BT_ABI_VERSION) {
        fprintf(stderr, "status/ABI mismatch: 0x%x size=%zu version=%u\n", ret, size, s.version);
        IOServiceClose(connection); return 1;
    }
    printf("ABI=%u state=%s errno=%u chip=%04x baud=%u firmware-ready=%u\n",
        s.version, state_name(s.state), s.error, s.chip_id, s.baud, s.firmware_ready);
    printf("firmware-index=%u bytes/%u sections (not downloaded) tx=%llu/%llu rx=%llu/%llu dropped=%llu bad=%llu uart-errors=%llu timeouts=%llu\n",
        s.firmware_bytes, s.firmware_sections, (unsigned long long)s.tx_frames, (unsigned long long)s.tx_bytes,
        (unsigned long long)s.rx_frames, (unsigned long long)s.rx_bytes, (unsigned long long)s.rx_dropped,
        (unsigned long long)s.bad_frames, (unsigned long long)s.uart_errors, (unsigned long long)s.timeouts);
    int result = 0;
    if (!strcmp(op, "--sections")) {
        for (uint64_t i = 0; i < s.firmware_sections; i++) {
            struct x9_bt_section section; size = sizeof(section);
            ret = IOConnectCallMethod(connection, X9_BT_GET_SECTION, &i, 1, NULL, 0, NULL, NULL, &section, &size);
            if (ret || size != sizeof(section)) { fprintf(stderr, "section %llu: 0x%x\n", (unsigned long long)i, ret); result = 1; break; }
            printf("section=%llu type=%08x offset=%08x bytes=%u address=%08x download=%u bin-type=%08x mode=%08x\n",
                (unsigned long long)i, section.type, section.offset, section.bytes, section.address,
                section.download_bytes, section.bin_type, section.mode);
        }
    } else if (strcmp(op, "--status")) {
        // Exactly one known, non-mutating WMT ROM query. No retry/assert loop.
        const uint8_t id = !strcmp(op, "--chip-id") ? 9 : 2;
        const uint8_t cmd[] = {1, 0x6f, 0xfc, 5, 1, 4, 1, 0, id};
        ret = IOConnectCallStructMethod(connection, X9_BT_SEND_H4, cmd, sizeof(cmd), NULL, NULL);
        if (ret) { fprintf(stderr, "H4 send: 0x%x (admin + x9_bt_raw=1 + ROM_READY required)\n", ret); result = 1; }
        else {
            result = 1;
            for (unsigned ms = 0; ms < 500; ms++) {
                uint8_t reply[X9_BT_FRAME_MAX]; size = sizeof(reply);
                ret = IOConnectCallStructMethod(connection, X9_BT_RECV_H4, NULL, 0, reply, &size);
                if (!ret && size == 13 && !memcmp(reply, "\x04\xe4\x0a\x02\x04\x06\x00\x00", 8) && reply[8] == id) {
                    printf("WMT query=%u reply:", id);
                    for (size_t n = 0; n < size; n++) printf(" %02x", reply[n]);
                    putchar('\n'); result = 0; break;
                }
                if (ret && ret != kIOReturnNotReady) { fprintf(stderr, "H4 receive: 0x%x\n", ret); break; }
                usleep(1000);
            }
            if (result) fprintf(stderr, "No matching WMT reply; no fabricated response\n");
        }
    }
    IOServiceClose(connection); return result;
}
