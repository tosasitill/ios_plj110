/* X9/iOS16 adaptation of sheng's user-app / platform separation.
 * No trust-cache-v2 or SPTM transplant: SWEET already has its own non-enforcing
 * AMFI policy. Ordinary user apps must not inherit the blanket system-platform
 * designation (including hardened platform exception-port restrictions).
 * No new root writes, Apple executable patches or fabricated pairing state.
 */
extern int sweet_early_is_x9(void);
extern int sweet_x9_arg(const char *, int);
static int x9_appsync_enabled(void);   /* x9_appsync_env.inc */
static bool
x9_appsync_user_path(const char *path)
{
    if (!path) return false;
    const char *part;
    if (!strncmp(path, "/private/var/", 13)) part = path + 13;
    else if (!strncmp(path, "/var/", 5)) part = path + 5;
    else return false;
    bool bundle = false;
    const char *end;
    while ((end = strchr(part, '/')) != NULL) {
        size_t n = (size_t)(end - part);
        if (!n || (n == 1 && part[0] == '.') ||
            (n == 2 && part[0] == '.' && part[1] == '.')) return false;
        if (n > 4 && !memcmp(end - 4, ".app", 4) && end[1]) bundle = true;
        part = end + 1;
    }
    return bundle && part[0] && strcmp(part, ".") && strcmp(part, "..");
}

static bool
x9_appsync_user_exec(struct image_params *imgp)
{
    if (!x9_appsync_enabled() || !imgp->ip_vp) return false;
    int length = MAXPATHLEN;
    char *path = kalloc_data(MAXPATHLEN, Z_WAITOK);
    if (!path) return false;
    bool user = !vn_getpath(imgp->ip_vp, path, &length) && x9_appsync_user_path(path);
    kfree_data(path, MAXPATHLEN);
    return user;
}
