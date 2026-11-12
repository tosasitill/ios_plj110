/* Included after existing read-only BBFW equality helpers. Runtime files must
 * be separately staged with consent; absent/mismatched files never get injected.
 * Header contains only signed-runtime hashes, not per-application allowlists.
 */
struct x9_appsync_file { const char *path; uint32_t size; uint8_t sha[32]; };
#include "../../../generated/x9_appsync_runtime.inc"

/* On by default when the build carries a runtime manifest (the boot-args are at
 * their 607-byte limit); appsync=0 turns it off. Injection still requires the
 * exact runtime files on the root volume, so an unstaged device is unaffected. */
static int
x9_appsync_enabled(void)
{
    return sweet_early_is_x9() && sweet_x9_arg("appsync", X9_APPSYNC_RUNTIME_READY) == 1;
}

#if X9_APPSYNC_RUNTIME_READY
/* Like x9_bbfw_equal (size + SHA256, regular file, root:wheel) but the runtime
 * ships executables and dylibs as 0755, so only group/other write is refused
 * instead of demanding exactly 0644. */
static int
x9_appsync_file_equal(const char *path, const uint8_t digest[32], uint32_t size)
{
    vnode_t vp = NULLVP;
    vfs_context_t ctx = vfs_context_kernel();
    int error = vnode_lookup(path, VNODE_LOOKUP_NOFOLLOW, &vp, ctx);
    if (error) return error;
    if (!vnode_isreg(vp)) { vnode_put(vp); return EEXIST; }
    struct vnode_attr va;
    VATTR_INIT(&va);
    VATTR_WANTED(&va, va_data_size); VATTR_WANTED(&va, va_mode);
    VATTR_WANTED(&va, va_uid); VATTR_WANTED(&va, va_gid);
    error = vnode_getattr(vp, &va, ctx);
    if (!error && (va.va_data_size != size || (va.va_mode & 07022) || va.va_uid || va.va_gid)) error = EPERM;
    SHA256_CTX hash; uint8_t buffer[4096], sum[32];
    SHA256_Init(&hash);
    for (uint32_t off = 0; !error && off < size;) {
        int n = (int)((size - off) < sizeof(buffer) ? size - off : sizeof(buffer)), resid = 0;
        error = vn_rdwr(UIO_READ, vp, (caddr_t)buffer, n, off, UIO_SYSSPACE, 0, vfs_context_ucred(ctx), &resid, current_proc());
        if (!error && resid) error = EIO;
        if (!error) SHA256_Update(&hash, buffer, n);
        off += (uint32_t)n;
    }
    SHA256_Final(sum, &hash);
    if (!error && memcmp(sum, digest, 32)) error = EEXIST;
    vnode_put(vp);
    return error;
}

#endif

static int
x9_appsync_exec_env(struct image_params *imgp, int ptr_size)
{
    (void)ptr_size; /* hashless/default build has no environment insertion */
    if (!x9_appsync_enabled()) return 0;
    const char *executable = imgp->ip_strings + strlen(EXECUTABLE_KEY);
    if (strcmp(executable, "/usr/libexec/installd") &&
        strcmp(executable, "/System/Library/CoreServices/SpringBoard.app/SpringBoard") &&
        strcmp(executable, "/System/Library/PrivateFrameworks/MobileInstallation.framework/XPCServices/"
                           "com.apple.MobileInstallationHelperService.xpc/com.apple.MobileInstallationHelperService")) return 0;
    const char *s = imgp->ip_endargv;
    for (int i = 0; i < imgp->ip_envc; ++i, s += strlen(s) + 1)
        if (!strncmp(s, "DYLD_INSERT_LIBRARIES=", 22)) {
            printf("X9 APPSYNC: existing injection chain preserved; not appended\n");
            return 0;
        }
#if X9_APPSYNC_RUNTIME_READY
    for (size_t i = 0; i < sizeof(x9_appsync_files)/sizeof(x9_appsync_files[0]); ++i) {
        const struct x9_appsync_file *f = &x9_appsync_files[i];
        int ferr = x9_appsync_file_equal(f->path, f->sha, f->size);
        if (ferr) {
            printf("X9 APPSYNC: runtime missing/changed (%s errno=%d); %s left unmodified\n", f->path, ferr, executable);
            return 0;
        }
    }
    static const char env[] = "DYLD_INSERT_LIBRARIES=" X9_APPSYNC_LIBRARY;
    if (imgp->ip_argspace < sizeof(env) + (size_t)ptr_size) return E2BIG;
    int error = exec_add_user_string(imgp, CAST_USER_ADDR_T(env), UIO_SYSSPACE, TRUE);
    if (!error) {
        imgp->ip_argspace -= ptr_size;
        ++imgp->ip_envc;
        printf("X9 APPSYNC: verified runtime environment appended to %s\n", executable);
    }
    return error;
#else
    printf("X9 APPSYNC: no runtime manifest; %s left unmodified\n", executable);
    return 0;
#endif
}
