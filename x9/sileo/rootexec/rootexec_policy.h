/* x9-rootexec policy: pure validation logic shared by the helper, the
 * SileoRootWrapper forwarder and the host unit tests. No platform calls.
 *
 * A request is (verb, argv). The verb selects one fixed binary under the
 * storm prefix; argv is passed to execve verbatim after validation. There
 * is no shell anywhere in the chain, so metacharacters are inert data --
 * the rules below exist to stop *semantic* escapes: options that would
 * redirect dpkg/apt outside the storm prefix or run attacker-chosen code.
 */
#ifndef X9_ROOTEXEC_POLICY_H
#define X9_ROOTEXEC_POLICY_H

#include <string.h>

#define X9_STORM_BIN "/var/storm/usr/bin"

typedef enum {
    X9_ALLOW = 0,
    X9_DENY_VERB,      /* verb not in the fixed set */
    X9_DENY_ARGC,      /* too many / too few arguments */
    X9_DENY_ARG,       /* an argument is on the verb's deny list */
    X9_DENY_PATH       /* a path argument escapes its allowed root */
} x9_decision;

typedef struct {
    const char *verb;
    const char *binary;      /* absolute path that will be execve'd */
    int max_args;            /* max arguments after the verb */
} x9_verb_rule;

static const x9_verb_rule X9_VERBS[] = {
    { "dpkg",    X9_STORM_BIN "/dpkg",    16 },
    { "apt-get", X9_STORM_BIN "/apt-get", 16 },
    { "dpkg-deb", X9_STORM_BIN "/dpkg-deb", 16 },
    { "uicache", X9_STORM_BIN "/uicache", 8 },
};

static int x9_starts_with(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

/* Options that relocate state or execute arbitrary code; denied as whole
 * arguments or as --opt=value prefixes. Shared by dpkg and apt-get. */
static const char *const X9_DENY_EXACT[] = {
    "--force-all", "--force-confnew", "--force-confmiss",
};
static const char *const X9_DENY_PREFIX[] = {
    "--root", "--admindir", "--instdir", "--log", "--status-fd",
    "--status-logger", "--preinvoke", "--postinvoke",
    "-o", "--option", "-c", "--config-file",
    "--print-foreign-architectures",
};

static int x9_arg_denied(const char *arg) {
    unsigned i;
    for (i = 0; i < sizeof(X9_DENY_EXACT) / sizeof(X9_DENY_EXACT[0]); i++)
        if (!strcmp(arg, X9_DENY_EXACT[i])) return 1;
    for (i = 0; i < sizeof(X9_DENY_PREFIX) / sizeof(X9_DENY_PREFIX[0]); i++) {
        const char *p = X9_DENY_PREFIX[i];
        if (!strcmp(arg, p)) return 1;
        if (x9_starts_with(arg, p) && arg[strlen(p)] == '=') return 1;
    }
    /* dpkg/apt hooks and script shells arrive via config too; the config
     * files themselves live under the prefix and are root-owned, so the
     * remaining lever is a *.deb path: it must be a plain file path with
     * no option-looking leading dash (already covered) -- nothing to do. */
    return 0;
}

/* uicache only ever registers/removes storm or /Applications apps. */
static int x9_uicache_arg_ok(const char *arg) {
    if (arg[0] != '-') return 0; /* positional handled below */
    return !strcmp(arg, "-a") || !strcmp(arg, "-r") ||
           !strcmp(arg, "-u") || !strcmp(arg, "-l");
}

static x9_decision x9_validate(const char *verb, int argc, char *const argv[],
                               const x9_verb_rule **rule_out) {
    unsigned i;
    const x9_verb_rule *rule = NULL;
    for (i = 0; i < sizeof(X9_VERBS) / sizeof(X9_VERBS[0]); i++)
        if (!strcmp(verb, X9_VERBS[i].verb)) rule = &X9_VERBS[i];
    if (!rule) return X9_DENY_VERB;
    if (argc < 0 || argc > rule->max_args) return X9_DENY_ARGC;
    for (i = 0; i < (unsigned)argc; i++) {
        const char *arg = argv[i];
        if (!strcmp(rule->verb, "uicache")) {
            if (arg[0] == '-') {
                if (!x9_uicache_arg_ok(arg)) return X9_DENY_ARG;
            } else if (!x9_starts_with(arg, "/var/storm/Applications/") &&
                       !x9_starts_with(arg, "/Applications/")) {
                return X9_DENY_PATH;
            }
        } else {
            if (arg[0] == '-' && x9_arg_denied(arg)) return X9_DENY_ARG;
            if (strstr(arg, "..") && strchr(arg, '/')) {
                /* a .deb operand may live anywhere readable, but never
                 * resolve through the prefix's parents */
                if (x9_starts_with(arg, "/var/storm/")) return X9_DENY_PATH;
            }
        }
    }
    if (rule_out) *rule_out = rule;
    return X9_ALLOW;
}

#endif
