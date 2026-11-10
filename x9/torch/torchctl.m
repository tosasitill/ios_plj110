/* SPDX-License-Identifier: MIT. Manual on-device utility, not an auto-run job.
 * torchctl status
 * torchctl raw 250 wait 1 raw 0 status       (root; no mediaserverd writer)
 * torchctl av 0.25 wait 1 av 0 status        (real AVFlashlight/RPC path)
 */
#import <Foundation/Foundation.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>
#include <sys/ioctl.h>
#include "../kernel/src/bsd/dev/x9_torch.h"
static void status(void) {
    int fd = open(X9_TORCH_DEVICE, O_RDONLY); struct x9_torch_status s;
    if (fd < 0) { perror("open torch"); return; }
    if (ioctl(fd, X9_TORCH_GET_STATUS, &s)) perror("torch status");
    else printf("v%u mode=%u flags=%x ch=%u confirmed=%u current=%uuA max=%uuA/%us dev=%02x enable=%02x fault=%06x errno=%u\n",
                s.version,s.mode,s.flags,s.channel,s.level,s.current_ua,s.max_current_ua,s.max_seconds,
                s.device_info,s.enable,s.fault,s.last_errno);
    close(fd);
}
int main(int argc, char **argv) {
    @autoreleasepool {
        int fd = -1, rc = 0; id flashlight = nil;
        if (argc == 1) { status(); return 0; }
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "status")) { status(); continue; }
            if (i + 1 >= argc) { rc = 2; break; }
            const char *cmd = argv[i++]; char *end; double v = strtod(argv[i], &end);
            if (*end || end == argv[i] || !isfinite(v) || v < 0) { rc = 2; break; }
            if (!strcmp(cmd, "wait") && v <= 30) { usleep((useconds_t)(v * 1000000)); continue; }
            if (!strcmp(cmd, "raw") && v <= 1000 && v == floor(v)) {
                if (fd < 0) fd = open(X9_TORCH_DEVICE, O_WRONLY | O_CLOEXEC);
                uint32_t level = v;
                if (fd < 0 || write(fd, &level, sizeof(level)) != sizeof(level)) { perror("raw torch"); rc = 1; break; }
            } else if (!strcmp(cmd, "av") && v <= 1) {
                if (!flashlight) {
                    dlopen("/System/Library/Frameworks/AVFoundation.framework/AVFoundation", RTLD_NOW);
                    Class c = objc_getClass("AVFlashlight");
                    if (!c || !((BOOL (*)(id,SEL))objc_msgSend)(c, sel_registerName("hasFlashlight"))) {
                        fprintf(stderr,"AVFlashlight hasFlashlight=NO\n"); rc = 1; break;
                    }
                    flashlight = [[c alloc] init];
                }
                id error = nil;
                BOOL ok = ((BOOL (*)(id,SEL,float,id *))objc_msgSend)(flashlight,
                    sel_registerName("setFlashlightLevel:withError:"), (float)v, &error);
                printf("AVFlashlight level=%.3f result=%d error=%s\n",v,ok,error ? [[error description] UTF8String] : "none");
                if (!ok) { rc = 1; break; }
                if (v == 0) ((void (*)(id,SEL))objc_msgSend)(flashlight, sel_registerName("turnPowerOff"));
            } else { rc = 2; break; }
        }
        /* Even a failed command sequence leaves a requested light dark. */
        if (flashlight) {
            id error = nil;
            ((BOOL (*)(id,SEL,float,id *))objc_msgSend)(flashlight,
                sel_registerName("setFlashlightLevel:withError:"), 0.0f, &error);
            ((void (*)(id,SEL))objc_msgSend)(flashlight, sel_registerName("turnPowerOff"));
        }
        if (fd >= 0) { uint32_t zero = 0; (void)write(fd, &zero, sizeof(zero)); close(fd); }
        if (rc == 2) fprintf(stderr,"usage: torchctl [status | raw 0..1000 | av 0..1 | wait 0..30]...\n");
        return rc;
    }
}
