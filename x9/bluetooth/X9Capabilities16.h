/* Independently traced 20H392 arm64 capability callback/getter ABI. */
#ifndef X9_CAPABILITIES16_H
#define X9_CAPABILITIES16_H
#include <stdint.h>
#include <stddef.h>
struct x9_cap16_hook {
    uint64_t va;
    uint32_t before[4];
    uintptr_t replacement;
};
typedef void (*x9_cap16_features_fn)(uint32_t, const void *);
extern const struct x9_cap16_hook x9_cap16_hooks[];
extern const size_t x9_cap16_hook_count;
void x9_cap16_configure(intptr_t, x9_cap16_features_fn, x9_cap16_features_fn);
void x9_cap16_reset(void);
void x9_cap16_supported(uint32_t);
void x9_cap16_le_entry(uint32_t, const void *);
void x9_cap16_local_entry(uint32_t, const void *);
void x9_cap16_publish(uint32_t);
void x9_cap16_prepare_advertising(void *);
#endif
