/* Opt-in 20H392 BTAudio XPC read-only recorder; native calls, values and flow stay unchanged.
 *
 * Injection point (offline evidence, 2026-11-28): BTAudioHALPlugin.driver's
 * Info.plist lists com.apple.BTAudioHALPlugin.xpc under
 * AudioServerPlugIn_MachServices, so the driver hosts the service inside
 * coreaudiod; the driver binary logs "BTHAL got kBTAudioMsgProperty..." and
 * imports exactly {apply, get_int64, get_string, get_value} on the read side,
 * while bluetoothd imports the full set_* family (it composes the envelopes).
 * The dictionaries are therefore read by the driver in coreaudiod. This
 * dylib rebinds only the pinned images' indirect pointers for those four
 * getters (driver UUID 9899D1C1-..., or bluetoothd UUID AA9424E8-... when
 * inserted there as the secondary, reply-side point); every replacement
 * forwards to the captured original first and only then records. Nothing is
 * written back into any dictionary, no return value is altered, and every
 * recording failure path falls open to the native call. */
#include <dlfcn.h>
#include <errno.h>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <os/log.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

/* Minimal C libxpc ABI declarations, same precedent as X9XPC16.h: Xcode's
 * iPhoneOS app SDK omits xpc.h. Signatures follow the MacOSX SDK xpc/xpc.h.
 * This file defines no libxpc structure layout. Type identity is probed at
 * runtime from created objects, so no _xpc_type_* symbol is referenced. */
typedef void *xpc_object_t;
typedef const struct _xpc_type_s *xpc_type_t;
extern xpc_type_t xpc_get_type(xpc_object_t);
extern void xpc_release(xpc_object_t);
extern xpc_object_t xpc_dictionary_create(const char *const *, const xpc_object_t *, size_t);
extern xpc_object_t xpc_dictionary_get_value(xpc_object_t, const char *);
extern bool xpc_dictionary_apply(xpc_object_t, bool (^)(const char *, xpc_object_t));
extern size_t xpc_dictionary_get_count(xpc_object_t);
extern xpc_object_t xpc_array_create(const xpc_object_t *, size_t);
extern size_t xpc_array_get_count(xpc_object_t);
extern xpc_object_t xpc_array_get_value(xpc_object_t, size_t);
extern xpc_object_t xpc_bool_create(bool);
extern bool xpc_bool_get_value(xpc_object_t);
extern xpc_object_t xpc_int64_create(int64_t);
extern int64_t xpc_int64_get_value(xpc_object_t);
extern xpc_object_t xpc_uint64_create(uint64_t);
extern uint64_t xpc_uint64_get_value(xpc_object_t);
extern xpc_object_t xpc_double_create(double);
extern double xpc_double_get_value(xpc_object_t);
extern xpc_object_t xpc_string_create(const char *);
extern const char *xpc_string_get_string_ptr(xpc_object_t);
extern xpc_object_t xpc_data_create(const void *, size_t);
extern size_t xpc_data_get_length(xpc_object_t);
extern const void *xpc_data_get_bytes_ptr(xpc_object_t);
extern xpc_object_t xpc_date_create(int64_t);
extern int64_t xpc_date_get_value(xpc_object_t);
extern xpc_object_t xpc_uuid_create(const uint8_t *);
extern const uint8_t *xpc_uuid_get_bytes(xpc_object_t);
extern xpc_object_t xpc_shmem_create(void *, size_t);
extern size_t xpc_shmem_map(xpc_object_t, void **);

#define DRIVER_UUID "9899D1C1-2F49-3AAB-9223-DFAD564CFE2E"
#define BTDAEMON_UUID "AA9424E8-0648-34E2-B3CF-E6C793B9D471"
static const uint8_t driver_uuid[16] = {0x98,0x99,0xd1,0xc1,0x2f,0x49,0x3a,0xab,
                                        0x92,0x23,0xdf,0xad,0x56,0x4c,0xfe,0x2e};
static const uint8_t daemon_uuid[16] = {0xaa,0x94,0x24,0xe8,0x06,0x48,0x34,0xe2,
                                        0xb3,0xcf,0xe6,0xc7,0x93,0xb9,0xd4,0x71};

#define MAX_DUMPS 16
#define DUMP_LINE_CAP 220
#define NOTE_CAP 8192
#define SEEN_SLOTS 256
#define SEEN_WINDOW_SEC 2

static _Atomic unsigned notes;
static _Atomic unsigned dumps;

static void note(const char *format, ...)
{
    int saved_errno = errno;
    if (atomic_fetch_add_explicit(&notes, 1, memory_order_relaxed) >= NOTE_CAP) {
        errno = saved_errno;
        return;
    }
    char text[288]; va_list args; va_start(args, format);
    vsnprintf(text, sizeof(text), format, args); va_end(args);
    size_t length = strlen(text);
    while (length && text[length-1] == '\n') text[--length] = 0;
    (void)sysctlbyname("kern.x9_bt_note", NULL, NULL, text, length + 1);
    os_log_error(OS_LOG_DEFAULT, "X9 BTAUDIO: %{public}s", text);
    errno = saved_errno;
}

/* ---- recorded-type identity, probed once from created objects ---- */
static xpc_type_t type_dictionary, type_array, type_bool, type_data, type_date,
                  type_double, type_int64, type_shmem, type_string, type_uint64,
                  type_uuid;
static char probe_byte;
static const uint8_t probe_uuid[16] = {0};

static xpc_type_t probe_type(xpc_object_t object)
{
    if (!object) return NULL;
    xpc_type_t type = xpc_get_type(object);
    xpc_release(object);
    return type;
}

static void probe_types(void)
{
    type_dictionary = probe_type(xpc_dictionary_create(NULL, NULL, 0));
    type_array = probe_type(xpc_array_create(NULL, 0));
    type_bool = probe_type(xpc_bool_create(false));
    type_data = probe_type(xpc_data_create("", 0));
    type_date = probe_type(xpc_date_create(0));
    type_double = probe_type(xpc_double_create(0));
    type_int64 = probe_type(xpc_int64_create(0));
    type_string = probe_type(xpc_string_create(""));
    type_uint64 = probe_type(xpc_uint64_create(0));
    type_shmem = probe_type(xpc_shmem_create(&probe_byte, 1));
    type_uuid = probe_type(xpc_uuid_create(probe_uuid));
}

/* ---- envelope dump ---- */
static _Thread_local int in_hook;

struct seen_entry { void *dict; int64_t msgid; time_t when; };
static struct seen_entry seen[SEEN_SLOTS];
static _Atomic unsigned seen_cursor;
/* Spinlock, not a pthread mutex: dumps are rare and short, and the static
 * mutex initializer would give this dylib a static-init section. */
static _Atomic int dump_spin;
static void dump_lock_acquire(void)
{
    while (atomic_exchange_explicit(&dump_spin, 1, memory_order_acquire)) {}
}
static void dump_lock_release(void)
{
    atomic_store_explicit(&dump_spin, 0, memory_order_release);
}

static int already_dumped(xpc_object_t dict, int64_t msgid)
{
    time_t now = time(NULL);
    for (unsigned i = 0; i < SEEN_SLOTS; i++)
        if (seen[i].dict == (void *)dict && seen[i].msgid == msgid &&
            now - seen[i].when < SEEN_WINDOW_SEC)
            return 1;
    unsigned slot = atomic_fetch_add_explicit(&seen_cursor, 1, memory_order_relaxed) % SEEN_SLOTS;
    seen[slot].dict = (void *)dict;
    seen[slot].msgid = msgid;
    seen[slot].when = now;
    return 0;
}

struct dump_ctx { int depth; int lines; int stop; char path[160]; };

static void dump_note(struct dump_ctx *ctx, const char *format, ...)
{
    if (ctx->lines >= DUMP_LINE_CAP) { ctx->stop = 1; return; }
    ctx->lines++;
    char text[288]; va_list args; va_start(args, format);
    vsnprintf(text, sizeof(text), format, args); va_end(args);
    note("%s", text);
    if (ctx->lines >= DUMP_LINE_CAP) ctx->stop = 1;
}

static void hex_into(char *out, size_t outcap, const uint8_t *bytes, size_t count)
{
    static const char digits[] = "0123456789abcdef";
    size_t at = 0;
    for (size_t i = 0; i < count && at + 2 < outcap; i++) {
        out[at++] = digits[bytes[i] >> 4];
        out[at++] = digits[bytes[i] & 15];
    }
    out[at] = 0;
}

static void dump_value(struct dump_ctx *ctx, const char *key, xpc_object_t value);

static void dump_dict(struct dump_ctx *ctx, xpc_object_t dict)
{
    dump_note(ctx, "dict %s keys=%zu", ctx->path[0] ? ctx->path : "(root)",
              xpc_dictionary_get_count(dict));
    struct dump_ctx *shared = ctx;
    xpc_dictionary_apply(dict, ^bool(const char *key, xpc_object_t value) {
        if (shared->stop) return false;
        dump_value(shared, key, value);
        return !shared->stop;
    });
}

static void dump_value(struct dump_ctx *ctx, const char *key, xpc_object_t value)
{
    if (ctx->stop) return;
    if (!value) { dump_note(ctx, "d%d %s = null", ctx->depth, key); return; }
    xpc_type_t type = xpc_get_type(value);
    if (type == type_dictionary) {
        if (ctx->depth >= 3) {
            dump_note(ctx, "d%d %s = dict keys=%zu (depth cap)", ctx->depth, key,
                      xpc_dictionary_get_count(value));
            return;
        }
        char saved[sizeof(ctx->path)];
        memcpy(saved, ctx->path, sizeof(saved));
        size_t used = strlen(ctx->path);
        snprintf(ctx->path + used, sizeof(ctx->path) - used, "%s%s",
                 used ? "." : "", key);
        ctx->depth++;
        dump_dict(ctx, value);
        ctx->depth--;
        memcpy(ctx->path, saved, sizeof(saved));
        return;
    }
    if (type == type_array) {
        size_t count = xpc_array_get_count(value);
        dump_note(ctx, "d%d %s = array count=%zu", ctx->depth, key, count);
        if (ctx->depth < 3)
            for (size_t i = 0; i < count && i < 8 && !ctx->stop; i++) {
                char index_key[32];
                snprintf(index_key, sizeof(index_key), "%s[%zu]", key, i);
                ctx->depth++;
                dump_value(ctx, index_key, xpc_array_get_value(value, i));
                ctx->depth--;
            }
        return;
    }
    if (type == type_int64) {
        dump_note(ctx, "d%d %s = i64 %lld", ctx->depth, key,
                  (long long)xpc_int64_get_value(value));
        return;
    }
    if (type == type_uint64) {
        dump_note(ctx, "d%d %s = u64 %llu", ctx->depth, key,
                  (unsigned long long)xpc_uint64_get_value(value));
        return;
    }
    if (type == type_double) {
        dump_note(ctx, "d%d %s = f64 %g", ctx->depth, key, xpc_double_get_value(value));
        return;
    }
    if (type == type_bool) {
        dump_note(ctx, "d%d %s = bool %d", ctx->depth, key,
                  xpc_bool_get_value(value) ? 1 : 0);
        return;
    }
    if (type == type_date) {
        dump_note(ctx, "d%d %s = date %lld", ctx->depth, key,
                  (long long)xpc_date_get_value(value));
        return;
    }
    if (type == type_string) {
        const char *text = xpc_string_get_string_ptr(value);
        dump_note(ctx, "d%d %s = str \"%.*s\"%s", ctx->depth, key,
                  120, text ? text : "(null)",
                  text && strlen(text) > 120 ? "..." : "");
        return;
    }
    if (type == type_data) {
        size_t length = xpc_data_get_length(value);
        const uint8_t *bytes = xpc_data_get_bytes_ptr(value);
        char head[33];
        hex_into(head, sizeof(head), bytes, length < 16 ? length : 16);
        dump_note(ctx, "d%d %s = data len=%zu head=%s", ctx->depth, key, length, head);
        return;
    }
    if (type == type_shmem) {
        void *region = NULL;
        size_t length = xpc_shmem_map(value, &region);
        char head[97];
        head[0] = 0;
        if (region && length)
            hex_into(head, sizeof(head), region, length < 48 ? length : 48);
        dump_note(ctx, "d%d %s = shmem len=%zu head=%s", ctx->depth, key,
                  region ? length : (size_t)0, head);
        return;
    }
    const uint8_t *uuid = type == type_uuid ? xpc_uuid_get_bytes(value) : NULL;
    if (uuid) {
        dump_note(ctx, "d%d %s = uuid %02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
                  "%02x%02x%02x%02x%02x%02x", ctx->depth, key,
                  uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6],
                  uuid[7], uuid[8], uuid[9], uuid[10], uuid[11], uuid[12],
                  uuid[13], uuid[14], uuid[15]);
        return;
    }
    dump_note(ctx, "d%d %s = other type=%p", ctx->depth, key, (const void *)type);
}

static void maybe_dump(xpc_object_t dict)
{
    if (!dict || in_hook) return;
    if (atomic_load_explicit(&dumps, memory_order_relaxed) >= MAX_DUMPS) return;
    in_hook = 1;
    xpc_object_t id = xpc_dictionary_get_value(dict, "kBTAudioMsgId");
    if (id && xpc_get_type(id) == type_int64) {
        dump_lock_acquire();
        if (!already_dumped(dict, xpc_int64_get_value(id)) &&
            atomic_fetch_add_explicit(&dumps, 1, memory_order_relaxed) < MAX_DUMPS) {
            const char *uid = NULL;
            xpc_object_t uid_value = xpc_dictionary_get_value(dict, "kBTAudioMsgDeviceUid");
            if (uid_value && xpc_get_type(uid_value) == type_string)
                uid = xpc_string_get_string_ptr(uid_value);
            int64_t method = -1;
            xpc_object_t method_value = xpc_dictionary_get_value(dict, "kBTAudioMsgMethod");
            if (method_value && xpc_get_type(method_value) == type_int64)
                method = xpc_int64_get_value(method_value);
            const char *process = NULL;
            xpc_object_t process_value = xpc_dictionary_get_value(dict, "kBTAudioMsgProcess");
            if (process_value && xpc_get_type(process_value) == type_string)
                process = xpc_string_get_string_ptr(process_value);
            note("msg dict=%p id=%lld uid=%s method=%lld process=%s",
                 (void *)dict, (long long)xpc_int64_get_value(id),
                 uid ? uid : "(null)", (long long)method,
                 process ? process : "(null)");
            struct dump_ctx ctx = {0};
            dump_dict(&ctx, dict);
            note("msg end dict=%p lines=%d", (void *)dict, ctx.lines);
        }
        dump_lock_release();
    }
    in_hook = 0;
}

/* ---- replacements: forward to the captured original, then record ---- */
typedef int64_t (*get_int64_fn)(xpc_object_t, const char *);
typedef const char *(*get_string_fn)(xpc_object_t, const char *);
typedef xpc_object_t (*get_value_fn)(xpc_object_t, const char *);
typedef bool (*apply_fn)(xpc_object_t, const void *);
static get_int64_fn orig_get_int64;
static get_string_fn orig_get_string;
static get_value_fn orig_get_value;
static apply_fn orig_apply;

static int64_t rep_get_int64(xpc_object_t dict, const char *key)
{
    int saved_errno = errno;
    int64_t result = orig_get_int64 ? orig_get_int64(dict, key) : 0;
    maybe_dump(dict);
    errno = saved_errno;
    return result;
}
static const char *rep_get_string(xpc_object_t dict, const char *key)
{
    int saved_errno = errno;
    const char *result = orig_get_string ? orig_get_string(dict, key) : NULL;
    maybe_dump(dict);
    errno = saved_errno;
    return result;
}
static xpc_object_t rep_get_value(xpc_object_t dict, const char *key)
{
    int saved_errno = errno;
    xpc_object_t result = orig_get_value ? orig_get_value(dict, key) : NULL;
    maybe_dump(dict);
    errno = saved_errno;
    return result;
}
static bool rep_apply(xpc_object_t dict, const void *applier)
{
    int saved_errno = errno;
    in_hook++;
    bool result = orig_apply ? orig_apply(dict, applier) : false;
    in_hook--;
    maybe_dump(dict);
    errno = saved_errno;
    return result;
}

/* ---- indirect-pointer rebinding in one pinned image ---- */
struct target { const char *name; void *replacement; void **original; };
static struct target targets[] = {
    {"_xpc_dictionary_get_int64", (void *)rep_get_int64, (void **)&orig_get_int64},
    {"_xpc_dictionary_get_string", (void *)rep_get_string, (void **)&orig_get_string},
    {"_xpc_dictionary_get_value", (void *)rep_get_value, (void **)&orig_get_value},
    {"_xpc_dictionary_apply", (void *)rep_apply, (void **)&orig_apply},
};

static int image_uuid_is(const void *base, const uint8_t expected[16])
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

static int write_slot(void **slot, void *value)
{
    uintptr_t page = (uintptr_t)slot & ~(uintptr_t)(vm_page_size - 1);
    kern_return_t kr = vm_protect(mach_task_self(), page, vm_page_size, FALSE,
                                  VM_PROT_READ | VM_PROT_WRITE);
    if (kr) return -1;
    *slot = value;
    (void)vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ);
    return 0;
}

static int rebind_image(const struct mach_header_64 *header, intptr_t slide)
{
    const struct symtab_command *symtab = NULL;
    const struct dysymtab_command *dysymtab = NULL;
    const struct segment_command_64 *linkedit = NULL;
    const struct section_64 *sections[8];
    unsigned section_count = 0;
    const uint8_t *at = (const uint8_t *)(header + 1);
    for (uint32_t i = 0; i < header->ncmds; i++) {
        const struct load_command *lc = (const struct load_command *)at;
        if (lc->cmdsize < sizeof(*lc)) return 0;
        if (lc->cmd == LC_SYMTAB) symtab = (const struct symtab_command *)lc;
        else if (lc->cmd == LC_DYSYMTAB) dysymtab = (const struct dysymtab_command *)lc;
        else if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *segment = (const struct segment_command_64 *)lc;
            if (!strcmp(segment->segname, "__LINKEDIT")) linkedit = segment;
            const struct section_64 *section = (const struct section_64 *)(segment + 1);
            for (uint32_t j = 0; j < segment->nsects; j++, section++)
                if ((!strcmp(section->sectname, "__la_symbol_ptr") ||
                     !strcmp(section->sectname, "__got")) &&
                    section_count < sizeof(sections)/sizeof(sections[0]))
                    sections[section_count++] = section;
        }
        at += lc->cmdsize;
    }
    if (!symtab || !dysymtab || !linkedit || !section_count) return 0;
    uintptr_t base = (uintptr_t)slide + linkedit->vmaddr - linkedit->fileoff;
    const struct nlist_64 *symbols = (const void *)(base + symtab->symoff);
    const char *strings = (const void *)(base + symtab->stroff);
    const uint32_t *indirect = (const void *)(base + dysymtab->indirectsymoff);
    int rebound = 0;
    for (unsigned s = 0; s < section_count; s++) {
        const struct section_64 *section = sections[s];
        const uint32_t *indices = indirect + section->reserved1;
        void **slots = (void **)((uintptr_t)slide + section->addr);
        size_t count = section->size / sizeof(void *);
        for (size_t j = 0; j < count; j++) {
            uint32_t index = indices[j];
            if (index & 0xc0000000u) continue; /* INDIRECT_SYMBOL_LOCAL/ABS */
            if (index >= symtab->nsyms) continue;
            uint32_t offset = symbols[index].n_un.n_strx;
            if (!offset || offset >= symtab->strsize) continue;
            const char *name = strings + offset;
            for (size_t t = 0; t < sizeof(targets)/sizeof(targets[0]); t++) {
                if (strcmp(name, targets[t].name)) continue;
                void *current = slots[j];
                if (!current || current == targets[t].replacement) continue;
                if (!*targets[t].original) *targets[t].original = current;
                if (!write_slot(&slots[j], targets[t].replacement)) rebound++;
            }
        }
    }
    return rebound;
}

static void on_image(const struct mach_header *header, intptr_t slide)
{
    const char *which = NULL;
    if (image_uuid_is(header, driver_uuid)) which = "BTAudioHALPlugin";
    else if (image_uuid_is(header, daemon_uuid)) which = "bluetoothd";
    else return;
    int rebound = rebind_image((const struct mach_header_64 *)header, slide);
    note("hooked %s image=%p rebound=%d of %zu; values and flow unchanged",
         which, (const void *)header, rebound, sizeof(targets)/sizeof(targets[0]));
}

__attribute__((constructor)) static void bt_audio_diagnostic(void)
{
    const char *prog = getprogname();
    if (strcmp(prog, "coreaudiod") && strcmp(prog, "bluetoothd")) return;
    const char *enabled = getenv("X9_BT_AUDIO_DIAG");
    if (!enabled || strcmp(enabled, "1")) return;
    probe_types();
    _dyld_register_func_for_add_image(on_image);
    note("recorder armed process=%s; values and flow unchanged", prog);
}
