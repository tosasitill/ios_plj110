/* Opt-in own-code VT bridge. Never alter an Apple library or launch plist.
 * Included after BBFW helpers. Publication is distinct from native readiness. */
#include "../../../generated/x9_hevc_bridge.inc"
extern int x9_vdec_autoload_available(void);
static bool x9_hevc_bridge_installed;

static int
x9_hevc_trusted_directory(const char *path)
{
    vnode_t vp = NULLVP;
    vfs_context_t ctx = vfs_context_kernel();
    int e = vnode_lookup(path, VNODE_LOOKUP_NOFOLLOW, &vp, ctx);
    if (e) return e;
    if (!vnode_isdir(vp)) { vnode_put(vp); return ENOTDIR; }
    struct vnode_attr va;
    VATTR_INIT(&va);
    VATTR_WANTED(&va, va_mode);
    VATTR_WANTED(&va, va_uid);
    VATTR_WANTED(&va, va_gid);
    e = vnode_getattr(vp, &va, ctx);
    if (!e && (va.va_uid || va.va_gid || (va.va_mode & 07022))) e = EPERM;
    vnode_put(vp);
    return e;
}

static int
x9_hevc_directories(proc_t p, user_addr_t scratch, user_addr_t end, bool create)
{
    static const char *const dirs[] = {
        "/private", "/private/var", "/private/var/x9", "/private/var/x9/hevc"
    };
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i) {
        /* Each parent is verified before creating its child. Existing symlinks
         * and writable/non-root parents are never repaired or traversed. */
        int e = x9_hevc_trusted_directory(dirs[i]);
        if (e == ENOENT && create && i >= 2) {
            e = x9_update_directory(p, scratch, end, dirs[i]);
            if (!e) e = x9_hevc_trusted_directory(dirs[i]);
        }
        if (e) return e;
    }
    return 0;
}

static int
x9_hevc_install(proc_t p, user_addr_t scratch, user_addr_t end)
{
    if (!sweet_early_is_x9() || sweet_x9_arg("x9_hevc", 0) != 1 || proc_getpid(p) != 1)
        return 0;
    if (!sweet_x9_ufs_root_active() || !sweet_x9_ufs_root_writable()) return EROFS;
    SHA256_CTX hash;
    uint8_t sum[32];
    SHA256_Init(&hash);
    SHA256_Update(&hash, x9_hevc_bridge_data, sizeof(x9_hevc_bridge_data));
    SHA256_Final(sum, &hash);
    if (memcmp(sum, x9_hevc_bridge_sha, sizeof(sum))) return EIO;
    int e = x9_hevc_directories(p, scratch, end, true);
    if (e) return e;
    int created = 0;
    e = x9_bbfw_write(p, scratch, end, X9_HEVC_BRIDGE_PATH, x9_hevc_bridge_data,
                      sizeof(x9_hevc_bridge_data), x9_hevc_bridge_sha, &created);
    if (!e) e = x9_bbfw_equal(X9_HEVC_BRIDGE_PATH, x9_hevc_bridge_sha,
                             sizeof(x9_hevc_bridge_data));
    if (!e) __atomic_store_n(&x9_hevc_bridge_installed, true, __ATOMIC_RELEASE);
    printf("X9 HEVC: own-code bridge install created=%d errno=%d; not hardware readiness\n",
           created, e);
    return e;
}

static bool
x9_hevc_target(const char *path)
{
    return !strcmp(path, "/usr/sbin/mediaserverd") ||
        !strcmp(path, "/Applications/MobileSlideShow.app/MobileSlideShow") ||
        !strcmp(path, "/System/Library/CoreServices/SpringBoard.app/SpringBoard");
}

/* Called AFTER exec_handle_sugid/final platform decision and BEFORE apple
 * strings and user argv/env pointers are constructed. Optional loading is
 * fail-open: failed qualification or insufficient budget leaves exec untouched.
 * This hook does not try to fix camera/torch's independent shared-IMP conflict. */
static void
x9_hevc_exec_env(struct image_params *imgp)
{
    if (!sweet_early_is_x9() || sweet_x9_arg("x9_hevc", 0) != 1 ||
        !__atomic_load_n(&x9_hevc_bridge_installed, __ATOMIC_ACQUIRE) ||
        !(imgp->ip_flags & IMGPF_IS_64BIT_ADDR) ||
        imgp->ip_origcputype != CPU_TYPE_ARM64 ||
        (imgp->ip_flags & IMGPF_INTERPRET) || imgp->ip_scriptvp ||
        !imgp->ip_vp || !imgp->ip_origvattr || !imgp->ip_vattr ||
        (imgp->ip_origvattr->va_mode & (VSUID | VSGID)) ||
        (imgp->ip_vattr->va_mode & 07022) || imgp->ip_vattr->va_uid ||
        imgp->ip_vattr->va_gid || !vnode_isreg(imgp->ip_vp)) return;
    proc_t p = vfs_context_proc(imgp->ip_vfs_context);
    if (p->p_flag & P_SUGID) return;
    kauth_cred_t cred = kauth_cred_proc_ref(p);
    uid_t uid = kauth_cred_getuid(cred);
    kauth_cred_unref(&cred);
    if (uid && uid != 501) return; /* Matches native device admission, no new grant. */
    const char *requested = imgp->ip_strings + strlen(EXECUTABLE_KEY);
    if (!x9_hevc_target(requested) || !x9_vdec_autoload_available()) return;
    char *resolved = kalloc_data(MAXPATHLEN, Z_WAITOK);
    if (!resolved) return;
    int length = MAXPATHLEN;
    bool target = !vn_getpath(imgp->ip_vp, resolved, &length) &&
                  !strcmp(requested, resolved);
    kfree_data(resolved, MAXPATHLEN);
    if (!target || x9_hevc_directories(p, 0, 0, false) ||
        x9_bbfw_equal(X9_HEVC_BRIDGE_PATH, x9_hevc_bridge_sha,
                      sizeof(x9_hevc_bridge_data))) return;

    static const char key[] = "DYLD_INSERT_LIBRARIES=";
    static const char path[] = X9_HEVC_BRIDGE_PATH;
    static const char env[] = "DYLD_INSERT_LIBRARIES=" X9_HEVC_BRIDGE_PATH;
    char *entry = NULL, *s = imgp->ip_endargv;
    unsigned keys = 0;
    /* Bound every scan by the already copied environment, not caller memory. */
    for (int i = 0; i < imgp->ip_envc; ++i) {
        if (s >= imgp->ip_endenvv) return;
        char *nul = memchr(s, 0, (size_t)(imgp->ip_endenvv - s));
        if (!nul) return;
        if ((size_t)(nul - s) >= sizeof(key) - 1 &&
            !memcmp(s, key, sizeof(key) - 1)) { entry = s; ++keys; }
        s = nul + 1;
    }
    /* Preserve the original first/last-key semantics for ambiguous input. */
    if (keys > 1 || imgp->ip_strendp != imgp->ip_endenvv ||
        s > imgp->ip_endenvv) return;
    size_t old_pad = (size_t)(imgp->ip_endenvv - s);
    const size_t pointer = sizeof(user64_addr_t);
    if (old_pad >= pointer || imgp->ip_strspace % pointer) return;
    char *insert = NULL;
    bool separator = false;
    size_t growth = sizeof(env);
    if (entry) {
        char *value = entry + sizeof(key) - 1;
        char *last = value + strlen(value);
        for (char *part = value; part < last;) {
            char *colon = memchr(part, ':', (size_t)(last - part));
            char *end = colon ? colon : last;
            if ((size_t)(end - part) == sizeof(path) - 1 &&
                !memcmp(part, path, sizeof(path) - 1)) return;
            part = colon ? colon + 1 : last;
        }
        insert = last;
        separator = last != value;
        growth = sizeof(path) - 1 + (separator ? 1 : 0);
    }
    /* The env NULL pointer is already charged by exec_extract_strings.
     * Reuse its old alignment padding, then charge the exact new string delta
     * plus one pointer only for a NEW key. No failing operation after commit. */
    size_t available = (size_t)imgp->ip_strspace + old_pad;
    if (growth > available) return;
    size_t pad = (available - growth) % pointer;
    size_t bytes = growth + pad - old_pad;
    size_t charge = bytes + (entry ? 0 : pointer);
    if ((size_t)imgp->ip_strspace < bytes || (size_t)imgp->ip_argspace < charge)
        return;
    if (entry) {
        memmove(insert + growth, insert, (size_t)(s - insert));
        if (separator) *insert++ = ':';
        memcpy(insert, path, sizeof(path) - 1); /* Retain the moved original NUL. */
    } else {
        memcpy(s, env, sizeof(env));
        ++imgp->ip_envc;
    }
    memset(s + growth, 0, pad);
    imgp->ip_strendp = s + growth + pad;
    imgp->ip_endenvv = imgp->ip_strendp;
    imgp->ip_strspace -= bytes;
    imgp->ip_argspace -= charge;
    printf("X9 HEVC: verified native-capability bridge environment merged for %s\n", requested);
}
