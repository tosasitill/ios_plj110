// SPDX-License-Identifier: MIT
// Read-only environment check. No root password, SSH daemon or fabricated trust.
#import <Foundation/Foundation.h>
#import <unistd.h>
#import <sys/syscall.h>
#import <sys/sysctl.h>
#import <errno.h>
extern int __mac_syscall(const char *, int, void *);

int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc != 2 || strcmp(argv[1], "--status")) {
            fprintf(stderr, "usage: x9-jbctl --status\n");
            return 2;
        }
        uint64_t flags = 0;
        int result = __mac_syscall("AMFI", 0x60, &flags);
        printf("uid=%u AMFI.query=%d errno=%d flags=0x%llx\n", getuid(), result,
               result ? errno : 0, (unsigned long long)flags);
        const char *paths[] = {
            "/private/var/x9/jailbreak/20H392/lib/UniXNUAppSync.dylib",
            "/private/var/x9/jailbreak/20H392/lib/libunixnu-ldid.dylib",
            "/private/var/x9/jailbreak/20H392/bin/x9-appinst",
            "/bin/sh", "/usr/bin/dpkg", "/usr/bin/apt", "/usr/sbin/sshd"
        };
        for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i)
            printf("%s: %s\n", paths[i], access(paths[i], F_OK) ? "absent" : "present (not execution verification)");
        printf("Native USB/Trust/AppSync hook activation requires separate runtime evidence.\n");
        return result ? 1 : 0;
    }
}
