/* Actual saved LK load_fw_image5a03c -> b6a24 selects connsys_gnss, reads
 * only outer512-stripped payload into EMI region1 (CONN_RO), then89f8
 * CIVAC+DSB. XNU qualifies that retained/provisioned image; no copy, clear,
 * alternate Linux48B stripping, writable alias, certificate or ROM jump.
 * The existing real provision callback MUST establish safe image/ROM/EMI
 * access and leave reset held first. Digest identity is NOT that authority.
 * A partial failure retains the mapping/lease and never retries the read. */
#include "../../../generated/x9_gps_firmware.inc"
#define GPS_IMAGE_RO_BYTES 0x3c0000u
static void gps_image_journal(int error)
{
 if(error && !gps_rt.firmware.error)gps_rt.firmware.error=error;
 lck_mtx_lock(&gps_lock);
 st.firmware_present=X9_GPS_FW_PRESENT;
 st.firmware_bytes=X9_GPS_FW_BYTES;
 st.firmware_started=gps_rt.firmware.started;
 st.firmware_complete=gps_rt.firmware.complete;
 st.firmware_checks=gps_rt.firmware.checks;
 st.firmware_checked_bytes=gps_rt.firmware.checked_bytes;
 st.firmware_errno=gps_rt.firmware.error;
 lck_mtx_unlock(&gps_lock);
}
static int gps_image_owned(void)
{
 if(gps_rt.firmware.error)return gps_rt.firmware.error;
 if(current_thread()!=gps_rt.worker || !gps_rt.lease ||
     !gps_rt.held_reset || !gps_rt.remapped || gps_rt.booted ||
     gps_rt.hif.prepared)return EPERM;
 if(gps_rt.firmware.checks==UINT64_MAX)return EOVERFLOW;
 gps_rt.firmware.checks++;
 int e=gps_mcu_guard();if(e)return e;
 e=gps_mcu_expect(F_reset,0);if(e)return e;
 /* IMAGE follows hold_reset's function-enable and stopped bus directions;
  * this is NOT the earlier POS function-disabled boundary. */
 e=gps_mcu_expect(F_function,1);if(e)return e;
 e=gps_mcu_expect(F_emi,gps_rt.layout.reserve_base>>16);if(e)return e;
 const enum gps_mcu_field_id stopped[]={F_c_rx_st,F_c_tx_st,F_g_rx_st,
     F_g_tx_st,F_a_rx_st,F_a_tx_st};
 for(unsigned i=0;i<sizeof(stopped)/sizeof(stopped[0]);i++) {
  e=gps_mcu_expect(stopped[i],0);if(e)return e;
 }
 return 0;
}
static int gps_image_validate(void)
{
 gps_image_journal(0);
 if(!X9_GPS_FW_PRESENT)return 0; /* legacy supplier-only, no native opt-in */
 if(gps_rt.firmware.started)return gps_rt.firmware.error ? gps_rt.firmware.error : EBUSY;
 if(!X9_GPS_FW_BYTES || X9_GPS_FW_BYTES>GPS_IMAGE_RO_BYTES ||
     GPS_IMAGE_RO_BYTES>gps_rt.layout.reserve_size ||
     gps_rt.layout.layout_size!=0x630000 ||
     (uint64_t)gps_rt.layout.reserve_base+GPS_IMAGE_RO_BYTES>0x100000000ULL) {
  gps_image_journal(ENOTSUP);return ENOTSUP;
 }
 int e=gps_image_owned();if(e){gps_image_journal(e);return e;}
 gps_rt.firmware.started=1;
 /* Same device/noncacheable mapping discipline as existing HIF/NV. Never
  * map past CONN_RO into MCU RW, batch, legacy, NV or HIF allocations. */
 gps_rt.firmware.p=(volatile uint8_t *)ml_io_map(
     (vm_offset_t)gps_rt.layout.reserve_base,GPS_IMAGE_RO_BYTES);
 if(!gps_rt.firmware.p){gps_image_journal(ENOMEM);return ENOMEM;}
 sha256_init(&gps_rt.firmware.sha);
 for(unsigned off=0;off<X9_GPS_FW_BYTES;) {
  unsigned end=X9_GPS_FW_BYTES-off>16384 ? off+16384 : X9_GPS_FW_BYTES;
  e=gps_image_owned();if(e)break;
  while(off<end) {
   unsigned n=end-off>sizeof(gps_rt.firmware.scratch) ?
       sizeof(gps_rt.firmware.scratch) : end-off;
   for(unsigned i=0;i<n;i++)gps_rt.firmware.scratch[i]=gps_rt.firmware.p[off+i];
   __asm__ volatile("dsb sy" ::: "memory");
   sha256_update(&gps_rt.firmware.sha,gps_rt.firmware.scratch,n);
   off+=n;
   /* Actual bytes consumed, even when the following lease check fails.
    * This prefix is an observation, not a qualified image. */
   gps_rt.firmware.checked_bytes=off;
  }
  e=gps_image_owned();if(e)break;
 }
 if(!e) {
  sha256_final(&gps_rt.firmware.sha,gps_rt.firmware.digest);
  if(memcmp(gps_rt.firmware.digest,x9_gps_fw_sha256,32))e=EBADMSG;
 }
 if(!e)e=gps_image_owned();
 if(!e)gps_rt.firmware.complete=1;
 gps_image_journal(e);return e;
}
#undef GPS_IMAGE_RO_BYTES
