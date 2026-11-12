// SPDX-License-Identifier: MIT
// x9-jbctl: environment status and LaunchServices registration of X9's own
// system apps (uicache). No root password, SSH daemon or fabricated trust.
// The registration dictionary follows TrollStore's uicache (MIT,
// opa334/TrollStore RootHelper/uicache.m): an ad-hoc signed, non-containerized
// app under /Applications registered as ApplicationType System.
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <dlfcn.h>
#import <unistd.h>
#import <errno.h>
extern int __mac_syscall(const char *, int, void *);

@interface NSObject (X9LaunchServices)
+ (id)defaultWorkspace;
- (BOOL)registerApplicationDictionary:(NSDictionary *)dict;
- (BOOL)unregisterApplication:(NSURL *)url;
- (BOOL)applicationIsInstalled:(NSString *)bundleID;
@end

static const char *const kPrefix = "/private/var/x9/jailbreak/20H392";

static int status(void) {
    uint64_t flags = 0;
    int result = __mac_syscall("AMFI", 0x60, &flags);
    printf("uid=%u AMFI.query=%d errno=%d flags=0x%llx\n", getuid(), result,
           result ? errno : 0, (unsigned long long)flags);
    const char *paths[] = {
        "/private/var/x9/jailbreak/20H392/lib/UniXNUAppSync.dylib",
        "/private/var/x9/jailbreak/20H392/lib/libunixnu-ldid.dylib",
        "/private/var/x9/jailbreak/20H392/bin/x9-appinst",
        "/Applications/X9Installer.app/X9Installer",
        "/bin/sh", "/usr/bin/dpkg", "/usr/bin/apt", "/usr/sbin/sshd"
    };
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i)
        printf("%s: %s\n", paths[i], access(paths[i], F_OK) ? "absent" : "present (not execution verification)");
    printf("Native USB/Trust/AppSync hook activation requires separate runtime evidence.\n");
    return result ? 1 : 0;
}

static id workspace(void) {
    dlopen("/System/Library/Frameworks/CoreServices.framework/CoreServices", RTLD_NOW);
    Class cls = objc_getClass("LSApplicationWorkspace");
    return [cls respondsToSelector:@selector(defaultWorkspace)] ? [cls defaultWorkspace] : nil;
}

// Only X9's own apps: /Applications/<Name>.app with entitlements recorded at
// build time under the runtime prefix (share/<Name>.entitlements).
static int registerApp(NSString *path) {
    path = path.stringByStandardizingPath;
    if (![path hasPrefix:@"/Applications/"] || ![path.pathExtension isEqual:@"app"] ||
        path.pathComponents.count != 3) {
        fprintf(stderr, "x9-jbctl: refusing %s (only /Applications/<Name>.app)\n", path.UTF8String);
        return 2;
    }
    NSDictionary *info = [NSDictionary dictionaryWithContentsOfFile:[path stringByAppendingPathComponent:@"Info.plist"]];
    NSString *bundleID = info[@"CFBundleIdentifier"];
    if (![bundleID isKindOfClass:NSString.class] || ![bundleID hasPrefix:@"org.x9."]) {
        fprintf(stderr, "x9-jbctl: %s has no org.x9.* CFBundleIdentifier\n", path.UTF8String);
        return 2;
    }
    NSString *name = path.lastPathComponent.stringByDeletingPathExtension;
    NSString *entPath = [NSString stringWithFormat:@"%s/share/%@.entitlements", kPrefix, name];
    NSDictionary *entitlements = [NSDictionary dictionaryWithContentsOfFile:entPath] ?: @{};
    id ws = workspace();
    if (![ws respondsToSelector:@selector(registerApplicationDictionary:)]) {
        fprintf(stderr, "x9-jbctl: LSApplicationWorkspace registration API unavailable\n");
        return 3;
    }
    NSMutableDictionary *dict = [NSMutableDictionary dictionary];
    [dict addEntriesFromDictionary:info];
    dict[@"ApplicationType"] = @"System";
    dict[@"CFBundleIdentifier"] = bundleID;
    dict[@"CodeInfoIdentifier"] = bundleID;
    dict[@"CompatibilityState"] = @0;
    dict[@"IsDeletable"] = @NO;
    dict[@"Path"] = path;
    dict[@"IsContainerized"] = @NO;
    dict[@"SignerOrganization"] = @"Apple Inc.";
    dict[@"SignatureVersion"] = @132352;
    dict[@"SignerIdentity"] = @"Apple iPhone OS Application Signing";
    dict[@"IsAdHocSigned"] = @YES;
    dict[@"LSInstallType"] = @1;
    dict[@"HasMIDBasedSINF"] = @0;
    dict[@"MissingSINF"] = @0;
    dict[@"FamilyID"] = @0;
    dict[@"IsOnDemandInstallCapable"] = @0;
    dict[@"Entitlements"] = entitlements;
    dict[@"_LSBundlePlugins"] = @{};
    BOOL ok = [ws registerApplicationDictionary:dict];
    BOOL installed = [ws respondsToSelector:@selector(applicationIsInstalled:)] && [ws applicationIsInstalled:bundleID];
    printf("x9-jbctl: register %s (%s) -> %s, installed=%d\n", path.UTF8String, bundleID.UTF8String,
           ok ? "ok" : "failed", installed);
    NSLog(@"[X9 JBCTL] register %@ (%@) -> %d installed=%d", path, bundleID, ok, installed);
    return ok ? 0 : 1;
}

int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc == 2 && !strcmp(argv[1], "--status")) return status();
        if (argc == 3 && !strcmp(argv[1], "--register")) return registerApp(@(argv[2]));
        fprintf(stderr, "usage: x9-jbctl --status | --register /Applications/<Name>.app\n");
        return 2;
    }
}
