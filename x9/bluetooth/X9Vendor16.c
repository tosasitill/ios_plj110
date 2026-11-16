/* Native 20H392 arm64 type-zero routing. Called only after UUID/transport
 * validation by X9H4Transport's constructor. No Apple file is rewritten.
 *
 * Reference: UniXNU/QcaBluetooth native_ios27.py vendor_routing (method only).
 * Offsets/prologues/table sizes below are independently read from 20H392.
 * In this image BOTH BCM/Apple copy exactly 0x750 B to VSC ops; callbacks
 * register a separate vendor bridge, then store the generic callback at
 * 0x1008507b8. Null vendor ops return native OI_STATUS_NOT_IMPLEMENTED 0xe11.
 * A chipset enum is not HCI manufacturer ID or a controller capability.
 */
#include <errno.h>
#include "X9Bootstrap16.h"
#include <mach/mach.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <libkern/OSCacheControl.h>
#include "X9Capabilities16.h"

_Static_assert(sizeof(vm_address_t) == 8, "20H392 arm64 VM addresses must be 64-bit");
static intptr_t image_slide;
static uintptr_t addr(uint64_t va) { return (uintptr_t)(va + image_slide); }
typedef void (*init_fn)(uint32_t, void *);
typedef void (*callback_fn)(void *);
typedef void (*reset_fn)(uint32_t);
static init_fn original_init;
static callback_fn original_callbacks;
static reset_fn original_reset;
static callback_fn original_advertising;
typedef void (*read_size_fn)(uint32_t, uint32_t);
static x9_cap16_features_fn original_states;
static read_size_fn original_filter, original_pa_list;
typedef uint32_t (*hci_init_fn)(const void *);
typedef void (*acl_rx_fn)(uint32_t, uint32_t, const void *, uint32_t, const void *);
typedef void (*hci_error_fn)(uint32_t, const void *);
typedef void (*packet_rx_fn)(uint32_t, const void *, uint32_t, const void *);
static hci_init_fn original_hci_init;
static callback_fn original_host_buffer;
static acl_rx_fn original_acl_rx;
static hci_error_fn original_hci_error;
static packet_rx_fn original_packet_rx;
/* Native HCI config ABI is twelve bytes. Its registry owns a POINTER, not
 * a copy, so this complete clone must outlive initialization and Reset. */
static uint8_t host_config[12];
static int host_flow_active;
static void failed_read(uint32_t, uint64_t);
struct host_credit { uint16_t handle, count; uint8_t blocked, reserved; };
_Static_assert(sizeof(struct host_credit) == 6, "20H392 host RX credit ABI");

static void credit_fatal(uint32_t status)
{
    /* 0902c0 preserves this status in its native fault payload; 090560 calls
     * _abort_with_payload (NOT _os_fault_with_payload). Unlike the dedicated
     * timeout callback, it does not relabel enqueue/transport errors 0x26d.
     * Never resend a credit whose UART delivery might already have begun. */
    x9_bt_note("X9 H4: host ACL credit failed status=0x%x; native fatal\n", status);
    ((reset_fn)addr(0x1000902c0))(status);
}

static uint32_t hci_init(const void *configuration)
{
    if (*(uint32_t *)addr(0x100863b20)) return original_hci_init(configuration);
    typedef const void *(*get_fn)(uint32_t);
    const void *source = configuration ? configuration :
        ((get_fn)addr(0x1001c2344))(0x2e);
    if (!source) return original_hci_init(NULL); /* native invalid config */
    if (source != host_config) memcpy(host_config, source, sizeof(host_config));
    host_config[7] = 1; /* BR/EDR ACL completed-packet path */
    host_config[8] = 0; /* SCO flow OFF: leave native HCI audio policy alone */
    host_config[9] = 1; /* LE ACL has its own tally and return-credit gate */
    host_flow_active = 0;
    /* The original non-NULL path registers config 2e BEFORE module init.
     * 1c2388 refuses an already initialized module: do not register here
     * from HostBufferSize, nor change only the 0c31 command's wire byte. */
    return original_hci_init(host_config);
}

static void host_buffer(void *context)
{
    /* Every native Reset chain reads SupportedCommands again before 0c33.
     * 0c31/33/35 are bitmap byte10 masks20/40/80, independently checked via
     * native descriptors. Unsupported means a REAL native init failure. */
    if (!x9_cap16_supports(0x0c31) || !x9_cap16_supports(0x0c33) ||
        !x9_cap16_supports(0x0c35)) {
        failed_read(0x274, 0x1006edfed);
        return;
    }
    original_host_buffer(context);
}

static void completed_packets(void *storage)
{
    struct host_credit *credit = storage;
    if (!credit->count || credit->blocked) return; /* original defer gate */
    if (!host_flow_active) { credit_fatal(0x67); return; }
    typedef uint32_t (*submit_fn)(uint32_t, reset_fn, ...);
    /* ID5f's ORIGINAL special marshaller emits one handle and two LE16
     * fields; the original queue bypasses normal command credits for 0c35.
     * Its response parser is 109290/no-op. Do not invent a completion or
     * consume an extra command credit. Successful ENQUEUE alone clears the
     * tally; asynchronous transport failures go through hci_error below. */
    uint32_t status = ((submit_fn)addr(0x10010ca98))(0x5f, NULL,
        (uint64_t)credit->handle, (uint64_t)credit->count);
    if (status) { credit_fatal(status); return; }
    credit->count = 0;
}

static void acl_rx(uint32_t handle_flags, uint32_t length, const void *payload,
                   uint32_t type, const void *timestamp)
{
    original_acl_rx(handle_flags, length, payload, type, timestamp);
    /* The stock function drops zero-length packets before its tally. They
     * still use a controller->host packet credit when the handle is live.
     * Nonzero packets retain the entire native callback and credit path. */
    if (length || !host_flow_active || !*(uint32_t *)addr(0x100863b20) ||
        *(uint32_t *)addr(0x100863918) != 2) return;
    typedef const uint8_t *(*lookup_fn)(uint32_t);
    uint32_t handle = handle_flags & 0xfff;
    const uint8_t *connection = ((lookup_fn)addr(0x1001163a4))(handle);
    /* Lookup excludes disconnected BR tombstones (state7). Core54 HCI4.3
     * frees the disconnected handle's host buffers without an extra 0c35.
     * Preserve native unknown-handle drops; never manufacture an ACK for
     * an unestablished/removed handle. Unsupported conn types stay native. */
    if (!connection || (connection[2] != 0 && connection[2] != 4)) return;
    struct host_credit *credit = (void *)addr(connection[2] == 0 ?
        0x10084f958 : 0x10084f95e);
    credit->handle = (uint16_t)handle;
    credit->count++;
    completed_packets(credit);
}

static void hci_error(uint32_t status, const void *metadata)
{
    /* 115eb4 passes queue+0x10, with opcode LE16 at metadata+4. This same
     * ABI is used by every original 11a220 caller. A successful callback or
     * any other opcode MUST preserve the full original error dispatch. */
    uint16_t opcode = 0;
    if (metadata) memcpy(&opcode, (const uint8_t *)metadata + 4, sizeof(opcode));
    if (status && opcode == 0x0c35 && host_flow_active) {
        credit_fatal(status);
        return;
    }
    original_hci_error(status, metadata);
}

static void packet_rx(uint32_t h4_type, const void *packet, uint32_t total,
                      const void *timestamp)
{
    /* 690ec..690fc passes type separately and data at H4-object+0xa.
     * Complete/Status here each have a 2-byte event header plus 4 bytes.
     * ID5f's original no-op/orphan path can discard genuine 0c35 errors:
     * catch ONLY the exact standard failure, before native dispatch.
     * Successful 0c35 normally emits no event; never await/invent one. */
    if (host_flow_active && h4_type == 4 && packet && total == 6) {
        const uint8_t *p = packet;
        uint16_t opcode = 0; uint8_t status = 0;
        if (p[1] == 4 && p[0] == 0x0e) {
            opcode = (uint16_t)(p[3] | ((uint16_t)p[4] << 8));
            status = p[5]; /* native 1198a8/1198cc */
        } else if (p[1] == 4 && p[0] == 0x0f) {
            status = p[2]; /* native 119110..119120 */
            opcode = (uint16_t)(p[4] | ((uint16_t)p[5] << 8));
        }
        if (opcode == 0x0c35 && status) {
            typedef uint32_t (*status_fn)(uint32_t);
            credit_fatal(((status_fn)addr(0x10014a4cc))(status));
            return;
        }
    }
    /* Includes malformed, unrelated and successful events, ACL/SCO/ISO:
     * preserve their exact original parser, callbacks and timestamp. */
    original_packet_rx(h4_type, packet, total, timestamp);
}

static void failed_read(uint32_t status, uint64_t label)
{
    /* These native callbacks otherwise skip their real cache setter on an
     * error, then force success at the init sequencer. Do not call the setter
     * with a failed/null payload, or discard the controller's error. */
    x9_cap16_reset();
    ((void (*)(uint32_t, const char *))addr(0x1001094ac))(status, (const char *)addr(label));
}
static void states_complete(uint32_t status, const void *states)
{
    /* 10018f0b4 loads eight bytes from x0: states really is a pointer. */
    if (!status && !states) status = 0x67;
    if (!status) original_states(0, states);
    else failed_read(status, 0x1006eddd5);
}
static void filter_complete(uint32_t status, uint32_t size)
{
    /* 1001a9cb0 stores w0; zero is a valid reported scalar, NOT NULL. */
    if (!status) original_filter(0, size);
    else failed_read(status, 0x1006ede1a);
}
static void pa_list_complete(uint32_t status, uint32_t size)
{
    /* 10008fbe4 retains x0 as the list-size scalar for its allocation. */
    if (!status) original_pa_list(0, size);
    else failed_read(status, 0x1006edee4);
}

static void init_complete(uint32_t status, uint64_t label)
{
    if (status) failed_read(status, label);
    else ((void (*)(uint32_t, const char *))addr(0x1001094ac))(0, (const char *)addr(label));
}
static void cis_complete(uint32_t status)
{
    /* Native submit 10010b654 checks real LE CIS bits and skips unsupported
     * descriptors (0x274) BEFORE submitting. Its completion at 10010c600
     * has no cache side effect: it must not turn a issued command's failure
     * into success. A missing feature is not an HCI completion failure. */
    init_complete(status, 0x1006edf25);
}
static void subrating_complete(uint32_t status)
{
    /* Same native feature/descriptor skip at 10010b750; no payload/cache. */
    init_complete(status, 0x1006edf44);
}
static void default_subrating_complete(uint32_t status)
{
    /* Native 10010b84c skips absent features/unsupported descriptor 0xfa.
     * Preserve failures of commands it DID actually issue. */
    init_complete(status, 0x1006edf8b);
}
static void host_flow_complete(uint32_t status)
{
    /* Original 10010c6ac only logs 0x2cd then advances, without cache work.
     * Its log-enabled path explicitly MOVs w0,0, and log-disabled paths
     * leak the logger's return value. Pass the incoming status instead;
     * the sequencer's own error logger retains the exact status and label. */
    host_flow_active = 0;
    if (!status) {
        const uint8_t *configuration = *(const uint8_t **)addr(0x100863910);
        if (configuration != host_config || configuration[7] != 1 ||
            configuration[8] != 0 || configuration[9] != 1) status = 0x67;
        else host_flow_active = 1; /* real 0c31 completion, not config intent */
    }
    init_complete(status, 0x1006ee068);
}

static void reset_complete(uint32_t status)
{
    /* Native HCI Reset starts a new capability epoch even on failure. Keep
     * its log/special-0x26d handling and propagate status through the patched
     * native tail. Never retain a previous controller's cached capabilities. */
    host_flow_active = 0;
    x9_cap16_reset();
    original_reset(status);
}
static void start_advertising(void *broadcaster)
{
    x9_cap16_prepare_advertising(broadcaster);
    original_advertising(broadcaster);
}

static void vendor_init(uint32_t chip, void *context)
{
    if (chip) { original_init(chip, context); return; }
    *(uint32_t *)addr(0x10084ff20) = 0;
    memset((void *)addr(0x100850030), 0, 0x750);
    *(uintptr_t *)addr(0x100850780) = 0; /* vendor event hook */
    memset((void *)addr(0x100850790), 0, 0x28); /* vendor callback bridge */
    *(uintptr_t *)addr(0x10084f930) = 0; /* vendor init parser */
    *(uintptr_t *)addr(0x10084f950) = 0; /* vendor sleep parser */
    host_flow_active = 0;
    x9_cap16_reset();
    /* Keep registered context 0x100850788 and generic callback 0x1008507b8.
     * Do not install no-op vendor functions returning false success. */
}

static void register_callbacks(void *callback)
{
    if (*(uint32_t *)addr(0x10084ff20)) {
        original_callbacks(callback); return;
    }
    *(uintptr_t *)addr(0x1008507b8) = (uintptr_t)callback;
}

struct edit {
    uint64_t va;
    uint32_t before[4], after[4];
    unsigned words;
};
static void jump_words(uint32_t *out, uintptr_t target)
{
    /* LDR x16, PC+8; BR x16; .quad target. x16 is ABI intra-procedure
     * scratch. No PAC: locked original is arm64 subtype0, not arm64e. */
    out[0] = 0x58000050; out[1] = 0xd61f0200;
    out[2] = (uint32_t)target; out[3] = (uint32_t)((uint64_t)target >> 32);
}

/* Copy only the independently verified, PC-independent four-instruction
 * prologue. Its original LR is still on the copied frame; jump (not call)
 * to native +16, so native epilogue returns to the wrapper's caller. */
/* Log 329: the constructor stopped silently after vm_allocate'd memory was
 * to become executable, and the old RWX|COPY text request is refused under
 * this kernel's W^X rules. Trampolines now live in this signed dylib's own
 * __TEXT page, and every page (ours and bluetoothd's) is changed the way a
 * debugger plants a breakpoint: R|W|COPY, write, then back to R|X. */
__attribute__((used, aligned(16384), section("__TEXT,__x9tramp")))
static const uint32_t x9_tramp_area[4096] = {0};
static uintptr_t trampoline(vm_address_t base, const struct edit *e)
{
    uint32_t words[8];
    memcpy(words, e->before, 16);
    jump_words(words+4, addr(e->va+16));
    memcpy((void *)(uintptr_t)base, words, sizeof(words));
    return (uintptr_t)base;
}

static uintptr_t success_trampoline(vm_address_t base, const struct edit *e)
{
    /* ONLY called with status0. Original first instruction is CBNZ w0 to
     * its failed-read tail; replacing that now-unreachable branch by NOP
     * avoids copying a PC-relative target into the trampoline. The remaining
     * STP / MOV fp / MOV x0,x1 are independently verified PC-independent. */
    struct edit success = *e;
    success.before[0] = 0xd503201f;
    return trampoline(base, &success);
}

static int host_flow_contract(void)
{
    /* Read-only original contracts, beyond the six entry prologues. These
     * guard the config lifetime, unchanged packet capacities/queue bypass,
     * error metadata and real status->fatal route before ANY text write. */
    static const struct {
        uint64_t va; uint32_t words[8]; unsigned count;
    } guards[] = {
        {0x1000f35f0, {0xd2800000,0x9400584c}, 2}, /* Init(NULL) caller */
        {0x10010975c, {0xb4000220}, 1}, /* non-NULL config registration */
        {0x100109794, {0xaa1303e0,0x528005c1,0x9402e2fb,0x528005c0}, 4},
        {0x10010987c, {0xb900baa8}, 1}, /* mark module initialized */
        {0x1001c23bc, {0xaa1303e0,0x94000510,0x34000240}, 3}, /* setter rejects live module */
        {0x10010b99c, {0x94004c90,0x90003ad4,0x52800028,0x39247288}, 4},
        {0x10010b9b0, {0xa90123e8,0xa9004fe9,0x100063c1,0xd503201f,
                       0x52800bc0,0x94000435}, 6}, /* host packet counts1/1 */
        {0x10011d084, {0xb94b2108,0xd0003a29,0xd503201f,0xb9491929,
                       0x7100011f,0x7a421920,0x54000220}, 7},
        {0x10011d0e0, {0xaa0103f5,0x34001cc1}, 2}, /* zero-length native drop */
        {0x10011d1c4, {0x7100211f,0x540005e0,0x7100111f,0x54000820}, 4},
        {0x10011d3c4, {0xf9448908,0x39401d08,0x34000588}, 3}, /* BR config+7 */
        {0x10011d464, {0xf9448908,0x39402508,0x34000088}, 3}, /* LE config+9 */
        {0x10011d4b4, {0x39401008,0x340000a8}, 2}, /* original blocked gate */
        {0x10011d4d8, {0x79400269,0xa90023e9,0x52800be0,0xd2800001,0x97ffbd6c}, 5},
        {0x10011fa80, {0x90003988,0x91256108,0x7900091f,0xb900011f}, 4}, /* BR tally +4=0 */
        {0x10011faa0, {0x90003988,0x91257908,0x394043bb,0xb900011f,0x7900091f}, 5}, /* LE +4=0 */
        {0x10011fad0, {0x51001008,0x12003d09,0x6b28235f,0x1a893348,0x79001b88}, 5}, /* payload cap */
        {0x10011ebdc, {0xb0003a28,0x79533900,0xd65f03c0}, 3}, /* capped payload getter */
        {0x100115d10, {0x7130d73f,0x540000a0}, 2}, /* 0c35 cmd-credit bypass */
        {0x100115e74, {0x79402a88,0x7130d51f,0x540000e0}, 3},
        {0x100115eb0, {0x91004101,0xaa1303e0,0x940010da}, 3}, /* error metadata */
        {0x1001123c4, {0x71017c1f,0x54000460}, 2},
        {0x100112454, {0x10fb71e0,0xd503201f,0x17ffffc6}, 3}, /* ID5f no-op */
        {0x1001163b4, {0x3940d809,0x71001d3f,0x54000080,0x79400009,
                       0x6b08013f,0x540006a0}, 6}, /* excludes disconnected */
        {0x1000690ec, {0x395fe900,0x91002901,0x79401102,0x910003e3,0x9402bd20}, 5},
        {0x100119110, {0xf94013e9,0x1100050a,0x79005bea,0x38686920,0x9400c4eb}, 5},
        {0x1001198a8, {0x786b6954,0x11000d0b,0x79005beb}, 3},
        {0x1001198c8, {0x92403d68,0x38686940,0x9400c2ff}, 3},
        {0x10009038c, {0x29004ec9}, 1}, /* realstatus faultpayload */
        {0x100090504, {0x29004c08}, 1},
        {0x10009055c, {0xd2800005,0x94173e48}, 2}, /* _abort_with_payload */
        {0x10014a4cc, {0x7101081f,0x540000c8,0x93401c08,0x90002969}, 4},
    };
    static const uint8_t default_config[12] = {
        0x00,0x04,0x10,0x0a,0x0a,0x2d,0x10,0x00,0x00,0x00,0x00,0x00
    };
    /* Descriptor pointer fields have dyld chained fixups. Compare only
     * pointer-free opcode/SupportedCommands/format prefixes, not raw ptrs. */
    static const uint8_t descriptors[3][8] = {
        {0x06,0x00,0x31,0x0c,0x20,0x0a,0x01,0x00},
        {0x06,0x00,0x33,0x0c,0x40,0x0a,0x07,0x00},
        {0x0e,0x00,0x35,0x0c,0x80,0x0a,0x05,0x00},
    };
    for (size_t k = 0; k < sizeof(guards)/sizeof(guards[0]); k++) {
        if (memcmp((void *)addr(guards[k].va), guards[k].words, guards[k].count*4)) {
            x9_bt_note("X9 vendor: unexpected host-flow contract at %llx; no text changed\n",
                    (unsigned long long)guards[k].va);
            return -1;
        }
    }
    if (*(uint32_t *)addr(0x100863b20) ||
        memcmp((void *)addr(0x10067684e), default_config, sizeof(default_config))) {
        x9_bt_note("X9 vendor: unexpected host-flow config/lifetime; no text changed\n");
        return -1;
    }
    for (size_t k = 0; k < 3; k++) {
        if (memcmp((void *)addr(0x100845198 + k*24), descriptors[k], 8)) {
            x9_bt_note("X9 vendor: unexpected host-flow descriptor; no text changed\n");
            return -1;
        }
    }
    return 0;
}

int x9_vendor16_install(intptr_t slide)
{
    /* Opt-in only; build manifest remains non-stageable until board handoff,
     * complete boot/BlueTool orchestration and device validation are done. */
    const char *selection = getenv("X9_BT_VENDOR");
    if (!selection || strcmp(selection, "MT6653")) return 0;
    int boot = ENXIO; size_t n = sizeof(boot);
    if (sysctlbyname("kern.x9_bt_boot", &boot, &n, NULL, 0) || n != sizeof(boot) || boot) {
        x9_bt_note("X9 vendor: complete board initialization required; no text changed\n");
        return -1;
    }
    image_slide = slide;
    if (host_flow_contract()) return -1;
    struct edit edits[44] = {
        /* Base and derived constructors: use THIRD_PARTY_DONGLE enum0.
         * Derived constructor's type-dependent BCM capability defaults must
         * not survive a base-only change to object+0x260. */
        {0x1002590e4, {0xaa0103f4}, {0xaa1f03f4}, 1},
        {0x10025994c, {0xaa0103f4}, {0xaa1f03f4}, 1},
        /* Original accepts 1..3999. Extend exactly to 0..3999, preserving
         * out-of-range rejection and context storage. */
        {0x100166c74, {0x51000409,0x713e793f}, {0x2a0003e9,0x713e7d3f}, 2},
        {0x100166a04, {0xd100c3ff,0xa9014ff4,0xa9027bfd,0x910083fd}, {0}, 4},
        {0x100166e48, {0xd100c3ff,0xa9014ff4,0xa9027bfd,0x910083fd}, {0}, 4},
        /* Record real supported-command completion, retain native next step. */
        {0x10010bd34, {0xd0002f01,0x91271c21,0x52800000,0x17fff5db}, {0}, 4},
        /* LE features callback logs on BOTH paths, and repurposes x19.
         * Its saved x20 is otherwise unused. Save incoming status before
         * CBZ, log that status via x20, restore it for init sequencing.
         * A NOP at the tail would leak a logging return value, not status.
         * Keep the original 8-byte feature storage at 0x1008638c0+0x24. */
        {0x10010c03c, {0x34000260,0xaa0003f3}, {0xaa0003f4,0x34000240}, 2},
        {0x10010c054, {0xf90003f3}, {0xf90003f4}, 1},
        /* Native frame stays intact; a bridge passes x20 to the publisher
         * then jumps to the ORIGINAL stack-check/epilogue at +16. */
        {0x10010c114, {0xb0002f01,0x912f0021,0x52800000,0x97fff4e3}, {0}, 4},
        {0x10010c01c, {0xd10143ff,0xa9034ff4,0xa9047bfd,0x910103fd}, {0}, 4},
        {0x10010bde8, {0xd100c3ff,0xa9014ff4,0xa9027bfd,0x910083fd}, {0}, 4},
        {0x10010bbd8, {0xd100c3ff,0xa9014ff4,0xa9027bfd,0x910083fd}, {0}, 4},
        /* x19 is callee-saved and never repurposed in this callback. Save
         * incoming status on BOTH paths, retaining native error logging. */
        {0x10010bbe8, {0x340002c0,0xaa0003f3}, {0xaa0003f3,0x340002a0}, 2},
        {0x10010bc48, {0x52800000}, {0x2a1303e0}, 1},
        {0x10050a18c, {0xd10183ff,0xa9025ff8,0xa90357f6,0xa9044ff4}, {0}, 4},
        /* These two callbacks preserve x0 on every valid success path and
         * on the immediate error path. Removing only MOV w0,0 retains the
         * original LE-support / LE-buffer cache logic and actual status. */
        {0x10010c328, {0x52800000}, {0xd503201f}, 1},
        {0x10010c450, {0x52800000}, {0xd503201f}, 1},
        /* Success-only cache setters clobber x0. Keep native successful
         * work in explicit success trampolines; forward failed reads using
         * their own original init-step labels. */
        {0x10010c458, {0x350000c0,0xa9bf7bfd,0x910003fd,0xaa0103e0}, {0}, 4},
        {0x10010c480, {0x350000c0,0xa9bf7bfd,0x910003fd,0xaa0103e0}, {0}, 4},
        {0x10010c5d8, {0x350000c0,0xa9bf7bfd,0x910003fd,0xaa0103e0}, {0}, 4},
        /* These three entire callbacks just set their label, force status0
         * and tail-call advance. All their side effects are reproduced by
         * the wrappers, including a genuine successful completion. */
        {0x10010c600, {0xb0002f01,0x913c9421,0x52800000,0x17fff3a8}, {0}, 4},
        {0x10010c610, {0xb0002f01,0x913d1021,0x52800000,0x17fff3a4}, {0}, 4},
        {0x10010c620, {0xb0002f01,0x913e2c21,0x52800000,0x17fff3a0}, {0}, 4},
        {0x10010c6ac, {0xa9be4ff4,0xa9017bfd,0x910043fd,0x710b341f}, {0}, 4},
        /* H4 RX 100068b28..3c budgets 0x7f0 bytes INCLUDING the 4-byte
         * ACL header. 11fa44 caps controller ACL payload at 0x7f0-4;
         * 11ebdc returns that payload capacity. HostBufferSize's native
         * descriptor 0x5e (0x0c33, format "2122") writes its first value
         * verbatim as Host_ACL_Data_Packet_Length, which EXCLUDES the
         * header. Do not add those four bytes back into the advertised
         * payload limit. Keep the real controller capability and credits,
         * RX layout and native completion status. Standard ACL host flow
         * below negotiates this controller-to-host payload limit; actual
         * controller compliance remains an on-device validation step.
         * ADD w9,w0,#4 -> MOV w9,w0; original opcode must match preflight. */
        {0x10010b9ac, {0x11001009}, {0x2a0003e9}, 1},
        /* Keep all previous indices. New complete native entry hooks. */
        {0x100109724, {0xa9bd57f6,0xa9014ff4,0xa9027bfd,0x910083fd}, {0}, 4},
        {0x10010b92c, {0xd10143ff,0xa90257f6,0xa9034ff4,0xa9047bfd}, {0}, 4},
        {0x10011d4a4, {0xd100c3ff,0xa9014ff4,0xa9027bfd,0x910083fd}, {0}, 4},
        {0x10011d060, {0xd10183ff,0xa90167fa,0xa9025ff8,0xa90357f6}, {0}, 4},
        {0x10011a220, {0xd102c3ff,0xa90857f6,0xa9094ff4,0xa90a7bfd}, {0}, 4},
        {0x10011857c, {0xd10343ff,0xa9095ff8,0xa90a57f6,0xa90b4ff4}, {0}, 4},
    };
    unsigned count = 31;
    if (x9_cap16_hook_count > sizeof(edits)/sizeof(edits[0]) - count) {
        x9_bt_note("X9 vendor: %zu capability hooks exceed edit table; no text changed\n",
                   x9_cap16_hook_count);
        return -1;
    }
    for (size_t k = 0; k < x9_cap16_hook_count; k++) {
        const struct x9_cap16_hook *h = &x9_cap16_hooks[k];
        edits[count].va = h->va; edits[count].words = 4;
        memcpy(edits[count].before, h->before, 16);
        jump_words(edits[count].after, h->replacement);
        count++;
    }
    for (unsigned k = 0; k < count; k++) {
        if (memcmp((void *)addr(edits[k].va), edits[k].before, edits[k].words*4)) {
            x9_bt_note("X9 vendor: unexpected 20H392 instruction at %llx; no text changed\n",
                    (unsigned long long)edits[k].va);
            return -1;
        }
    }
    vm_size_t page = (vm_size_t)vm_page_size;
    vm_address_t tramp = (vm_address_t)(uintptr_t)x9_tramp_area;
    if (page > sizeof(x9_tramp_area) || (tramp & (page-1))) {
        x9_bt_note("X9 vendor: trampoline area %#lx not page-aligned for page %#lx\n",
                   (unsigned long)tramp, (unsigned long)page);
        return -1;
    }
    kern_return_t kr = vm_protect(mach_task_self(), tramp, page, FALSE,
                                  VM_PROT_READ|VM_PROT_WRITE|VM_PROT_COPY);
    if (kr) {
        x9_bt_note("X9 vendor: trampoline page COW failed (%d); no text changed\n", kr);
        return -1;
    }
    original_init = (init_fn)trampoline(tramp, &edits[3]);
    original_callbacks = (callback_fn)trampoline(tramp+32, &edits[4]);
    x9_cap16_configure(slide,
        (x9_cap16_features_fn)trampoline(tramp+64, &edits[9]),
        (x9_cap16_features_fn)trampoline(tramp+96, &edits[10]));
    original_reset = (reset_fn)trampoline(tramp+128, &edits[11]);
    original_advertising = (callback_fn)trampoline(tramp+160, &edits[14]);
    original_states = (x9_cap16_features_fn)success_trampoline(tramp+256, &edits[17]);
    original_filter = (read_size_fn)success_trampoline(tramp+288, &edits[18]);
    original_pa_list = (read_size_fn)success_trampoline(tramp+320, &edits[19]);
    original_hci_init = (hci_init_fn)trampoline(tramp+352, &edits[25]);
    original_host_buffer = (callback_fn)trampoline(tramp+384, &edits[26]);
    original_acl_rx = (acl_rx_fn)trampoline(tramp+416, &edits[28]);
    original_hci_error = (hci_error_fn)trampoline(tramp+448, &edits[29]);
    original_packet_rx = (packet_rx_fn)trampoline(tramp+480, &edits[30]);
    uint32_t bridge[10] = {
        0xaa1403e0, /* MOV x0,x20: actual status preserved across native logs */
        0x580000b0, /* LDR x16, literal +24 */
        0xd63f0200, /* BLR x16: publisher has its own ABI frame */
        0x580000b0, /* LDR x16, literal +32 */
        0xd61f0200, /* BR x16: native stack check/epilogue restores saved LR */
        0xd503201f, 0, 0, 0, 0
    };
    uintptr_t publish = (uintptr_t)x9_cap16_publish, epilogue = addr(0x10010c124);
    memcpy(bridge+6, &publish, 8); memcpy(bridge+8, &epilogue, 8);
    memcpy((void *)(uintptr_t)(tramp+192), bridge, sizeof(bridge));
    sys_icache_invalidate((void *)(uintptr_t)tramp, 512);
    kr = vm_protect(mach_task_self(), tramp, page, FALSE, VM_PROT_READ|VM_PROT_EXECUTE);
    if (kr) {
        x9_bt_note("X9 vendor: trampoline page R|X failed (%d); no bluetoothd text changed\n", kr);
        return -1;
    }
    jump_words(edits[3].after, (uintptr_t)vendor_init);
    jump_words(edits[4].after, (uintptr_t)register_callbacks);
    jump_words(edits[5].after, (uintptr_t)x9_cap16_supported);
    jump_words(edits[8].after, (uintptr_t)(tramp+192));
    jump_words(edits[9].after, (uintptr_t)x9_cap16_le_entry);
    jump_words(edits[10].after, (uintptr_t)x9_cap16_local_entry);
    jump_words(edits[11].after, (uintptr_t)reset_complete);
    jump_words(edits[14].after, (uintptr_t)start_advertising);
    jump_words(edits[17].after, (uintptr_t)states_complete);
    jump_words(edits[18].after, (uintptr_t)filter_complete);
    jump_words(edits[19].after, (uintptr_t)pa_list_complete);
    jump_words(edits[20].after, (uintptr_t)cis_complete);
    jump_words(edits[21].after, (uintptr_t)subrating_complete);
    jump_words(edits[22].after, (uintptr_t)default_subrating_complete);
    jump_words(edits[23].after, (uintptr_t)host_flow_complete);
    jump_words(edits[25].after, (uintptr_t)hci_init);
    jump_words(edits[26].after, (uintptr_t)host_buffer);
    jump_words(edits[27].after, (uintptr_t)completed_packets);
    jump_words(edits[28].after, (uintptr_t)acl_rx);
    jump_words(edits[29].after, (uintptr_t)hci_error);
    jump_words(edits[30].after, (uintptr_t)packet_rx);

    /* Preflight ALL pages before modifying ANY instruction. COW mappings
     * preserve the original Apple executable on disk. Constructor installs
     * before native controller/RX threads start. */
    vm_address_t pages[sizeof(edits)/sizeof(edits[0])]; unsigned page_count = 0;
    for (unsigned k = 0; k < count; k++) {
        vm_address_t p = addr(edits[k].va) & ~(page-1);
        unsigned j;
        for (j = 0; j < page_count && pages[j] != p; j++) {}
        if (j != page_count) continue;
        kr = vm_protect(mach_task_self(), p, page, FALSE,
                              VM_PROT_READ|VM_PROT_WRITE|VM_PROT_COPY);
        if (kr) {
            for (j = 0; j < page_count; j++)
                if (vm_protect(mach_task_self(), pages[j], page, FALSE,
                                    VM_PROT_READ|VM_PROT_EXECUTE)) abort();
            x9_bt_note("X9 vendor: COW text protection failed (%d); no text changed\n", kr);
            return -1;
        }
        pages[page_count++] = p;
    }
    for (unsigned k = 0; k < count; k++) {
        memcpy((void *)addr(edits[k].va), edits[k].after, edits[k].words*4);
        sys_icache_invalidate((void *)addr(edits[k].va), edits[k].words*4);
    }
    for (unsigned j = 0; j < page_count; j++) {
        kr = vm_protect(mach_task_self(), pages[j], page, FALSE, VM_PROT_READ|VM_PROT_EXECUTE);
        if (kr) {
            x9_bt_note("X9 vendor: patched text page %#lx R|X failed (%d)\n",
                       (unsigned long)pages[j], kr);
            abort();
        }
    }
    x9_bt_note("X9 vendor: native type0 routing and HCI capability publisher; unsupported vendor ops stay null\n");
    return 1;
}
