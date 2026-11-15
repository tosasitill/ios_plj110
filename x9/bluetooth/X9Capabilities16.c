/* 20H392 native type-zero capabilities. Reference METHOD: UniXNU/QcaBluetooth
 * Type0.c (pinned 986d6319). No iOS27 layout, fixed controller capabilities,
 * vendor scan extensions, invented advertising count or synthetic HCI reply.
 * Every address/descriptor/prologue below is read independently from iOS16.
 */
#include "X9Capabilities16.h"
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static intptr_t image_slide;
static x9_cap16_features_fn original_le, original_local;
static void *at(uint64_t va) { return (void *)(uintptr_t)(va + image_slide); }
typedef void (*advance_fn)(uint32_t, const char *);
typedef void (*set_fn)(uint32_t);
typedef void *(*model_fn)(void);
typedef uint32_t (*cached_fn)(void *);
typedef void (*complete_fn)(uint32_t, uint32_t);
typedef uint32_t (*submit_fn)(uint32_t, complete_fn);
typedef void (*fail_fn)(void *, uint32_t);
static struct {
    bool commands_valid, local_valid, le_valid, complete;
    uint64_t local_features, le_features;
    uint32_t ext_adv, periodic_adv, periodic_sync, past_sender, past_recipient;
    uint32_t ext_scan, enhanced_connection, spec_adv, coded_phy, phy_2m, adv_sets;
} caps;
/* Native callback ABI has no user context. Do not reuse a pending 203b slot
 * after reset: quarantine its old generation until the queue completes it.
 * Otherwise an old controller response could publish the new epoch's flags. */
static bool query_pending;
static uint64_t generation, query_generation;
/* Computation/pending 203b is private to the native command queue. Getters
 * expose ONE released snapshot only after native caches/setters are ready;
 * they must not advertise ext_adv=true with an as-yet-unqueried count. */
static _Atomic(uint64_t) published;

/* Native submit 10010cbbc..be0 uses the same descriptor offset4 encoding:
 * high byte is bitmap byte, low byte its mask. 10014a340 bounds ID < 0xfd
 * and returns descriptor = 1008448e0 + ID*24. Do not use an opcode as ID.
 */
static bool supports(uint16_t opcode)
{
    if (!caps.commands_valid) return false;
    const uint8_t *descriptors = at(0x1008448e0), *bitmap = at(0x100863938);
    for (unsigned id = 0; id < 0xfd; id++) {
        uint16_t op, bit;
        memcpy(&op, descriptors + id*24 + 2, 2);
        if (op != opcode) continue;
        memcpy(&bit, descriptors + id*24 + 4, 2);
        return (bit >> 8) <= 0x2e && (bit & 255) &&
               (bitmap[bit >> 8] & (bit & 255));
    }
    return false;
}
static bool all(const uint16_t *opcodes, unsigned count)
{
    for (unsigned i = 0; i < count; i++) if (!supports(opcodes[i])) return false;
    return true;
}
static void advance(uint32_t status)
{
    ((advance_fn)at(0x1001094ac))(status, at(0x1006edbc0));
}

/* The provider's dispatch_once caches are consulted BEFORE HCI discovery.
 * Finish each original once via its actual provider object before updating
 * the byte, so a later first call cannot overwrite the publication. Never
 * fabricate dispatch_once tokens or copy an iOS27 cache layout.
 */
static void cache(void *provider, uint64_t getter, uint64_t byte, uint32_t value)
{
    (void)((cached_fn)at(getter))(provider);
    *(uint8_t *)at(byte) = (uint8_t)value;
}
static void set(uint64_t function, uint32_t value) { ((set_fn)at(function))(value); }
static void publish_flags(void)
{
    void *provider = ((model_fn)at(0x10029a838))();
    uint8_t *model = ((model_fn)at(0x10029aba8))();
    model[0xa0] = (uint8_t)caps.ext_adv;
    cache(provider, 0x1002ddd70, 0x10085cba8, caps.ext_scan);
    cache(provider, 0x1002de06c, 0x10085cbb8, caps.ext_adv);
    cache(provider, 0x1002de368, 0x10085cbc8, caps.periodic_adv);
    cache(provider, 0x1002de5f8, 0x10085cbd8, caps.periodic_sync);
    cache(provider, 0x1002de888, 0x10085cbe8, caps.past_sender);
    cache(provider, 0x1002deb18, 0x10085cbf8, caps.past_recipient);
    cache(provider, 0x1002e0a1c, 0x10085ccc8, caps.coded_phy);
    cache(provider, 0x1002e1228, 0x10085ccf8, caps.spec_adv);
    cache(provider, 0x1002e1524, 0x10085cd08, caps.enhanced_connection);
    /* initCoreStack 1003c7110..7198: distinct standard vs vendor gates.
     * 17763c is the BT-spec selector, 177630 extended advertising,
     * 19a7e8 starts Apple's VSC scan path (MUST remain false), while
     * 19a83c selects standard 2041/2042 instead of legacy 200b/200c.
     */
    set(0x10017763c, caps.spec_adv);
    set(0x100177630, caps.ext_adv);
    set(0x100177658, caps.adv_sets);
    set(0x10019a7e8, 0);
    set(0x10019a83c, caps.ext_scan);
    set(0x100188390, caps.enhanced_connection);
    set(0x100192aec, caps.coded_phy);
    set(0x100178bbc, caps.periodic_adv);
    set(0x10008efec, caps.periodic_sync);
    uint64_t snapshot = caps.ext_adv | ((uint64_t)caps.periodic_adv<<1) |
        ((uint64_t)caps.periodic_sync<<2) | ((uint64_t)caps.past_sender<<3) |
        ((uint64_t)caps.past_recipient<<4) | ((uint64_t)caps.ext_scan<<5) |
        ((uint64_t)caps.enhanced_connection<<6) | ((uint64_t)caps.spec_adv<<7) |
        ((uint64_t)caps.coded_phy<<8) | ((uint64_t)caps.phy_2m<<9) |
        ((uint64_t)caps.adv_sets<<16);
    atomic_store_explicit(&published, snapshot, memory_order_release);
    fprintf(stderr, "X9 capabilities: HCI ext scan=%u adv=%u sets=%u periodic=%u/%u PAST=%u/%u coded=%u 2M=%u\n",
            caps.ext_scan, caps.ext_adv, caps.adv_sets, caps.periodic_adv,
            caps.periodic_sync, caps.past_sender, caps.past_recipient, caps.coded_phy, caps.phy_2m);
}
static void clear_flags(void)
{
    memset(&caps.ext_adv, 0, sizeof(caps) - offsetof(__typeof__(caps), ext_adv));
}
void x9_cap16_reset(void)
{
    atomic_store_explicit(&published, 0, memory_order_release);
    generation++;
    memset(&caps, 0, sizeof(caps));
    /* Runtime reset must clear the LOWER selectors and provider caches too,
     * not only the replacement model getters. Configure does not call this
     * function: it runs before text hooks/model construction are installed. */
    publish_flags();
}
void x9_cap16_configure(intptr_t slide, x9_cap16_features_fn le, x9_cap16_features_fn local)
{
    image_slide = slide; original_le = le; original_local = local;
    generation = 1; query_generation = 0; query_pending = false;
    memset(&caps, 0, sizeof(caps));
    atomic_store_explicit(&published, 0, memory_order_release);
}
void x9_cap16_supported(uint32_t status)
{
    caps.commands_valid = !status;
    if (status) { generation++; clear_flags(); publish_flags(); }
    ((advance_fn)at(0x1001094ac))(status, at(0x1006ed9c7));
}
void x9_cap16_local_entry(uint32_t status, const void *features)
{
    if (!status && !features) status = 0x67; /* native invalid parameters */
    caps.local_valid = !status;
    caps.local_features = 0;
    if (!status) memcpy(&caps.local_features, features, 8);
    else { generation++; clear_flags(); publish_flags(); }
    original_local(status, features);
}
void x9_cap16_le_entry(uint32_t status, const void *features)
{
    if (!status && !features) status = 0x67;
    caps.le_valid = !status;
    caps.le_features = 0;
    if (!status) memcpy(&caps.le_features, features, 8);
    /* The original still stores and logs the REAL 8-byte feature mask.
     * Its patched tail calls publish with x20's preserved incoming status. */
    original_le(status, features);
}
static void advertising_complete(uint32_t status, uint32_t sets)
{
    if (!query_pending) return;
    query_pending = false;
    if (query_generation != generation) return; /* discarded reset epoch */
    caps.complete = true;
    if (status) { clear_flags(); publish_flags(); advance(status); return; }
    /* Native 1001770d0..e8 computes 7*count with __memset_chk bound 35:
     * FIVE is the software allocation capacity, NOT a controller claim. */
    caps.adv_sets = sets < 5 ? sets : 5;
    if (!caps.adv_sets) caps.ext_adv = caps.spec_adv = caps.periodic_adv = 0;
    publish_flags(); advance(0);
}
void x9_cap16_publish(uint32_t status)
{
    /* A real failure invalidates an earlier publication/pending query too. */
    if (status) {
        generation++; caps.complete = true; clear_flags(); publish_flags();
        advance(status); return;
    }
    if (query_pending) {
        /* Same-epoch repeated callbacks must not queue/advance twice. A
         * new initialization racing an old query is rejected, never reused. */
        if (query_generation != generation) advance(0x67);
        return;
    }
    if (caps.complete) return;
    clear_flags();
    if (status || !caps.commands_valid || !caps.local_valid || !caps.le_valid) {
        caps.complete = true;
        publish_flags(); advance(status ? status : 0x67); return;
    }
    if (!(caps.local_features & (UINT64_C(1)<<38))) {
        caps.complete = true; publish_flags(); advance(0x67); return;
    }
    static const uint16_t advertising[] = {0x2035,0x2036,0x2037,0x2038,0x2039,0x203a,0x203b,0x203c,0x203d};
    static const uint16_t scanning[] = {0x2041,0x2042};
    static const uint16_t periodic_adv[] = {0x203e,0x203f,0x2040};
    static const uint16_t periodic_sync[] = {0x2044,0x2045,0x2046};
    static const uint16_t past_sender[] = {0x205a,0x205b};
    static const uint16_t past_recipient[] = {0x2059,0x205c,0x205d};
    uint64_t f = caps.le_features;
    caps.ext_scan = all(scanning, 2);
    /* Keep the usable set internally consistent: after using extended
     * advertising the controller may reject legacy scan commands. */
    caps.ext_adv = caps.spec_adv = caps.ext_scan && (f>>12 & 1) && all(advertising, 9);
    caps.enhanced_connection = supports(0x2043);
    caps.periodic_adv = caps.ext_adv && (f>>13 & 1) && all(periodic_adv, 3);
    caps.periodic_sync = (f>>13 & 1) && all(periodic_sync, 3);
    caps.past_sender = (f>>24 & 1) && all(past_sender, 2);
    caps.past_recipient = (f>>25 & 1) && all(past_recipient, 3);
    caps.coded_phy = caps.ext_scan && (f>>11 & 1) && supports(0x2030) && supports(0x2032);
    caps.phy_2m = (f>>8 & 1) && supports(0x2030) && supports(0x2032);
    if (!caps.ext_adv) { caps.complete = true; publish_flags(); advance(0); return; }
    query_generation = generation; query_pending = true;
    uint8_t *flag = at(0x10086391c), previous = *flag;
    *flag = 1;
    /* Independently read descriptor ID d1 -> opcode 203b, formatter byte1.
     * Native command queue owns timeout/error completion. No private VSC. */
    uint32_t error = ((submit_fn)at(0x10010ca98))(0xd1, advertising_complete);
    *flag = previous;
    if (error) {
        query_pending = false; caps.complete = true; clear_flags(); publish_flags();
        ((fail_fn)at(0x1001c2284))(*(void **)at(0x10084f598), error);
    }
}
#define GETTER(name, bit) static uint32_t get_##name(void *model) { \
    (void)model; return (uint32_t)(atomic_load_explicit(&published, memory_order_acquire)>>bit)&1; }
GETTER(ext_adv, 0) GETTER(periodic_adv, 1) GETTER(periodic_sync, 2)
GETTER(past_sender, 3) GETTER(past_recipient, 4) GETTER(ext_scan, 5)
GETTER(enhanced_connection, 6) GETTER(spec_adv, 7) GETTER(coded_phy, 8)
GETTER(phy_2m, 9)
static uint32_t get_adv_sets(void *model)
{ (void)model; return (uint32_t)(atomic_load_explicit(&published, memory_order_acquire)>>16)&255; }
void x9_cap16_prepare_advertising(void *broadcaster)
{
    /* 20H392 constructor 100509670..6c4 caches legacy mode1 at +0x88
     * before discovery. 10050a230..254 reads it for native GAP init. */
    uint32_t mode = get_ext_adv(NULL) ? 0 : 1;
    memcpy((uint8_t *)broadcaster + 0x88, &mode, sizeof(mode));
}
#define BOOL_HOOK(va, name, subtract, comparison) \
    {va, {0xb9426008,subtract,comparison,0x1a9f27e0}, (uintptr_t)get_##name}
const struct x9_cap16_hook x9_cap16_hooks[] = {
    BOOL_HOOK(0x10025a114, ext_adv, 0x51004d08, 0x7100151f),
    BOOL_HOOK(0x10025a128, periodic_adv, 0x51005908, 0x711ee91f),
    BOOL_HOOK(0x10025a13c, periodic_sync, 0x51005908, 0x711ee91f),
    BOOL_HOOK(0x10025a150, past_sender, 0x51005908, 0x711ee91f),
    BOOL_HOOK(0x10025a164, past_recipient, 0x51005908, 0x711ee91f),
    BOOL_HOOK(0x10025a4e8, ext_scan, 0x51004d08, 0x7100151f),
    BOOL_HOOK(0x10025a4fc, enhanced_connection, 0x51004d08, 0x7100151f),
    BOOL_HOOK(0x10025a510, spec_adv, 0x51004d08, 0x7100151f),
    BOOL_HOOK(0x10025a4d4, coded_phy, 0x51005108, 0x711ef11f),
    /* This version's actual local-2M predicate is a lower-stack HCI getter,
     * not the iOS27 model slot/cache. 18e6e0 / 187d34 consume this getter.
     * Keep the native mask intact and additionally require 2030/2032. */
    {0x1000f9d38, {0xd0003b48,0x39639508,0x12000100,0xd65f03c0}, (uintptr_t)get_phy_2m},
    {0x10026ec24, {0xa9be4ff4,0xa9017bfd,0x910043fd,0xb9426014}, (uintptr_t)get_adv_sets},
};
const size_t x9_cap16_hook_count = sizeof(x9_cap16_hooks)/sizeof(x9_cap16_hooks[0]);
