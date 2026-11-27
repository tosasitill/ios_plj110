/* giveMeRoot (storm replacement for the auxiliary executable bundled in
 * Sileo.app). Contract, from Sileo 2.5.1's APTWrapper/RootWrapper: Sileo
 * posix_spawns this binary with argv = {"giveMeRoot", <tool path>,
 * <tool args...>}; the tool is expected to run as root. (On iOS 13+
 * upstream Sileo instead spawns the tool directly with a persona-99
 * posix_spawn attribute; whether this port's kernel honors that is an
 * open discriminator -- this binary is the fallback path and the shape a
 * source-patched Sileo would call.)
 *
 * Behavior: translate the tool path to a rootexec verb and exec
 * x9-rootexec. This binary carries no privilege and does no caller
 * check on purpose: after execve, the parent of the x9-rootexec process
 * is still the Sileo process, so x9-rootexec pins the Sileo app binary
 * (path + SHA256) as its caller and re-validates the full request
 * policy. A tampered giveMeRoot therefore cannot exceed the policy --
 * the only setuid binary in the chain is x9-rootexec.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <libgen.h>

static const char *verb_for_path(const char *path) {
    static const struct { const char *full; const char *verb; } map[] = {
        { "/var/storm/usr/bin/dpkg", "dpkg" },
        { "/var/storm/usr/bin/dpkg-deb", "dpkg-deb" },
        { "/var/storm/usr/bin/apt-get", "apt-get" },
        { "/var/storm/usr/bin/uicache", "uicache" },
    };
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (!strcmp(path, map[i].full)) return map[i].verb;
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: giveMeRoot <tool path> [args...]\n");
        return 2;
    }
    const char *verb = verb_for_path(argv[1]);
    if (!verb) {
        fprintf(stderr, "giveMeRoot: tool not allowed: %s\n", argv[1]);
        return 5;
    }
    char *args[24];
    args[0] = "x9-rootexec";
    args[1] = (char *)verb;
    int n = argc - 2;
    if (n > 20) return 5;
    for (int i = 0; i < n; i++) args[i + 2] = argv[i + 2];
    args[n + 2] = NULL;
    extern char **environ;
    execve("/var/storm/usr/bin/x9-rootexec", args, environ);
    fprintf(stderr, "giveMeRoot: exec x9-rootexec: %s\n", strerror(errno));
    return 6;
}
