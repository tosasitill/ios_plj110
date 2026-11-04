/*
 * Built-in AMFI interface for SWEET.
 *
 * On Apple platforms AppleMobileFileIntegrity registers this interface and
 * provides entitlements through the closed CoreEntitlements library. SWEET
 * has neither, and code signing reaches it on every exec (ubc_cs_blob_add).
 *
 * Entitlements: when a code signature is added, its XML entitlements blob
 * (already checked against the code directory) is parsed into a
 * SweetEntitlements object (iokit/Kernel/arm/SweetEntitlements.cpp), set as
 * the blob's csb_entitlements, and every query is answered from it. DER-only
 * signatures get none. Code signature and page hash validation stay with the
 * kernel. Trust caches remain unavailable (no Image4), so the TrustCache
 * interface is left at version 0.
 *
 * Platform binaries: AMFI marks binaries whose cdhash is in a trust cache
 * as platform binaries from its vnode_check_signature MAC hook. Without
 * trust caches, the SWEET policy below marks every binary on the root
 * filesystem instead (the iOS system binaries are ad-hoc signed and are
 * platform binaries only through the static trust cache).
 *
 * Interfaces no kernel path uses on SWEET (they serve other Apple kexts)
 * panic with a clear message instead of being NULL.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <kern/debug.h>
#include <kern/startup.h>
#include <kern/task.h>
#include <libkern/amfi/amfi.h>
#include <mach/kern_return.h>
#include <mach/vm_param.h>
#include <sys/codesign.h>
#include <sys/mount.h>
#include <sys/priv.h>
#include <sys/proc.h>
#include <sys/proc_internal.h>
#include <sys/systm.h>
#include <sys/ubc_internal.h>
#include <sys/vnode.h>
#include <security/mac_policy.h>
#include <vm/vm_kern.h>
#include <vm/vm_map.h>
#include <arm64/sweet_early.h>

/* iokit/Kernel/arm/SweetEntitlements.cpp */
void *sweet_entitlements_create(const CS_GenericBlob *blob, size_t blob_len);
void *sweet_entitlements_copy_dict(const void *ent);
bool sweet_entitlements_copy_xml(const void *ent, CS_GenericBlob **out);
bool sweet_entitlements_bool(const void *ent, const char *name);
bool sweet_entitlements_string(const void *ent, const char *name, const char *value);
void *sweet_entitlements_copy_object(const void *ent, const char *name);
unsigned int sweet_entitlements_count(const void *ent);
extern void osobject_release(void *object);

/* boot-arg sweet_amfi_debug=1: log the entitlements attached to each signature. */
static TUNABLE(uint32_t, sweet_amfi_debug, "sweet_amfi_debug", 0);

/* The entitlements object of a process's main binary, or NULL. */
static void *
sweet_proc_entitlements(proc_t proc)
{
	struct cs_blob *blob = csproc_get_blob(proc);
	return blob != NULL ? csblob_os_entitlements_get(blob) : NULL;
}

static void
sweet_amfi_entitlements_invalidate(__unused void *os_entitlements)
{
	/* Queries already require a valid signature (csblob_get_entitlements). */
}

static void *
sweet_amfi_entitlements_as_dict(void *os_entitlements)
{
	return sweet_entitlements_copy_dict(os_entitlements);
}

static CEError_t
sweet_amfi_entitlements_query(__unused void *os_entitlements,
    __unused uint8_t cdhash[CS_CDHASH_LEN], __unused CEQuery_t query,
    __unused size_t query_length)
{
	panic("SWEET AMFI: CoreEntitlements queries are not implemented");
}

static bool
sweet_amfi_entitlements_get_transmuted(__unused void *os_entitlements,
    __unused const CS_GenericBlob **blob)
{
	return false;
}

static bool
sweet_amfi_entitlements_get_xml(void *os_entitlements, CS_GenericBlob **blob)
{
	return sweet_entitlements_copy_xml(os_entitlements, blob);
}

static bool
sweet_amfi_get_legacy_profile_exemptions(__unused const uint8_t **profile,
    __unused size_t *profile_length)
{
	return false;
}

static bool
sweet_amfi_get_udid(__unused const uint8_t **udid, __unused size_t *udid_length)
{
	return false;
}

static void *
sweet_amfi_query_context_to_object(__unused CEQueryContext_t ctx)
{
	panic("SWEET AMFI: CoreEntitlements query contexts are not implemented");
}

static kern_return_t
sweet_amfi_adjust_with_monitor(__unused void *os_entitlements,
    __unused const CEQueryContext_t ce_ctx, __unused const void *monitor_sig_obj,
    __unused const char *identity, __unused const uint32_t code_signing_flags)
{
	/* SWEET has no code signing monitor. */
	return KERN_NOT_SUPPORTED;
}

/*
 * Attach the parsed XML entitlements to a code signature being added. This
 * must happen in the vnode_check_signature MAC hook, like AMFI does: later,
 * converting a 4K-page signature to multilevel hashes drops the XML blob.
 * Nothing is granted when the blob is missing, unreadable or does not match
 * its code directory hash.
 */
static void
sweet_amfi_attach_entitlements(struct cs_blob *code_signing_blob)
{
	void *xml = NULL;
	size_t xml_len = 0;

	if (csblob_get_entitlements(code_signing_blob, &xml, &xml_len) != 0) {
		printf("SWEET AMFI: %s: entitlements do not match the code directory, granting none\n",
		    csblob_get_identity(code_signing_blob));
		return;
	}
	if (xml == NULL) {
		return;                 /* no XML entitlements */
	}
	void *ent = sweet_entitlements_create(xml, xml_len);
	if (ent == NULL) {
		printf("SWEET AMFI: %s: unreadable entitlements (%zu bytes), granting none\n",
		    csblob_get_identity(code_signing_blob), xml_len);
		return;
	}
	if (sweet_amfi_debug) {
		/* The test keys only exist in tools/mkinit.py test signatures. */
		printf("SWEET AMFI: %s: %u entitlements (test: bool %d string %d array %d)\n",
		    csblob_get_identity(code_signing_blob), sweet_entitlements_count(ent),
		    sweet_entitlements_bool(ent, "com.apple.private.sweet.test-bool"),
		    sweet_entitlements_string(ent, "com.apple.private.sweet.test-string", "hello"),
		    sweet_entitlements_string(ent, "com.apple.private.sweet.test-array", "two"));
	}
	csblob_os_entitlements_set(code_signing_blob, ent);    /* takes a reference */
	osobject_release(ent);
}

static kern_return_t
sweet_amfi_adjust_without_monitor(__unused void *os_entitlements,
    __unused struct cs_blob *code_signing_blob)
{
	/* Entitlements were attached in sweet_amfi_vnode_check_signature. */
	return KERN_SUCCESS;
}

static kern_return_t
sweet_amfi_query_boolean(const void *os_entitlements, const char *entitlement_name)
{
	return sweet_entitlements_bool(os_entitlements, entitlement_name) ? KERN_SUCCESS : KERN_DENIED;
}

static kern_return_t
sweet_amfi_query_boolean_with_proc(const proc_t proc, const char *entitlement_name)
{
	return sweet_amfi_query_boolean(sweet_proc_entitlements(proc), entitlement_name);
}

static kern_return_t
sweet_amfi_query_string(const void *os_entitlements,
    const char *entitlement_name, const char *entitlement_value)
{
	return sweet_entitlements_string(os_entitlements, entitlement_name, entitlement_value) ?
	       KERN_SUCCESS : KERN_DENIED;
}

static kern_return_t
sweet_amfi_query_string_with_proc(const proc_t proc,
    const char *entitlement_name, const char *entitlement_value)
{
	return sweet_amfi_query_string(sweet_proc_entitlements(proc), entitlement_name,
	           entitlement_value);
}

static kern_return_t
sweet_amfi_copy_object(const void *os_entitlements,
    const char *entitlement_name, void **entitlement_object)
{
	void *obj = sweet_entitlements_copy_object(os_entitlements, entitlement_name);
	if (entitlement_object) {
		*entitlement_object = obj;
	} else if (obj) {
		osobject_release(obj);
	}
	return obj ? KERN_SUCCESS : KERN_NOT_FOUND;
}

static kern_return_t
sweet_amfi_copy_object_with_proc(const proc_t proc,
    const char *entitlement_name, void **entitlement_object)
{
	return sweet_amfi_copy_object(sweet_proc_entitlements(proc), entitlement_name,
	           entitlement_object);
}

static const amfi_t sweet_amfi = {
	.OSEntitlements_invalidate = sweet_amfi_entitlements_invalidate,
	.OSEntitlements_asdict = sweet_amfi_entitlements_as_dict,
	.OSEntitlements_query = sweet_amfi_entitlements_query,
	.OSEntitlements_get_transmuted = sweet_amfi_entitlements_get_transmuted,
	.OSEntitlements_get_xml = sweet_amfi_entitlements_get_xml,
	/* CoreEntitlements is only used by the PMAP_CS code signing monitor. */
	.get_legacy_profile_exemptions = sweet_amfi_get_legacy_profile_exemptions,
	.get_udid = sweet_amfi_get_udid,
	.query_context_to_object = sweet_amfi_query_context_to_object,
	/* .TrustCache stays version 0: trust caches are unavailable without Image4. */
	.OSEntitlements = {
		.version = OSENTITLEMENTS_INTERFACE_VERSION,
		.adjustContextWithMonitor = sweet_amfi_adjust_with_monitor,
		.adjustContextWithoutMonitor = sweet_amfi_adjust_without_monitor,
		.queryEntitlementBoolean = sweet_amfi_query_boolean,
		.queryEntitlementBooleanWithProc = sweet_amfi_query_boolean_with_proc,
		.queryEntitlementString = sweet_amfi_query_string,
		.queryEntitlementStringWithProc = sweet_amfi_query_string_with_proc,
		.copyEntitlementAsOSObject = sweet_amfi_copy_object,
		.copyEntitlementAsOSObjectWithProc = sweet_amfi_copy_object_with_proc,
	},
};

__startup_func
static void
sweet_amfi_register(void)
{
	amfi_interface_register(&sweet_amfi);
	printf("SWEET AMFI: built-in interface registered (XML entitlements)\n");
}
STARTUP(EARLY_BOOT, STARTUP_RANK_MIDDLE, sweet_amfi_register);

/*
 * The signature reached this hook validated, so mark it CS_SIGNED like AMFI
 * does: exec copies the flag to the process, and csproc_get_blob() (csops
 * status, cdhash, platform and entitlement queries by process) ignores
 * processes without it. Every signed binary on the root filesystem is a
 * platform binary. New signatures (ubc_cs_blob_add passes its writable copy;
 * revalidation passes the read-only blob itself) get their entitlements here.
 */
static int
sweet_amfi_vnode_check_signature(struct vnode *vp, __unused struct label *label,
    __unused cpu_type_t cpu_type, struct cs_blob *cs_blob,
    unsigned int *cs_flags, __unused unsigned int *signer_type,
    __unused int flags, __unused unsigned int platform,
    __unused char **fatal_failure_desc, __unused size_t *fatal_failure_desc_len)
{
	mount_t mp = vnode_mount(vp);
	*cs_flags |= CS_SIGNED;
	if (mp != NULL && (vfs_flags(mp) & MNT_ROOTFS)) {
		*cs_flags |= CS_PLATFORM_BINARY;
	}
	/* Bring-up diag, gated behind sweet_amfi_debug: unconditional it logs one
	 * line per signed binary and floods the small (~256 KB) pstore ring. */
	if (sweet_amfi_debug) {
		const char *nm = vnode_getname(vp);
		printf("SWEET AMFI: sig %s rootfs=%d cs_flags=0x%x\n",
		    nm ? nm : "?", (mp && (vfs_flags(mp) & MNT_ROOTFS)) ? 1 : 0, *cs_flags);
		if (nm) {
			vnode_putname(nm);
		}
	}
	if (cs_blob != cs_blob->csb_ro_addr && cs_blob->csb_entitlements == NULL) {
		sweet_amfi_attach_entitlements(cs_blob);
	}
	return 0;
}

/*
 * Policy "Sandbox": libsystem_sandbox calls __mac_syscall("Sandbox", op,
 * arg). Without a policy of that name every call fails with ENOPOLICY and
 * launchd reports mach lookups as "error = 159: Sandbox restriction"
 * (backboardd cannot reach mobilegestalt or iohideventsystem; tccd:
 * "profile not found"). SWEET runs without a sandbox: operations succeed
 * (checks allow, profile init is accepted). The first use of each operation
 * number is logged.
 *
 * Request 4 is the one that needs an answer: sandbox_container_path_for_pid()
 * and sandbox_container_path_for_audit_token() ask which container a process
 * is in, and an application extension hosted by PlugInKit does not start
 * without one -[PKService checkEnvironment:] pre-fills the buffer with "?"
 * and raises "plugin has invalid sandbox or container" unless a longer path
 * comes back. The launcher that started the process (RunningBoard for an
 * app, pkd for an extension) got the path from containermanagerd and passed
 * it on in CFFIXED_USER_HOME, which stays among the strings exec placed at
 * the top of the process stack; answer with that. A process without the
 * variable has no container and gets success with the buffer untouched.
 * Ported from hackios-xnu-kernel bsd/kern/kern_minimac.c
 * (minisandbox_container_path, c84b4634277a70beac73921029b1fb6afc2530af).
 */
static uint64_t sweet_sandbox_seen;

#define SWEET_SANDBOX_CONTAINER_PATH    4
#define SWEET_SANDBOX_CONTAINER_VAR     "CFFIXED_USER_HOME="

static int
sweet_sandbox_container_path(user_addr_t arg)
{
	struct {
		int64_t     pid;
		uint64_t    pidversion;
		user_addr_t buffer;
		user_size_t size;
		uint64_t    check_pidversion;
	} args;
	const size_t prefix_len = sizeof(SWEET_SANDBOX_CONTAINER_VAR) - 1;
	proc_t p;
	task_t task;
	vm_map_t map;
	vm_map_copy_t copy = NULL;
	vm_offset_t strings = 0;
	vm_size_t strings_size;
	user_addr_t user_stack;
	size_t argslen, len;
	const char *s, *end;
	int error;

	error = copyin(arg, &args, sizeof(args));
	if (error) {
		return error;
	}
	p = proc_find((int)args.pid);
	if (p == PROC_NULL) {
		return ESRCH;
	}
	if (args.check_pidversion && (uint64_t)proc_pidversion(p) != args.pidversion) {
		proc_rele(p);
		return ESRCH;
	}
	user_stack = p->user_stack;
	argslen = p->p_argslen;
	task = proc_task(p);
	if (user_stack == 0 || argslen == 0 || task == TASK_NULL) {
		proc_rele(p);
		return 0;
	}
	/* sysctl_procargsx() order: the task keeps the map alive after proc_rele */
	task_reference(task);
	proc_rele(p);
	map = get_task_map_reference(task);
	task_deallocate(task);
	if (map == VM_MAP_NULL) {
		return 0;
	}
	strings_size = vm_map_round_page((vm_map_offset_t)argslen,
	    vm_map_page_mask(map));
	if (kmem_alloc(kernel_map, &strings, strings_size,
	    KMA_DATA | KMA_ZERO, VM_KERN_MEMORY_BSD) != KERN_SUCCESS) {
		vm_map_deallocate(map);
		return ENOMEM;
	}
	if (vm_map_copyin(map, (vm_map_address_t)(user_stack - strings_size),
	    strings_size, FALSE, &copy) != KERN_SUCCESS) {
		copy = NULL;
	}
	vm_map_deallocate(map);
	if (copy == NULL || vm_map_copy_overwrite(kernel_map, strings, copy,
	    strings_size, FALSE) != KERN_SUCCESS) {
		if (copy != NULL) {
			vm_map_copy_discard(copy);
		}
		kmem_free(kernel_map, strings, strings_size);
		return 0;               /* process exiting or stack unmapped */
	}
	end = (const char *)(strings + strings_size);
	for (s = end - argslen; s < end; s += len + 1) {
		len = strnlen(s, (size_t)(end - s));
		if (len > prefix_len && s + len < end &&
		    strncmp(s, SWEET_SANDBOX_CONTAINER_VAR, prefix_len) == 0) {
			s += prefix_len;
			len -= prefix_len;
			error = len < args.size ?
			    copyout(s, args.buffer, len + 1) : ENAMETOOLONG;
			break;
		}
	}
	kmem_free(kernel_map, strings, strings_size);
	return error;
}

static int
sweet_sandbox_syscall(struct proc *p, int call, user_addr_t arg)
{
	if (call >= 0 && call < 64 && !(sweet_sandbox_seen & (1ULL << call))) {
		sweet_sandbox_seen |= 1ULL << call;
		char name[32];
		proc_name(proc_pid(p), name, sizeof(name));
		if (call == SWEET_SANDBOX_CONTAINER_PATH) {
			printf("SWEET Sandbox: %s op %d container path\n", name, call);
		} else {
			printf("SWEET Sandbox: %s op %d\n", name, call);
		}
	}
	if (call == SWEET_SANDBOX_CONTAINER_PATH) {
		return sweet_sandbox_container_path(arg);
	}
	return 0;
}

/* hackios reports the actual no-enforcement state via policy AMFI 0x60.
 * X9 already runs SWEET's root-platform compatibility policy; do not alter
 * vnode signature checks, other boards, or return success for unknown ops. */
static int
sweet_x9_amfi_policy_syscall(__unused struct proc *p,int call,user_addr_t arg)
{
 if(!sweet_early_is_x9())return ENOPOLICY;
 if(call==0x60){uint64_t state=0x86;return copyout(&state,arg,sizeof(state));}
 if(call==0x5a){
  struct {uint64_t input_flags;user_addr_t output_flags;} req;
  uint64_t flags=0xff;int error=copyin(arg,&req,sizeof(req));
  return error?error:copyout(&flags,req.output_flags,sizeof(flags));
 }
 return ENOPOLICY;
}

static const struct mac_policy_ops sweet_amfi_mac_ops = {
	.mpo_vnode_check_signature = sweet_amfi_vnode_check_signature,
	.mpo_policy_syscall = sweet_x9_amfi_policy_syscall,
};

/*
 * Privileges for non-root processes come from MAC policies (priv_check_cred
 * -> mac_priv_grant); on iOS Sandbox/AMFI grant them. Without a grant UIKit
 * apps die in CoreAnimation setup: work_interval_create(CA_CLIENT) needs
 * PRIV_WORK_INTERVAL ("Failed to create work interval", SpringBoard).
 * Grant that one; log the first denial of each other privilege per process.
 */
#define SWEET_PRIV_LOG_MAX      64
static struct {
	int  priv;
	char name[20];
} sweet_priv_logged[SWEET_PRIV_LOG_MAX];
static int sweet_priv_logged_count;

static int
sweet_sandbox_priv_grant(kauth_cred_t cred __unused, int priv)
{
	if (priv == PRIV_WORK_INTERVAL) {
		return 0;
	}
	char name[20];
	proc_selfname(name, sizeof(name));
	for (int i = 0; i < sweet_priv_logged_count; i++) {
		if (sweet_priv_logged[i].priv == priv &&
		    strncmp(sweet_priv_logged[i].name, name, sizeof(name)) == 0) {
			return EPERM;
		}
	}
	if (sweet_priv_logged_count < SWEET_PRIV_LOG_MAX) {
		sweet_priv_logged[sweet_priv_logged_count].priv = priv;
		strlcpy(sweet_priv_logged[sweet_priv_logged_count].name, name, sizeof(name));
		sweet_priv_logged_count++;
		printf("SWEET Sandbox: %s privilege %d denied\n", name, priv);
	}
	return EPERM;
}

static const struct mac_policy_ops sweet_sandbox_mac_ops = {
	.mpo_policy_syscall = sweet_sandbox_syscall,
	.mpo_priv_grant = sweet_sandbox_priv_grant,
};

static struct mac_policy_conf sweet_sandbox_mac_conf = {
	.mpc_name = "Sandbox",
	.mpc_fullname = "SWEET permissive sandbox stand-in",
	.mpc_ops = &sweet_sandbox_mac_ops,
};

static mac_policy_handle_t sweet_sandbox_mac_handle;

static struct mac_policy_conf sweet_amfi_mac_conf = {
	.mpc_name = "SWEET AMFI",
	.mpc_fullname = "SWEET built-in Mobile File Integrity",
	.mpc_ops = &sweet_amfi_mac_ops,
};

static mac_policy_handle_t sweet_amfi_mac_handle;

/* Called from bsd_init once MAC policies can register, before any exec. */
void sweet_amfi_policy_register(void);

void
sweet_amfi_policy_register(void)
{
	if (sweet_early_is_x9()) sweet_amfi_mac_conf.mpc_name = "AMFI";
	int error = mac_policy_register(&sweet_amfi_mac_conf, &sweet_amfi_mac_handle, NULL);
	printf("SWEET AMFI: platform binary policy %s (%d)\n",
	    error ? "NOT registered" : "registered", error);
	error = mac_policy_register(&sweet_sandbox_mac_conf, &sweet_sandbox_mac_handle, NULL);
	printf("SWEET Sandbox: permissive policy %s (%d)\n",
	    error ? "NOT registered" : "registered", error);
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
