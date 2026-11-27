/* Host unit tests for rootexec_policy.h: cc test_policy.c -o t && ./t */
#include <stdio.h>
#include "rootexec_policy.h"

static int failures, total;
static void check(const char *verb, int argc, char *const argv[], x9_decision want) {
    const x9_verb_rule *rule = NULL;
    x9_decision got = x9_validate(verb, argc, argv, &rule);
    total++;
    if (got != want) {
        failures++;
        printf("FAIL %s argc=%d: got %d want %d\n", verb, argc, (int)got, (int)want);
    }
    if (want == X9_ALLOW && (!rule || !rule->binary)) {
        failures++;
        printf("FAIL %s: allowed without rule\n", verb);
    }
}

int main(void) {
    char *a1[] = {"--install", "/tmp/pkg.deb"};
    char *a2[] = {"--install", "--force-all", "/tmp/pkg.deb"};
    char *a3[] = {"--root=/etc", "--install", "/tmp/pkg.deb"};
    char *a4[] = {"--admindir", "/tmp/x", "--install", "/tmp/pkg.deb"};
    char *a5[] = {"update"};
    char *a6[] = {"install", "sileo"};
    char *a7[] = {"-o", "Dir::State=/tmp", "update"};
    char *a8[] = {"--option=APT::Get::AllowUnauthenticated=1", "update"};
    char *a9[] = {"-p", "/Applications/Sileo.app"};
    char *a10[] = {"-a"};
    char *a11[] = {"-p", "/var/storm/Applications/Foo.app"};
    char *a12[] = {"/private/var/mobile/x.app"};
    char *a16[] = {"/Applications/Foo.app"};
    char *a13[] = {"--preinvoke=/tmp/evil", "update"};
    char *a14[] = {"--remove", "somepkg"};
    char *a15[] = {"--install", "/var/storm/../storm/Library/dpkg/status"};
    char *many[20]; int i;
    for (i = 0; i < 20; i++) many[i] = "x";
    check("dpkg", 2, a1, X9_ALLOW);
    check("dpkg", 3, a2, X9_DENY_ARG);
    check("dpkg", 3, a3, X9_DENY_ARG);
    check("dpkg", 4, a4, X9_DENY_ARG);
    check("dpkg", 3, a15, X9_DENY_PATH);
    check("dpkg", 1, a14, X9_ALLOW);
    check("apt-get", 1, a5, X9_ALLOW);
    check("apt-get", 2, a6, X9_ALLOW);
    check("apt-get", 3, a7, X9_DENY_ARG);
    check("apt-get", 2, a8, X9_DENY_ARG);
    check("apt-get", 2, a13, X9_DENY_ARG);
    check("uicache", 2, a9, X9_DENY_ARG);      /* -p is not in the flag set */
    check("uicache", 1, a10, X9_ALLOW);
    check("uicache", 2, a11, X9_DENY_ARG);    /* -p denied; path rule moot */
    check("uicache", 1, a12, X9_DENY_PATH);   /* bare path outside roots */
    check("uicache", 1, a16, X9_ALLOW);        /* bare path inside roots */
    char *a17[] = {"--build", "/tmp/pkgdir", "/tmp/out.deb"};
    char *a18[] = {"--root-owner-group", "--build", "/tmp/pkgdir"};
    check("dpkg-deb", 3, a17, X9_ALLOW);
    check("dpkg-deb", 3, a18, X9_ALLOW);
    check("rm", 1, a10, X9_DENY_VERB);
    check("sh", 1, a10, X9_DENY_VERB);
    check("dpkg", 20, many, X9_DENY_ARGC);
    printf("rootexec policy: %d/%d passed\n", total - failures, total);
    return failures ? 1 : 0;
}
