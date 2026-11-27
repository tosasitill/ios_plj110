/* Caller verification shared by x9-rootexec and SileoRootWrapper.
 *
 * The caller is the parent process. It is authorized iff its executable
 * path matches a table entry AND the file's SHA256 matches the pinned
 * hash (the userland echo of the kernel's exec-time hash verification;
 * the residual TOCTOU window is the same class the AppSync runtime
 * accepts with its repair-style verifier). An entry with a NULL hash is
 * an unpinned placeholder and NEVER authorizes (fail closed).
 */
#ifndef X9_CALLER_CHECK_H
#define X9_CALLER_CHECK_H

#include <stdio.h>
#include <string.h>
#include <unistd.h>
/* proc_pidpath is SPI on iOS (no public header in the SDK); the
 * symbol is exported by libsystem_kernel in 20H392 (verified
 * against the DSC extraction). */
#define PROC_PIDPATHINFO_MAXSIZE 4096
int proc_pidpath(int pid, void *buffer, unsigned int buffersize);
#include <CommonCrypto/CommonDigest.h>

typedef struct {
    const char *path;
    const char *sha256_hex; /* NULL = unpinned, never authorizes */
} x9_caller_entry;

static int x9_file_sha256_hex(const char *path, char out[65]) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    CC_SHA256_CTX ctx;
    CC_SHA256_Init(&ctx);
    unsigned char buf[16384];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) CC_SHA256_Update(&ctx, buf, (CC_LONG)n);
    fclose(f);
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(digest, &ctx);
    static const char hexd[] = "0123456789abcdef";
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) {
        out[2 * i] = hexd[digest[i] >> 4];
        out[2 * i + 1] = hexd[digest[i] & 15];
    }
    out[64] = 0;
    return 0;
}

static int x9_caller_authorized(const x9_caller_entry *table, unsigned count,
                                char *caller_path_out, unsigned caller_path_size) {
    char path[PROC_PIDPATHINFO_MAXSIZE];
    if (proc_pidpath(getppid(), path, sizeof path) <= 0) return 0;
    if (caller_path_out) snprintf(caller_path_out, caller_path_size, "%s", path);
    for (unsigned i = 0; i < count; i++) {
        if (strcmp(path, table[i].path)) continue;
        if (!table[i].sha256_hex) return 0;
        char digest[65];
        if (x9_file_sha256_hex(path, digest)) return 0;
        return strcmp(digest, table[i].sha256_hex) == 0;
    }
    return 0;
}

#endif
