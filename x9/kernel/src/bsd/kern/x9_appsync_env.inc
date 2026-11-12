/* Included after existing read-only BBFW equality helpers. Runtime files must
 * be separately staged with consent; absent/mismatched files never get injected.
 * Header contains only signed-runtime hashes, not per-application allowlists.
 */
struct x9_appsync_file { const char *path; uint32_t size; uint8_t sha[32]; };
#include "../../../generated/x9_appsync_runtime.inc"

static int
x9_appsync_exec_env(struct image_params *imgp, int ptr_size)
{
    (void)ptr_size; /* hashless/default build has no environment insertion */
    if (!sweet_early_is_x9() || sweet_x9_arg("appsync", 0) != 1) return 0;
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
        if (x9_bbfw_equal(f->path, f->sha, f->size)) {
            printf("X9 APPSYNC: runtime missing/changed; %s left unmodified\n", executable);
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
