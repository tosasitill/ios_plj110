/*
 * KPI surface for the prelinked 20H392 APFS closure (2026-11-28).
 *
 * 1. vfs_getvfs_with_vfsops: the one kernel export the closure
 *    (com.apple.filesystems.apfs) imports that XNU 8796 lacks.  Semantics
 *    copied from the 20H392 kernelcache (0xfffffff00734c520): under the
 *    mount list lock, find the FIRST mount whose f_fsid matches, then return
 *    it only if its mnt_op is the caller's vfsops, else NULL.
 *
 * 2. Prelink decline shims.  The closure starts three kexts whose start
 *    routines install process-global kernel interfaces that this kernel
 *    deliberately owns or leaves empty:
 *      kec.corecrypto  -> register_and_init_prng      (SWEET already installed
 *                         its HMAC-DRBG at EARLY_BOOT; a second call trips
 *                         assert(!prng_ready) on DEVELOPMENT and would swap the
 *                         live per-CPU generators mid-flight)
 *      AppleImage4     -> img4_interface_register     (img4if stays NULL: the
 *                         trust-cache runtime was resolved NULL at boot)
 *      AppleImage4     -> mac_policy_register         (no late MAC policy)
 *      CoreTrust       -> coretrust_interface_register (SWEET AMFI owns trust)
 *    x9/build/scripts/prelink_rebind.py rebinds exactly those imports to the
 *    shims below at prelink time.  Nothing in the native kernel calls them.
 *    register_crypto_functions is NOT declined: g_crypto_funcs is NULL in
 *    this kernel and the stock registration is the intended behaviour.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/mount_internal.h>
#include <libkern/libkern.h>

mount_t vfs_getvfs_with_vfsops(fsid_t *fsid, const struct vfsops * const ops);

mount_t
vfs_getvfs_with_vfsops(fsid_t *fsid, const struct vfsops * const ops)
{
	mount_t mp;

	if (fsid == NULL) {
		return NULL;
	}
	mount_list_lock();
	TAILQ_FOREACH(mp, &mountlist, mnt_list) {
		if (mp->mnt_vfsstat.f_fsid.val[0] == fsid->val[0] &&
		    mp->mnt_vfsstat.f_fsid.val[1] == fsid->val[1]) {
			mount_list_unlock();
			return (mp->mnt_op == ops) ? mp : NULL;
		}
	}
	mount_list_unlock();
	return NULL;
}

/* Opaque prototypes: the prelinker binds by symbol address, not type. */
void sweet_prelink_decline_prng(void *ctx, const void *funcs);
void sweet_prelink_decline_img4(const void *i4);
void sweet_prelink_decline_coretrust(const void *ct);
int sweet_prelink_decline_mac_policy(void *mpc, unsigned int *handlep, void *xd);

static int sweet_prelink_declined;

static void
sweet_prelink_note(const char *what)
{
	if (sweet_prelink_declined++ < 8) {
		printf("SWEET PRELINK: declined %s (kernel-owned interface)\n", what);
	}
}

void
sweet_prelink_decline_prng(void *ctx, const void *funcs)
{
	(void)ctx; (void)funcs;
	sweet_prelink_note("register_and_init_prng");
}

void
sweet_prelink_decline_img4(const void *i4)
{
	(void)i4;
	sweet_prelink_note("img4_interface_register");
}

void
sweet_prelink_decline_coretrust(const void *ct)
{
	(void)ct;
	sweet_prelink_note("coretrust_interface_register");
}

/* Reports success with an out-of-range handle so the kext's start path
 * proceeds; the policy is never added to the MAC framework.  The closure
 * kexts are never unloaded, so the handle is never passed back. */
int
sweet_prelink_decline_mac_policy(void *mpc, unsigned int *handlep, void *xd)
{
	(void)mpc; (void)xd;
	if (handlep != NULL) {
		*handlep = 0xffffffffu;
	}
	sweet_prelink_note("mac_policy_register");
	return 0;
}
