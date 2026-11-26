/* Opt-in 20H392 Preferences diagnostic; native calls and manager state stay unchanged. */
#include <dlfcn.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <libkern/OSCacheControl.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <os/log.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define UUID_PREFS "B2ED1830-FB2B-3702-B254-17872B2E0A97"
#define UUID_MANAGER "54FB08F7-FE66-344D-B7CE-31838CDEE572"
#define UUID_MOBILE_BT "FDE5B98E-2257-35C8-B323-92B824492207"
#define CALLBACK_VA UINT64_C(0x1d4d0c380)

static const uint8_t prefs_uuid[16] = {0xb2,0xed,0x18,0x30,0xfb,0x2b,0x37,0x02,0xb2,0x54,0x17,0x87,0x2b,0x2e,0x0a,0x97};
static const uint8_t manager_uuid[16] = {0x54,0xfb,0x08,0xf7,0xfe,0x66,0x34,0x4d,0xb7,0xce,0x31,0x83,0x8c,0xde,0xe5,0x72};
static const uint8_t mobile_uuid[16] = {0xfd,0xe5,0xb9,0x8e,0x22,0x57,0x35,0xc8,0xb3,0x23,0x92,0xb8,0x24,0x49,0x22,0x07};
static _Atomic unsigned notes;
static const uint8_t settings_uuid[16] = {0x43,0x3b,0xd2,0x2d,0x83,0x55,0x34,0x2f,0xba,0x34,0xd2,0x47,0x1d,0xc0,0x1c,0xb0};
static const uint8_t core_bluetooth_uuid[16] = {0x40,0x85,0x02,0x43,0x24,0xa8,0x34,0x02,0x9b,0x19,0x33,0x92,0xd4,0xaf,0x07,0x2b};

static void note(const char *format, ...)
{
    int saved_errno = errno;
    if (atomic_fetch_add_explicit(&notes, 1, memory_order_relaxed) >= 256) {
        errno = saved_errno;
        return;
    }
    char text[192]; va_list args; va_start(args, format);
    vsnprintf(text, sizeof(text), format, args); va_end(args);
    size_t length = strlen(text);
    while (length && text[length-1] == '\n') text[--length] = 0;
    (void)sysctlbyname("kern.x9_bt_note", NULL, NULL, text, length + 1);
    os_log_error(OS_LOG_DEFAULT, "X9 BT SETTINGS: %{public}s", text);
    errno = saved_errno;
}

static int image_uuid(const void *base, const uint8_t expected[16])
{
    const struct mach_header_64 *header = base;
    if (!header || header->magic != MH_MAGIC_64 || header->sizeofcmds > 65536) return 0;
    const uint8_t *at = (const uint8_t *)(header + 1), *end = at + header->sizeofcmds;
    for (uint32_t index = 0; index < header->ncmds; index++) {
        const struct load_command *command = (const struct load_command *)at;
        if ((size_t)(end-at) < sizeof(*command) || command->cmdsize < sizeof(*command) ||
            command->cmdsize > (size_t)(end-at)) return 0;
        if (command->cmd == LC_UUID && command->cmdsize == sizeof(struct uuid_command))
            return !memcmp(((const struct uuid_command *)command)->uuid, expected, 16);
        at += command->cmdsize;
    }
    return 0;
}

static id (*original_specifiers)(id, SEL);
static void (*original_central_state)(id, SEL, id);
static void (*original_xpc_invalid)(id, SEL);
static void (*original_handle_state)(id, SEL, id);

static void manager_xpc_invalid(id object, SEL selector)
{
    note("CoreBluetooth manager XPC connection invalidated");
    original_xpc_invalid(object, selector);
}

static void manager_handle_state(id object, SEL selector, id args)
{
    note("CoreBluetooth manager received XPC state update");
    original_handle_state(object, selector, args);
}

static void settings_install_xpc(void *context)
{
    const struct mach_header *header = context;
    Class manager = objc_getClass("CBManager");
    if (!manager) return;
    Method invalid = class_getInstanceMethod(manager, sel_registerName("xpcConnectionIsInvalid"));
    Method update = class_getInstanceMethod(manager, sel_registerName("handleStateUpdatedMsg:"));
    const char *invalid_type = invalid ? method_getTypeEncoding(invalid) : NULL;
    const char *update_type = update ? method_getTypeEncoding(update) : NULL;
    Dl_info invalid_info = {0}, update_info = {0};
    if (!invalid_type || !update_type ||
        strcmp(invalid_type, "v16@0:8") || strcmp(update_type, "v24@0:8@16") ||
        !dladdr((void *)method_getImplementation(invalid), &invalid_info) || invalid_info.dli_fbase != header ||
        !dladdr((void *)method_getImplementation(update), &update_info) || update_info.dli_fbase != header) {
        note("CoreBluetooth manager XPC diagnostic identity rejected");
        return;
    }
    original_xpc_invalid = (void (*)(id, SEL))method_getImplementation(invalid);
    original_handle_state = (void (*)(id, SEL, id))method_getImplementation(update);
    method_setImplementation(invalid, (IMP)manager_xpc_invalid);
    method_setImplementation(update, (IMP)manager_handle_state);
    note("CoreBluetooth manager XPC diagnostics installed");
}

static int bool_ivar(id object, const char *name)
{
    Ivar variable = class_getInstanceVariable(object_getClass(object), name);
    const char *type = variable ? ivar_getTypeEncoding(variable) : NULL;
    if (!type || strcmp(type, "B")) return -1;
    return *((const unsigned char *)object + ivar_getOffset(variable)) != 0;
}

static id settings_specifiers(id object, SEL selector)
{
    id result = original_specifiers(object, selector);
    note("Settings specifiers restricted=%d busy=%d toggling=%d initialized=%d",
         bool_ivar(object, "_bluetoothRestricted"),
         bool_ivar(object, "_bluetoothIsBusy"),
         bool_ivar(object, "_togglingPower"),
         bool_ivar(object, "_bluetoothInitialized"));
    return result;
}

static void settings_central_state(id object, SEL selector, id manager)
{
    SEL state_selector = sel_registerName("state");
    Method state_method = manager ? class_getInstanceMethod(object_getClass(manager), state_selector) : NULL;
    const char *type = state_method ? method_getTypeEncoding(state_method) : NULL;
    if (type && !strcmp(type, "q16@0:8")) {
        intptr_t state = ((intptr_t (*)(id, SEL))objc_msgSend)(manager, state_selector);
        note("Settings central state=%ld restricted=%d busy=%d toggling=%d initialized=%d",
             (long)state, bool_ivar(object, "_bluetoothRestricted"),
             bool_ivar(object, "_bluetoothIsBusy"), bool_ivar(object, "_togglingPower"),
             bool_ivar(object, "_bluetoothInitialized"));
    } else {
        note("Settings central state selector ABI rejected");
    }
    original_central_state(object, selector, manager);
}

static void settings_install_ui(void *context)
{
    const struct mach_header *header = context;
    Class controller = objc_getClass("BTSDevicesController");
    Method method = controller ? class_getInstanceMethod(controller, sel_registerName("specifiers")) : NULL;
    Dl_info info = {0};
    const char *type = method ? method_getTypeEncoding(method) : NULL;
    if (!type || strcmp(type, "@16@0:8") ||
        !dladdr((void *)method_getImplementation(method), &info) || info.dli_fbase != header) {
        note("Settings UI method identity rejected");
    } else {
        original_specifiers = (id (*)(id, SEL))method_getImplementation(method);
        method_setImplementation(method, (IMP)settings_specifiers);
        note("Settings UI specifiers diagnostic installed");
    }
    method = controller ? class_getInstanceMethod(controller, sel_registerName("centralManagerDidUpdateState:")) : NULL;
    type = method ? method_getTypeEncoding(method) : NULL;
    if (!type || strcmp(type, "v24@0:8@16") ||
        !dladdr((void *)method_getImplementation(method), &info) || info.dli_fbase != header) {
        note("Settings central callback identity rejected");
        return;
    }
    original_central_state = (void (*)(id, SEL, id))method_getImplementation(method);
    method_setImplementation(method, (IMP)settings_central_state);
    note("Settings central callback diagnostic installed");
}

static void settings_image_added(const struct mach_header *header, intptr_t slide)
{
    (void)slide;
    if (image_uuid(header, settings_uuid))
        dispatch_async_f(dispatch_get_main_queue(), (void *)header, settings_install_ui);
    if (image_uuid(header, core_bluetooth_uuid))
        dispatch_async_f(dispatch_get_main_queue(), (void *)header, settings_install_xpc);
}

/* The audit VAs are preferred Mach-O VAs, not offsets from the image header.
 * Shared-cache images have nonzero __TEXT vmaddr values. Resolve dyld's slide
 * for the exact image so each hook is slide+VA, never dli_fbase+VA. */
struct native_image { uintptr_t base; intptr_t slide; };
static int framework_image(const char *path, const uint8_t expected[16],
                           struct native_image *out)
{
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) { note("dlopen failed path=%s error=%s", path, dlerror()); return -1; }
    uint32_t count = _dyld_image_count();
    for (uint32_t index = 0; index < count; index++) {
        const char *name = _dyld_get_image_name(index);
        if (!name || strcmp(name, path)) continue;
        const struct mach_header *header = _dyld_get_image_header(index);
        if (!image_uuid(header, expected)) {
            note("loaded framework UUID rejected path=%s; no text changed", path);
            return -1;
        }
        out->base = (uintptr_t)header;
        out->slide = _dyld_get_image_vmaddr_slide(index);
        return 0;
    }
    note("framework missing from dyld image list path=%s; no text changed", path);
    return -1;
}

typedef int (*service_callbacks_fn)(void *, void *, void *);
typedef int (*default_fn)(void *, void *);
typedef int (*attach_fn)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
typedef void (*event_fn)(void *, uint32_t, uint32_t, void *);
static void *originals[7];

static int service_callbacks(void *service, void *callbacks, void *event)
{
    note("setup 1 BTServiceAddCallbacks enter");
    int result = ((service_callbacks_fn)originals[0])(service, callbacks, event);
    note("setup 1 BTServiceAddCallbacks result=%d%s", result, result ? " FAIL" : "");
    return result;
}
static int local_default(void *session, void *output)
{
    note("setup 2 BTLocalDeviceGetDefault enter");
    int result = ((default_fn)originals[1])(session, output);
    note("setup 2 BTLocalDeviceGetDefault result=%d%s", result, result ? " FAIL" : "");
    return result;
}
static int local_callbacks(void *device, void *callbacks, void *manager)
{
    note("setup 3 BTLocalDeviceAddCallbacks enter");
    int result = ((service_callbacks_fn)originals[2])(device, callbacks, manager);
    note("setup 3 BTLocalDeviceAddCallbacks result=%d%s", result, result ? " FAIL" : "");
    return result;
}
static int accessory_default(void *session, void *output)
{
    note("setup 4 BTAccessoryManagerGetDefault enter");
    int result = ((default_fn)originals[3])(session, output);
    note("setup 4 BTAccessoryManagerGetDefault result=%d%s", result, result ? " FAIL" : "");
    return result;
}
static int accessory_callbacks(void *manager, void *callbacks, void *context)
{
    note("setup 5 BTAccessoryManagerAddCallbacks enter");
    int result = ((service_callbacks_fn)originals[4])(manager, callbacks, context);
    note("setup 5 BTAccessoryManagerAddCallbacks result=%d%s", result, result ? " FAIL" : "");
    return result;
}
static int session_attach(uintptr_t queue, uintptr_t callback, uintptr_t context, uintptr_t options)
{
    note("BTSessionAttachWithQueue enter");
    int result = ((attach_fn)originals[5])(queue, callback, context, options);
    note("BTSessionAttachWithQueue result=%d%s", result, result ? " FAIL" : "");
    return result;
}
static void session_event(void *service, uint32_t first, uint32_t second, void *manager)
{
    note("session callback status1=%u status2=%u%s", first, second,
         (first | second) ? "" : " setup-eligible");
    ((event_fn)originals[6])(service, first, second, manager);
}

struct hook {
    const char *name;
    uint64_t va;
    uint32_t words[4];
    void *replacement;
};
static struct hook hooks[] = {
    {"BTAccessoryManagerAddCallbacks", UINT64_C(0x1b0900a78),
     {0xd10283ff,0xa90567fa,0xa9065ff8,0xa90757f6}, accessory_callbacks},
    {"BTAccessoryManagerGetDefault", UINT64_C(0x1b08fdee8),
     {0xd10343ff,0xa9095ff8,0xa90a57f6,0xa90b4ff4}, accessory_default},
    {"BTLocalDeviceAddCallbacks", UINT64_C(0x1b0918c30),
     {0xd10283ff,0xa90567fa,0xa9065ff8,0xa90757f6}, local_callbacks},
    {"BTLocalDeviceGetDefault", UINT64_C(0x1b08fdad4),
     {0xd10383ff,0xa90967fa,0xa90a5ff8,0xa90b57f6}, local_default},
    {"BTServiceAddCallbacks", UINT64_C(0x1b08fde4c),
     {0xa9bd57f6,0xa9014ff4,0xa9027bfd,0x910083fd}, service_callbacks},
    {"BTSessionAttachWithQueue", UINT64_C(0x1b08fd88c),
     {0xd10183ff,0xa9025ff8,0xa90357f6,0xa9044ff4}, session_attach},
    {"__btSessionEventCallback", CALLBACK_VA,
     {0xd10243ff,0xa9055ff8,0xa90657f6,0xa9074ff4}, session_event},
};
static struct native_image manager_image, mobile_image;

static void jump(uint32_t *out, uintptr_t target)
{
    out[0] = 0x58000050; out[1] = 0xd61f0200;
    out[2] = (uint32_t)target; out[3] = (uint32_t)((uint64_t)target >> 32);
}
static void *trampoline(uintptr_t at, const struct hook *hook, uintptr_t native)
{
    uint32_t words[8]; memcpy(words, hook->words, sizeof(words));
    jump(words + 4, native + 16);
    memcpy((void *)at, words, sizeof(words));
    return (void *)at;
}

__attribute__((used, aligned(16384), section("__TEXT,__x9tramp")))
static const uint32_t tramp_area[4096] = {0};

static int install(void)
{
    const void *main_base = NULL;
    uint32_t count = _dyld_image_count(), index;
    for (index = 0; index < count; index++) {
        const struct mach_header_64 *header = (const struct mach_header_64 *)_dyld_get_image_header(index);
        if (header && header->magic == MH_MAGIC_64 && header->filetype == MH_EXECUTE) {
            main_base = header; break;
        }
    }
    if (!main_base || !image_uuid(main_base, prefs_uuid)) {
        note("Preferences UUID/header rejected; no text changed"); return -1;
    }
    if (framework_image("/System/Library/PrivateFrameworks/MobileBluetooth.framework/MobileBluetooth",
                        mobile_uuid, &mobile_image) ||
        framework_image("/System/Library/PrivateFrameworks/BluetoothManager.framework/BluetoothManager",
                        manager_uuid, &manager_image)) return -1;
    const char *exports[] = {
        "BTAccessoryManagerAddCallbacks", "BTAccessoryManagerGetDefault",
        "BTLocalDeviceAddCallbacks", "BTLocalDeviceGetDefault",
        "BTServiceAddCallbacks", "BTSessionAttachWithQueue",
    };
    Dl_info info = {0};
    for (size_t k = 0; k < sizeof(hooks)/sizeof(hooks[0]); k++) {
        const struct native_image *image = k < 6 ? &mobile_image : &manager_image;
        uintptr_t expected = (uintptr_t)(image->slide + (int64_t)hooks[k].va);
        void *symbol = k < 6 ? dlsym(RTLD_DEFAULT, exports[k]) : (void *)expected;
        if (!symbol || !dladdr(symbol, &info) ||
            (uintptr_t)symbol != expected || (uintptr_t)info.dli_fbase != image->base) {
            note("image/slide mismatch %s; no text changed", hooks[k].name); return -1;
        }
        if (memcmp((void *)expected, hooks[k].words, sizeof(hooks[k].words))) {
            note("instruction mismatch %s; no text changed", hooks[k].name); return -1;
        }
    }
    vm_size_t page = vm_page_size;
    uintptr_t tramp = (uintptr_t)tramp_area;
    if (page > sizeof(tramp_area) || (tramp & (page-1)) ||
        sizeof(hooks)/sizeof(hooks[0]) * 32 > sizeof(tramp_area)) {
        note("trampoline alignment rejected; no text changed"); return -1;
    }
    kern_return_t kr = vm_protect(mach_task_self(), tramp, page, FALSE,
                                  VM_PROT_READ|VM_PROT_WRITE|VM_PROT_COPY);
    if (kr) { note("trampoline COW failed kr=%d; no text changed", kr); return -1; }
    for (size_t k = 0; k < sizeof(hooks)/sizeof(hooks[0]); k++) {
        const struct native_image *image = k < 6 ? &mobile_image : &manager_image;
        originals[k] = trampoline(tramp + k*32, &hooks[k],
                                  (uintptr_t)(image->slide + (int64_t)hooks[k].va));
    }
    sys_icache_invalidate((void *)tramp, sizeof(hooks)/sizeof(hooks[0]) * 32);
    kr = vm_protect(mach_task_self(), tramp, page, FALSE, VM_PROT_READ|VM_PROT_EXECUTE);
    if (kr) { note("trampoline R|X failed kr=%d; no text changed", kr); return -1; }
    uintptr_t pages[sizeof(hooks)/sizeof(hooks[0])]; size_t page_count = 0;
    for (size_t k = 0; k < sizeof(hooks)/sizeof(hooks[0]); k++) {
        const struct native_image *image = k < 6 ? &mobile_image : &manager_image;
        uintptr_t address = (uintptr_t)(image->slide + (int64_t)hooks[k].va);
        uintptr_t target = address & ~(page-1);
        size_t j; for (j = 0; j < page_count && pages[j] != target; j++) {}
        if (j != page_count) continue;
        kr = vm_protect(mach_task_self(), target, page, FALSE,
                        VM_PROT_READ|VM_PROT_WRITE|VM_PROT_COPY);
        if (kr) {
            for (j = 0; j < page_count; j++)
                if (vm_protect(mach_task_self(), pages[j], page, FALSE,
                               VM_PROT_READ|VM_PROT_EXECUTE)) abort();
            note("text COW failed kr=%d; no text changed", kr); return -1;
        }
        pages[page_count++] = target;
    }
    for (size_t k = 0; k < sizeof(hooks)/sizeof(hooks[0]); k++) {
        const struct native_image *image = k < 6 ? &mobile_image : &manager_image;
        uintptr_t address = (uintptr_t)(image->slide + (int64_t)hooks[k].va);
        uint32_t words[4]; jump(words, (uintptr_t)hooks[k].replacement);
        memcpy((void *)address, words, sizeof(words));
        sys_icache_invalidate((void *)address, sizeof(words));
    }
    for (size_t j = 0; j < page_count; j++) {
        kr = vm_protect(mach_task_self(), pages[j], page, FALSE, VM_PROT_READ|VM_PROT_EXECUTE);
        if (kr) { note("text R|X failed kr=%d", kr); abort(); }
    }
    note("seven native hooks installed; results and manager state unchanged");
    return 0;
}

__attribute__((constructor)) static void settings_bt_diagnostic(void)
{
    if (strcmp(getprogname(), "Preferences")) return;
    const char *enabled = getenv("X9_BT_SETTINGS_DIAG");
    if (!enabled || strcmp(enabled, "1")) return;
    (void)install();
    _dyld_register_func_for_add_image(settings_image_added);
}
