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
#include <mach/mach.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <libkern/OSCacheControl.h>

_Static_assert(sizeof(vm_address_t) == 8, "20H392 arm64 VM addresses must be 64-bit");
static intptr_t image_slide;
static uintptr_t addr(uint64_t va) { return (uintptr_t)(va + image_slide); }
typedef void (*init_fn)(uint32_t, void *);
typedef void (*callback_fn)(void *);
static init_fn original_init;
static callback_fn original_callbacks;

static void vendor_init(uint32_t chip, void *context)
{
    if (chip) { original_init(chip, context); return; }
    *(uint32_t *)addr(0x10084ff20) = 0;
    memset((void *)addr(0x100850030), 0, 0x750);
    *(uintptr_t *)addr(0x100850780) = 0; /* vendor event hook */
    memset((void *)addr(0x100850790), 0, 0x28); /* vendor callback bridge */
    *(uintptr_t *)addr(0x10084f930) = 0; /* vendor init parser */
    *(uintptr_t *)addr(0x10084f950) = 0; /* vendor sleep parser */
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
static uintptr_t trampoline(vm_address_t base, const struct edit *e)
{
    uint32_t words[8];
    memcpy(words, e->before, 16);
    jump_words(words+4, addr(e->va+16));
    memcpy((void *)(uintptr_t)base, words, sizeof(words));
    return (uintptr_t)base;
}

int x9_vendor16_install(intptr_t slide)
{
    /* Opt-in only; build manifest remains non-stageable until native model
     * capability getters and boot/BlueTool orchestration are complete. */
    const char *selection = getenv("X9_BT_VENDOR");
    if (!selection || strcmp(selection, "MT6653")) return 0;
    int boot = ENXIO; size_t n = sizeof(boot);
    if (sysctlbyname("kern.x9_bt_boot", &boot, &n, NULL, 0) || n != sizeof(boot) || boot) {
        fprintf(stderr, "X9 vendor: complete board initialization required; no text changed\n");
        return -1;
    }
    image_slide = slide;
    struct edit edits[] = {
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
        /* This callback tail has not called logging/functions: x0 still
         * holds actual HCI status. Preserve it instead of MOV w0,#0. */
        {0x10010bd3c, {0x52800000}, {0xd503201f}, 1},
    };
    const unsigned count = sizeof(edits)/sizeof(edits[0]);
    for (unsigned k = 0; k < count; k++) {
        if (memcmp((void *)addr(edits[k].va), edits[k].before, edits[k].words*4)) {
            fprintf(stderr, "X9 vendor: unexpected 20H392 instruction at %llx; no text changed\n",
                    (unsigned long long)edits[k].va);
            return -1;
        }
    }
    vm_address_t tramp = 0;
    vm_size_t page = (vm_size_t)vm_page_size;
    kern_return_t kr = vm_allocate(mach_task_self(), &tramp, page, VM_FLAGS_ANYWHERE);
    if (kr) return -1;
    original_init = (init_fn)trampoline(tramp, &edits[3]);
    original_callbacks = (callback_fn)trampoline(tramp+32, &edits[4]);
    sys_icache_invalidate((void *)(uintptr_t)tramp, 64);
    kr = vm_protect(mach_task_self(), tramp, page, FALSE, VM_PROT_READ|VM_PROT_EXECUTE);
    if (kr) { vm_deallocate(mach_task_self(), tramp, page); return -1; }
    jump_words(edits[3].after, (uintptr_t)vendor_init);
    jump_words(edits[4].after, (uintptr_t)register_callbacks);

    /* Preflight ALL pages before modifying ANY instruction. COW mappings
     * preserve the original Apple executable on disk. Constructor installs
     * before native controller/RX threads start. */
    vm_address_t pages[6]; unsigned page_count = 0;
    for (unsigned k = 0; k < count; k++) {
        vm_address_t p = addr(edits[k].va) & ~(page-1);
        unsigned j;
        for (j = 0; j < page_count && pages[j] != p; j++) {}
        if (j != page_count) continue;
        kr = vm_protect(mach_task_self(), p, page, FALSE,
                              VM_PROT_READ|VM_PROT_WRITE|VM_PROT_EXECUTE|VM_PROT_COPY);
        if (kr) {
            for (j = 0; j < page_count; j++)
                if (vm_protect(mach_task_self(), pages[j], page, FALSE,
                                    VM_PROT_READ|VM_PROT_EXECUTE)) abort();
            vm_deallocate(mach_task_self(), tramp, page);
            fprintf(stderr, "X9 vendor: COW text protection failed (%d); no text changed\n", kr);
            return -1;
        }
        pages[page_count++] = p;
    }
    for (unsigned k = 0; k < count; k++) {
        memcpy((void *)addr(edits[k].va), edits[k].after, edits[k].words*4);
        sys_icache_invalidate((void *)addr(edits[k].va), edits[k].words*4);
    }
    for (unsigned j = 0; j < page_count; j++)
        if (vm_protect(mach_task_self(), pages[j], page, FALSE,
                            VM_PROT_READ|VM_PROT_EXECUTE)) abort();
    fprintf(stderr, "X9 vendor: native type0 routing; generic callbacks retained, unsupported vendor ops stay null\n");
    return 1;
}
