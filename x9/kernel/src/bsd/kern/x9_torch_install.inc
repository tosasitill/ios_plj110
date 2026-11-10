/* Built into kern_exec after the existing non-destructive BBFW file helpers.
 * Publish only our hash-addressed dylib; never overwrite an Apple file, plist,
 * existing different file or symlink. No mailbox/partition layout changes.
 */
#include "../../../generated/x9_torch_bridge.inc"
static bool x9_torch_bridge_installed;
static int x9_torch_install(proc_t p, user_addr_t scratch, user_addr_t end)
{
    if (!sweet_early_is_x9() || sweet_x9_arg("x9_torch", 0) != 3 || proc_getpid(p) != 1) return 0;
    if (!sweet_x9_ufs_root_active() || !sweet_x9_ufs_root_writable()) return EROFS;
    int e = x9_bbfw_directory(p, scratch, end, "/private/var/x9/torch"); if (e) return e;
    SHA256_CTX h; uint8_t hash[32]; SHA256_Init(&h);
    SHA256_Update(&h, x9_torch_bridge_data, sizeof(x9_torch_bridge_data)); SHA256_Final(hash, &h);
    if (memcmp(hash, x9_torch_bridge_sha, 32)) return EIO;
    int created = 0;
    e = x9_bbfw_write(p, scratch, end, X9_TORCH_BRIDGE_PATH, x9_torch_bridge_data,
                      sizeof(x9_torch_bridge_data), x9_torch_bridge_sha, &created);
    if (!e) e = x9_bbfw_equal(X9_TORCH_BRIDGE_PATH, x9_torch_bridge_sha, sizeof(x9_torch_bridge_data));
    if (!e) __atomic_store_n(&x9_torch_bridge_installed, true, __ATOMIC_RELEASE);
    printf("X9 TORCH: bridge install created=%d errno=%d (own-code only)\n", created, e);
    return e;
}
static int x9_torch_exec_env(struct image_params *imgp, int ptr_size)
{
    if (!sweet_early_is_x9() || sweet_x9_arg("x9_torch", 0) != 3 ||
        !__atomic_load_n(&x9_torch_bridge_installed, __ATOMIC_ACQUIRE) ||
        strcmp(imgp->ip_strings + strlen(EXECUTABLE_KEY), "/usr/sbin/mediaserverd")) return 0;
    /* An explicitly supplied injection chain belongs to its caller. Do not
     * append a duplicate DYLD key (dyld's first/last-key behavior can differ). */
    const char *s = imgp->ip_endargv;
    for (int i = 0; i < imgp->ip_envc; i++, s += strlen(s) + 1)
        if (!strncmp(s, "DYLD_INSERT_LIBRARIES=", 22)) return 0;
    if (x9_bbfw_equal(X9_TORCH_BRIDGE_PATH, x9_torch_bridge_sha, sizeof(x9_torch_bridge_data))) {
        printf("X9 TORCH: bridge changed/unreadable; mediaserverd left unmodified\n"); return 0;
    }
    static const char env[] = "DYLD_INSERT_LIBRARIES=" X9_TORCH_BRIDGE_PATH;
    if (imgp->ip_argspace < sizeof(env) + (size_t)ptr_size) return E2BIG;
    int e = exec_add_user_string(imgp, CAST_USER_ADDR_T(env), UIO_SYSSPACE, TRUE);
    if (!e) { imgp->ip_argspace -= ptr_size; imgp->ip_envc++;
        printf("X9 TORCH: mediaserverd bridge environment appended\n"); }
    return e;
}
