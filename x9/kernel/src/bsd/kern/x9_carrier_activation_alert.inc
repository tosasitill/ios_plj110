/* 20H392 arm64 SpringBoard, NOT MobileActivation/Lockdown state.
 * Reference: hackios-xnu-kernel userland/src/inject_sb.c demonstrates the
 * SpringBoard activation UI boundary, but its forced SBLockdownManager state
 * is deliberately NOT ported. SpringBoard.loctable ACTIVATION_FAILED_TITLE
 * is "iPhone is not Activated" / the user's carrier-provisioning popup.
 *
 * -[SBTelephonyManager _provisioningUpdateWithStatus:slot:], status == 2:
 * 1c58f2cac..cb4 increments the actual failure counter. At 1c58f2cbc the
 * original cbz w8,1c58f2d80 skips constructing SBActivationFailedAlertItem.
 * Use that existing nil-alert/cleanup path regardless of the UI flag.
 * Other statuses, SIM state, provisioning result, MobileActivation record,
 * network eligibility and error/reset reporting are all unchanged.
 *
 * Original .21 cache UUID, complete logical 16 KiB page and CodeDirectory
 * hash were qualified offline. Only this instruction and its page hash
 * change. x9_activation_alert=1 (default): suppress this specific popup;
 * =0 or =2: restore the original branch; unknown settings do nothing.
 * This is a UI workaround, not a successful activation or a data fix.
 */
static const struct x9_ccgate_edit x9_activation_alert_edits[] = {
 {45755580, 4, {0x28,0x06,0x00,0x34}, {0x31,0x00,0x00,0x14},
  "carrier provisioning failure popup branch"},
 {114990561, 32,
  {0x19,0x0c,0x94,0xd3,0x0c,0x72,0x60,0xbd,0x0f,0x79,0xda,0xba,0xf8,0xe6,0x47,0x6b,
   0x10,0x54,0xe8,0xbb,0x81,0xfa,0x28,0x5c,0x8a,0xbc,0xba,0x58,0xb7,0x7b,0xfe,0x0f},
  {0x12,0xde,0x90,0xde,0x67,0x3e,0x36,0x29,0x27,0x14,0x91,0xb3,0xfb,0xe0,0xa3,0x4b,
   0x1d,0x68,0x53,0x80,0x1b,0x05,0xc5,0xc9,0x5a,0x23,0x2b,0xbe,0x27,0x0d,0xaa,0x8e},
  "CodeDirectory page 2792 SHA-256"},
};

static int
x9_activation_alert_page(vnode_t vp, const uint8_t *expected, vfs_context_t ctx)
{
 uint8_t buf[256], digest[32];
 SHA256_CTX hash;
 SHA256_Init(&hash);
 for (unsigned i = 0; i < 16384; i += sizeof(buf)) {
  int error = x9_ccgate_io(vp, (off_t)2792 * 16384 + i, buf, sizeof(buf), UIO_READ, ctx);
  if (error) return error;
  SHA256_Update(&hash, buf, sizeof(buf));
 }
 SHA256_Final(digest, &hash);
 return memcmp(digest, expected, sizeof(digest)) ? EINVAL : 0;
}

static int
sweet_x9_carrier_activation_alert(void)
{
 extern boolean_t sweet_x9_ufs_root_writable(void);
 static const char path[] = "/private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64.21";
 static const uint8_t uuid[16] = {
  0x15,0x55,0x22,0x41,0x3d,0x4d,0x38,0x09,0xbc,0x25,0x61,0x20,0x6e,0x91,0xb9,0xce
 };
 const int mode = sweet_x9_arg("x9_activation_alert", 1);
 const int target = mode == 1;
 vfs_context_t ctx = vfs_context_kernel();
 struct vnode_attr va;
 vnode_t vp = NULLVP;
 uint8_t buf[104], hdr[104];
 unsigned matches = 3;
 int error, source;
 if ((mode != 0 && mode != 1 && mode != 2) || !sweet_x9_ufs_root_writable()) return 0;
 error = vnode_lookup(path, VNODE_LOOKUP_NOFOLLOW, &vp, ctx);
 if (error) { printf("X9 ACTIVATION UI: lookup errno=%d; not touched\n", error); return error; }
 VATTR_INIT(&va); VATTR_WANTED(&va, va_data_size); VATTR_WANTED(&va, va_flags);
 error = vnode_getattr(vp, &va, ctx);
 if (error || !vnode_isreg(vp) || va.va_data_size != 115130368) {
  printf("X9 ACTIVATION UI: file errno=%d reg=%d size=%lld; not touched\n",
      error, vnode_isreg(vp), (long long)va.va_data_size);
  error = error ? error : EINVAL; goto out;
 }
 error = x9_ccgate_io(vp, 0, hdr, sizeof(hdr), UIO_READ, ctx);
 if (error) goto out;
 if (memcmp(hdr, "dyld_v1   arm64", 15) || memcmp(hdr + 88, uuid, sizeof(uuid))) {
  error = EINVAL; printf("X9 ACTIVATION UI: cache magic/UUID mismatch; not touched\n"); goto out;
 }
 for (unsigned i = 0; i < 2; i++) {
  const struct x9_ccgate_edit *e = &x9_activation_alert_edits[i];
  error = x9_ccgate_io(vp, e->off, buf, e->len, UIO_READ, ctx);
  if (error) goto out;
  for (unsigned state = 0; state < 2; state++)
   if (memcmp(buf, state ? e->patched : e->orig, e->len)) matches &= ~(1u << state);
 }
 if (matches != 1 && matches != 2) {
  printf("X9 ACTIVATION UI: mixed/unknown page state mask=0x%x; not touched\n", matches);
  error = EINVAL; goto out;
 }
 source = matches == 2;
 error = x9_activation_alert_page(vp, source ? x9_activation_alert_edits[1].patched : x9_activation_alert_edits[1].orig, ctx);
 if (error) { printf("X9 ACTIVATION UI: logical page mismatch/read errno=%d; not touched\n", error); goto out; }
 if (source == target) {
  printf("X9 ACTIVATION UI: already %s; actual activation/provisioning unchanged\n", target ? "carrier failure popup suppressed" : "original");
  goto out;
 }
 if (va.va_flags & UF_COMPRESSED) {
  /* Same lossless HFS conversion as CCBUNDLE: unchanged size, no FWRITE
   * open or manual removal of UF_COMPRESSED, requalify before writing. */
  VATTR_INIT(&va); VATTR_SET(&va, va_data_size, 115130368);
  error = vnode_setattr(vp, &va, ctx);
  if (error) goto out;
  VATTR_INIT(&va); VATTR_WANTED(&va, va_data_size); VATTR_WANTED(&va, va_flags);
  error = vnode_getattr(vp, &va, ctx);
  if (!error && (va.va_data_size != 115130368 || (va.va_flags & UF_COMPRESSED))) error = EINVAL;
  if (!error) error = x9_ccgate_io(vp, 0, buf, sizeof(hdr), UIO_READ, ctx);
  if (!error && memcmp(buf, hdr, sizeof(hdr))) error = EINVAL;
  for (unsigned i = 0; i < 2 && !error; i++) {
   const struct x9_ccgate_edit *e = &x9_activation_alert_edits[i];
   error = x9_ccgate_io(vp, e->off, buf, e->len, UIO_READ, ctx);
   if (!error && memcmp(buf, source ? e->patched : e->orig, e->len)) error = EINVAL;
  }
  if (!error) error = x9_activation_alert_page(vp, source ? x9_activation_alert_edits[1].patched : x9_activation_alert_edits[1].orig, ctx);
  printf("X9 ACTIVATION UI: lossless cache conversion/requalification errno=%d\n", error);
  if (error) goto out;
 }
 /* Code first, hash last. No retries or unknown partial-state repair. */
 for (unsigned i = 0; i < 2; i++) {
  const struct x9_ccgate_edit *e = &x9_activation_alert_edits[i];
  memcpy(buf, target ? e->patched : e->orig, e->len);
  error = x9_ccgate_io(vp, e->off, buf, e->len, UIO_WRITE, ctx);
  if (error) { printf("X9 ACTIVATION UI: write %s errno=%d after %u edits\n", e->what, error, i); goto out; }
 }
 error = VNOP_FSYNC(vp, MNT_WAIT, ctx);
 for (unsigned i = 0; i < 2 && !error; i++) {
  const struct x9_ccgate_edit *e = &x9_activation_alert_edits[i];
  error = x9_ccgate_io(vp, e->off, buf, e->len, UIO_READ, ctx);
  if (!error && memcmp(buf, target ? e->patched : e->orig, e->len)) error = EIO;
 }
 printf("X9 ACTIVATION UI: %s -> %s, page2792 hash/readback errno=%d; actual activation/provisioning unchanged\n",
     source ? "popup suppressed" : "original", target ? "popup suppressed" : "original", error);
out:
 vnode_put(vp);
 return error;
}
