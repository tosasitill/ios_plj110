/* MT6653 connac3 download executor. No MMIO, power or implicit radio-ready. */
#ifndef X9_BT_DOWNLOAD_H
#define X9_BT_DOWNLOAD_H
#include <stdint.h>
#include <stddef.h>
struct x9_bt_download_ops {
    void *context;
    /* One command submission, never retry after an ambiguous TX failure.
     * exchange must retain unrelated HCI events and return one complete WMT
     * event, including H4. raw_write reports actual bytes accepted. */
    int (*exchange)(void *, const uint8_t *, size_t, uint8_t *, size_t *, uint32_t);
    int (*raw_write)(void *, const uint8_t *, size_t, size_t *);
    int (*drain)(void *, uint32_t);
    void (*sleep_ms)(void *, uint32_t);
    uint64_t (*now_ms)(void *);
    /* Exact board SKU WMT command, required after type3. No default FEM. */
    int (*connfem)(void *);
};
struct x9_bt_download_result {
    uint32_t selected, downloaded, already_ready, last_type, last_index;
    uint64_t bytes;
};
int x9_bt_download_mt6653(const struct x9_bt_download_ops *, const uint8_t *,
                          size_t, struct x9_bt_download_result *);
int x9_bt_download_builtin(const struct x9_bt_download_ops *, struct x9_bt_download_result *);
#endif
