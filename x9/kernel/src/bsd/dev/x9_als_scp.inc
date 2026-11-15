/* Native PLJ110 SCP/sensorhub v2 worker (sole owner, thread context).
 * Sources: OPPO rv/scp_helper.{c,h}, scp_awake.c, scp_dvfs.c,
 * sensor/2.0/sensorhub/{ready,share_memory,sensor_comm,transceiver,timesync}.
 * No downloaded firmware in the kernel: LK/TFA owns the signed image.
 * Keep 26M/INFRA/DRAM requested: ULPOSC calibration/DVFS handoff is NOT
 * implemented. Never withdraw those votes after an uncertain remote start.
 * Mode 3 is experimental and explicit; no automatic retries/resets. */
#include <kern/sched_prim.h>
#include "x9_scp_layout.inc"
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern kern_return_t thread_terminate(thread_t);

static struct x9_scp_handoff als_layout;
static volatile uint32_t *als_mb0, *als_mb1, *als_core0, *als_core1;
static volatile uint32_t *als_infra, *als_vlp, *als_data, *als_super, *als_tx, *als_rx;
static uint8_t als_sequence, als_ack[8], als_reply[28], als_snapshot[8192];
static uint8_t als_expected_sequence, als_expected_type, als_expected_command;
static int als_ack_pending, als_ack_received, als_reply_received, als_awake;
static uint32_t als_wp, als_super_wp, als_data_bytes, als_super_bytes;
static uint32_t als_old_parent;
static uint8_t als_prox_info[X9_PROX_INFO_BYTES];
static int als_prox_present, als_list_complete;
static uint32_t als_prox_pending, als_prox_enabled, als_prox_interval;

/* HID requests only enqueue. The one worker owns OUT29 and correlates ACKs;
 * no second mailbox user, clock reparent, coprocessor release or awake vote. */
static int als_prox_request(uint32_t enabled, uint32_t interval)
{
 lck_mtx_lock(&x9_als_lock);
 int error = !physical_mode || !status.worker_running ? ENXIO : status.error;
 if (!error) {
  als_prox_pending = 1; als_prox_enabled = enabled; als_prox_interval = interval;
 }
 lck_mtx_unlock(&x9_als_lock); return error;
}
static const struct x9_prox_transport als_prox_transport = { als_prox_request };

static uint64_t als_now_ns(void)
{
 uint64_t ns; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns;
}
static void als_sleep(unsigned ms)
{
 assert_wait_timeout((event_t)&status, THREAD_UNINT, ms, 1000000);
 thread_block(THREAD_CONTINUE_NULL);
}
static int als_alive(void)
{
 lck_mtx_lock(&x9_als_lock); int yes = active;
 lck_mtx_unlock(&x9_als_lock); return yes;
}
static void als_stage(enum x9_als_stage stage)
{
 lck_mtx_lock(&x9_als_lock); status.stage = stage; lck_mtx_unlock(&x9_als_lock);
 printf("X9 ALS SCP: stage=%u\n", (unsigned)stage);
}
static void als_fault(int error)
{
 lck_mtx_lock(&x9_als_lock);
 if (!status.error) status.error = error;
 valid = 0; status.control_enabled = 0;
 lck_mtx_unlock(&x9_als_lock);
 x9_prox_native_disconnect(error);
}
static uint32_t als_rr(volatile uint32_t *base, unsigned off) { return base[off/4]; }
static void als_wr(volatile uint32_t *base, unsigned off, uint32_t value)
{ base[off/4] = value; __asm__ volatile("dsb sy" ::: "memory"); }
static void als_from_io(void *dst, volatile uint32_t *base, unsigned off, unsigned bytes)
{
 uint8_t *p = dst;
 __asm__ volatile("dmb sy" ::: "memory");
 for (unsigned i = 0; i < bytes; i += 4) {
  uint32_t v = als_rr(base, off+i);
  for (unsigned j = 0; j < 4; j++) p[i+j] = (uint8_t)(v>>(j*8));
 }
 __asm__ volatile("dmb sy" ::: "memory");
}
static void als_to_io(volatile uint32_t *base, unsigned off, const void *src, unsigned bytes)
{
 const uint8_t *p = src;
 for (unsigned i = 0; i < bytes; i += 4) base[(off+i)/4] = le32(p+i);
 __asm__ volatile("dsb sy" ::: "memory");
}
static void als_put32(uint8_t *p, uint32_t v)
{ for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(v>>(i*8)); }
static void als_put64(uint8_t *p, uint64_t v)
{ for (unsigned i = 0; i < 8; i++) p[i] = (uint8_t)(v>>(i*8)); }
static uint8_t als_crc8(const uint8_t *p, unsigned bytes)
{
 uint8_t c = 0;
 for (unsigned i = 0; i < bytes; i++) {
  c ^= p[i]; for (unsigned j = 0; j < 8; j++) c = (c>>1)^((c&1)?0x8c:0);
 }
 return c;
}
static uint32_t als_crc32(const uint8_t *p, unsigned bytes)
{
 uint32_t c = 0xffffffff;
 for (unsigned i = 0; i < bytes; i++) {
  c ^= p[i]; for (unsigned j = 0; j < 8; j++) c = (c>>1)^((c&1)?0xedb88320:0);
 }
 return c^0xffffffff;
}
static uint64_t als_smc(uint32_t fid, unsigned op, unsigned arg)
{
 register uint64_t x0 __asm__("x0") = fid, x1 __asm__("x1") = op;
 register uint64_t x2 __asm__("x2") = arg, x3 __asm__("x3") = 0;
 register uint64_t x4 __asm__("x4") = 0, x5 __asm__("x5") = 0;
 register uint64_t x6 __asm__("x6") = 0, x7 __asm__("x7") = 0;
 __asm__ volatile("dsb sy\nsmc #0\ndsb sy\nisb" :
     "+r"(x0),"+r"(x1),"+r"(x2),"+r"(x3),"+r"(x4),"+r"(x5),"+r"(x6),"+r"(x7) ::
     "memory","cc","x8","x9","x10","x11","x12","x13","x14","x15","x16","x17");
 lck_mtx_lock(&x9_als_lock); status.last_smc = (int64_t)x0; lck_mtx_unlock(&x9_als_lock);
 printf("X9 ALS SCP: SMC %x op=%u arg=%u -> %llx\n", fid, op, arg, x0);
 return x0;
}
static int als_smc_ok(uint32_t fid, unsigned op, unsigned arg)
{
 uint64_t r = als_smc(fid, op, arg);
 return !r ? 0 : (int64_t)r == -1 ? ENOTSUP : EIO;
}

/* Receive only our pins. W1C only the copied pin, never another subsystem's
 * IRQ. Unknown/late ACKs are discarded, not accepted as another command. */
static int als_poll(void)
{
 uint32_t bit = 1u<<als_layout.ready_pin.pin_index;
 if (als_rr(als_mb0, 0x10c)&bit) {
  uint32_t image = als_rr(als_mb0, als_layout.ready_pin.offset_bytes);
  als_wr(als_mb0, 0x10c, bit);
  lck_mtx_lock(&x9_als_lock);
  int duplicate = status.firmware_ready && status.stage > X9_ALS_BOOT;
  if (image == als_layout.sram_size && !duplicate) status.firmware_ready = 1;
  lck_mtx_unlock(&x9_als_lock);
  printf("X9 ALS SCP: READY channel22 image=%x%s\n", image, duplicate?" (remote restart)":"");
  if (image != als_layout.sram_size || duplicate) return EPIPE;
 }
 if ((als_rr(als_core0, 0x30)|als_rr(als_core1, 0x30))&1) return EIO;
 const struct x9_scp_pin *ack = &als_layout.sensor_pin[1], *notify = &als_layout.sensor_pin[3];
 uint32_t irqs = als_rr(als_mb1, 0x10c);
 if (irqs&(1u<<ack->pin_index)) {
  uint8_t p[8]; als_from_io(p, als_mb1, ack->offset_bytes, sizeof(p));
  als_wr(als_mb1, 0x10c, 1u<<ack->pin_index);
  if (als_ack_pending && !als_ack_received && p[0] == als_expected_sequence &&
      p[1] == als_expected_type && p[2] == als_expected_command && p[4] == als_crc8(p,4)) {
   memcpy(als_ack, p, sizeof(p)); als_ack_received = 1;
  } else {
   lck_mtx_lock(&x9_als_lock); status.rejected++; lck_mtx_unlock(&x9_als_lock);
  }
 }
 if (irqs&(1u<<notify->pin_index)) {
  uint8_t n[28]; als_from_io(n, als_mb1, notify->offset_bytes, sizeof(n));
  als_wr(als_mb1, 0x10c, 1u<<notify->pin_index);
  lck_mtx_lock(&x9_als_lock); status.packets++; lck_mtx_unlock(&x9_als_lock);
  if (n[4] != als_crc8(n,4) || n[3] > 20) {
   lck_mtx_lock(&x9_als_lock); status.rejected++; lck_mtx_unlock(&x9_als_lock);
   return 0;
  }
  if (n[1] == 0 && n[2] == 4 && n[3] == 0) {
   lck_mtx_lock(&x9_als_lock);
   int duplicate = status.sensor_ready && status.stage >= X9_ALS_SHARED;
   status.sensor_ready = 1; lck_mtx_unlock(&x9_als_lock);
   if (duplicate) return EPIPE;
  } else if (n[1] == 0 && n[2] == 9 && n[3] == 12 && n[0] == als_expected_sequence) {
   memcpy(als_reply, n, sizeof(n)); als_reply_received = 1;
  } else if (n[3] == 20 && (n[2] == 0 || n[2] == 1)) {
   als_wp = le32(n+8);
   if (!als_data_bytes || als_wp >= als_data_bytes || als_wp%36) return EBADMSG;
  } else if (n[3] == 20 && (n[2] == 2 || n[2] == 3)) {
   als_super_wp = le32(n+8);
   if (!als_super_bytes || als_super_wp >= als_super_bytes || als_super_wp%76) return EBADMSG;
  }
 }
 return 0;
}

/* OPPO scp_awake: magic A0 plus AP_AWAKE_LOCK bit0, wait for remote
 * consumption, then W1C the owned request. Keep one remote awake reference
 * across the stream. No shared wake-count fiction or force-reset fallback. */
static int als_wake(void)
{
 if (als_rr(als_infra, 0xb10)&0xaf) return EBUSY;
 als_wr(als_infra, 0xb14, 0xa1);
 uint64_t until = als_now_ns()+100000000;
 int error = ETIMEDOUT;
 do {
  uint32_t v = als_rr(als_infra, 0xb10);
  if ((v&0xa0) != 0xa0) { error = EIO; break; }
  if (!(v&1)) { als_awake = 1; error = 0; break; }
  if ((als_rr(als_core0, 0x30)|als_rr(als_core1, 0x30))&1) { error = EIO; break; }
  delay(10);
 } while (als_now_ns() < until);
 als_wr(als_infra, 0xb18, 0xa1);
 return error;
}
static int als_send(unsigned pin_index, const uint8_t *message, unsigned bytes)
{
 const struct x9_scp_pin *pin = &als_layout.sensor_pin[pin_index];
 if (!als_awake || bytes > pin->message_bytes || bytes%4) return EINVAL;
 uint32_t bit = 1u<<pin->pin_index;
 if (als_rr(als_mb1, 0x100)&bit) return EBUSY;
 uint8_t full[64] = {0}; memcpy(full, message, bytes);
 als_to_io(als_mb1, pin->offset_bytes, full, pin->message_bytes);
 als_wr(als_mb1, 0x100, bit);
 return 0;
}
static int als_control(unsigned type, unsigned command, const uint8_t *data, unsigned bytes)
{
 if (bytes > 56 || bytes%4) return EINVAL;
 uint8_t p[64] = {0}; p[0] = als_sequence++; p[1] = (uint8_t)type; p[2] = (uint8_t)command; p[3] = (uint8_t)bytes;
 p[4] = als_crc8(p,4); if (bytes) memcpy(p+8, data, bytes);
 als_expected_sequence = p[0]; als_expected_type = (uint8_t)type; als_expected_command = (uint8_t)command;
 als_ack_received = 0; als_ack_pending = 1; /* BEFORE mailbox SET */
 int error = als_send(0, p, 8+bytes);
 uint64_t until = als_now_ns()+100000000;
 while (!error && !als_ack_received && als_now_ns() < until) {
  error = als_poll(); if (!error && !als_ack_received) als_sleep(1);
 }
 als_ack_pending = 0;
 if (!error && !als_ack_received) error = ETIMEDOUT;
 if (!error && (int8_t)als_ack[3] < 0) error = EIO;
 printf("X9 ALS SCP: control type=%u cmd=%u seq=%u -> %d ret=%d\n",
     type, command, p[0], error, als_ack_received?(int8_t)als_ack[3]:-128);
 return error;
}
static int als_notify(unsigned command, const uint8_t *data, unsigned bytes)
{
 if (bytes > 20 || bytes%4) return EINVAL;
 uint8_t n[28] = {0}; n[0] = als_sequence++; n[2] = (uint8_t)command; n[3] = (uint8_t)bytes;
 n[4] = als_crc8(n,4); if (bytes) memcpy(n+8, data, bytes);
 als_expected_sequence = n[0];
 return als_send(2, n, sizeof(n));
}
static int als_in_firmware(uint32_t address, uint32_t bytes)
{
 return bytes && address >= als_layout.firmware_base &&
     address-als_layout.firmware_base < als_layout.firmware_size &&
     bytes <= als_layout.firmware_size-(address-als_layout.firmware_base);
}
static int als_prepare(void)
{
 als_stage(X9_ALS_LAYOUT);
 int error = x9_scp_read_layout(&als_layout); if (error) return error;
 lck_mtx_lock(&x9_als_lock); status.layout_valid = 1; lck_mtx_unlock(&x9_als_lock);
 als_stage(X9_ALS_REGION);
 uint32_t region[8];
 for (unsigned i = 0; i < 8; i++) {
  uint64_t v = als_smc(0xc2000528, 10, i);
  if (v > 0xffffffffULL) return ENOTSUP;
  region[i] = (uint32_t)v;
 }
 if (region[7] < 56 || region[7] > 72 || region[7]%4 ||
     !als_in_firmware(region[0],region[1]) || !als_in_firmware(region[2],region[3]) ||
     !als_in_firmware(region[4],region[5])) return ENOEXEC;
 als_stage(X9_ALS_POWER);
 /* Resource op1 26M|INFRA, SCP op12 DRAM: the Linux exact API. */
 error = als_smc_ok(0xc2000232, 1, 3); if (error) return error;
 lck_mtx_lock(&x9_als_lock); status.power_requested = 1; lck_mtx_unlock(&x9_als_lock);
 error = als_smc_ok(0xc2000528, 12, 0); if (error) return error;
 als_vlp = (volatile uint32_t *)ml_io_map(als_layout.vlp_base, 0x1000);
 als_core0 = (volatile uint32_t *)ml_io_map(als_layout.core0_base, 0x1000);
 als_core1 = (volatile uint32_t *)ml_io_map(als_layout.core1_base, 0x1000);
 als_infra = (volatile uint32_t *)ml_io_map(als_layout.scpsys_base, 0x1000);
 als_mb0 = (volatile uint32_t *)ml_io_map(als_layout.mbox0_base, 0x1000);
 als_mb1 = (volatile uint32_t *)ml_io_map(als_layout.mbox_base, 0x1000);
 if (!als_vlp || !als_core0 || !als_core1 || !als_infra || !als_mb0 || !als_mb1) return ENOMEM;
 als_stage(X9_ALS_CLOCK);
 uint32_t reset = als_rr(als_core0,0), pc = als_rr(als_core0,0x80);
 als_old_parent = als_rr(als_vlp,0xb0)&3;
 printf("X9 ALS SCP: LK reset=%x pc=%x clk-parent=%u irq0=%x irq1=%x\n",
     reset, pc, als_old_parent, als_rr(als_mb0,0x10c), als_rr(als_mb1,0x10c));
 /* Never reparent a running coprocessor. Only release an LK-held core;
  * no RESET_SET, RESTORE_TCM/DRAM or guessed reset-state recovery. */
 if (!(reset&1)) {
  als_wr(als_vlp,0xb8,3); als_wr(als_vlp,8,1u<<9);
  uint64_t until = als_now_ns()+5000000;
  while ((als_rr(als_vlp,8)&(1u<<9)) && als_now_ns() < until) delay(10);
  if ((als_rr(als_vlp,8)&(1u<<9)) || (als_rr(als_vlp,0xb0)&3)) return ETIMEDOUT;
 }
 als_stage(X9_ALS_BOOT);
 if (!(reset&1)) { error = als_smc_ok(0xc2000528,3,0); if (error) return error; }
 uint64_t until = als_now_ns()+5000000000ULL;
 struct x9_als_status st;
 do {
  error = als_poll(); if (error) return error;
  x9_als_copy_status(&st); if (st.firmware_ready) break;
  if (!als_alive()) return ECANCELED;
  als_sleep(2);
 } while (als_now_ns() < until);
 if (!st.firmware_ready) return ETIMEDOUT;
 /* TFA sets this shared-memory window on secure RESET_RELEASE. Confirm
  * before mapping/zeroing it; a DT layout alone cannot authorize writes. */
 uint32_t base = als_rr(als_core0,0x44), size = als_rr(als_core0,0x48);
 printf("X9 ALS SCP: firmware DRAM reserve=%x/%x expected=%x/%x\n",
     base,size,als_layout.reserve_base,als_layout.reserve_size);
 if (base != als_layout.reserve_base || size != als_layout.reserve_size) return ESTALE;
 error = als_wake(); if (error) return error;
 als_stage(X9_ALS_HOST_READY);
 error = als_notify(4,NULL,0); if (error) return error;
 until = als_now_ns()+5000000000ULL;
 do {
  error = als_poll(); if (error) return error;
  x9_als_copy_status(&st); if (st.sensor_ready) return 0;
  if (!als_alive()) return ECANCELED;
  als_sleep(2);
 } while (als_now_ns() < until);
 return ETIMEDOUT;
}
static int als_share(void)
{
 als_stage(X9_ALS_SHARED);
 const unsigned ids[] = {2,8,9,10};
 volatile uint32_t **maps[] = {&als_data,&als_super,&als_tx,&als_rx};
 for (unsigned i = 0; i < 4; i++) {
  const struct x9_scp_mem_region *r = &als_layout.memory[ids[i]];
  *maps[i] = (volatile uint32_t *)ml_io_map(als_layout.reserve_base+r->offset, r->size);
  if (!*maps[i]) return ENOMEM;
 }
 als_data_bytes = ((als_layout.memory[2].size-16)/36)*36;
 als_super_bytes = ((als_layout.memory[8].size-16)/76)*76;
 /* Host owns rp, firmware owns wp AFTER the SHARE_MEMORY ACK. */
 als_wr(als_data,0,0); als_wr(als_data,4,0);
 als_wr(als_data,8,als_data_bytes); als_wr(als_data,12,36);
 als_wr(als_super,0,0); als_wr(als_super,4,0);
 als_wr(als_super,8,als_super_bytes); als_wr(als_super,12,76);
 als_wr(als_tx,0,0xdeadbeef); als_wr(als_tx,8192-4,0xdeadbeef);
 als_wr(als_rx,0,0xdeadbeef); als_wr(als_rx,8192-4,0xdeadbeef);
 uint8_t config[36] = {2}; /* available_num + padding + 4 base_info[8] */
 config[4] = 0; als_put32(config+8,als_layout.reserve_base+als_layout.memory[2].offset);
 config[12] = 1; als_put32(config+16,als_layout.reserve_base+als_layout.memory[8].offset);
 return als_control(0,11,config,sizeof(config));
}
static int als_timesync(void)
{
 uint8_t p[48] = {0}; uint64_t counter;
 clock_sec_t sec; clock_usec_t usec; clock_get_calendar_microtime(&sec,&usec);
 __asm__ volatile("isb; mrs %0,cntvct_el0" : "=r"(counter) :: "memory");
 uint64_t now = als_now_ns(); als_put64(p,now); als_put64(p+8,counter); als_put64(p+16,now);
 als_put32(p+24,usec); als_put32(p+28,(uint64_t)sec%60);
 als_put32(p+32,((uint64_t)sec/60)%60); als_put32(p+36,((uint64_t)sec/3600)%24);
 /* Gregorian UTC, month is zero-based as Android rtc_time.tm_mon. */
 uint64_t days = (uint64_t)sec/86400; unsigned year = 1970, month = 0;
 while (days >= 365u+((year%4==0 && year%100!=0)||year%400==0)) {
  days -= 365u+((year%4==0 && year%100!=0)||year%400==0); year++;
 }
 const unsigned mdays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
 while (month < 11) {
  unsigned n = mdays[month]+(month==1 && ((year%4==0 && year%100!=0)||year%400==0));
  if (days < n) break; days -= n; month++;
 }
 als_put32(p+40,(uint32_t)days+1); als_put32(p+44,month);
 return als_control(0,10,p,sizeof(p));
}
static int als_list(void)
{
 als_stage(X9_ALS_LIST);
 bzero(als_snapshot,sizeof(als_snapshot));
 als_put32(als_snapshot,0xdeadbeef); als_put32(als_snapshot+8192-4,0xdeadbeef);
 als_put32(als_snapshot+16,4096); als_put32(als_snapshot+20,4096);
 als_put32(als_snapshot+4,als_crc32(als_snapshot+8,16));
 als_to_io(als_tx,0,als_snapshot,sizeof(als_snapshot));
 uint8_t header[12]; memcpy(header,als_snapshot+8,sizeof(header));
 als_reply_received = 0;
 int error = als_notify(9,header,sizeof(header));
 uint64_t until = als_now_ns()+100000000;
 while (!error && !als_reply_received && als_now_ns() < until) {
  error = als_poll(); if (!error && !als_reply_received) als_sleep(1);
 }
 if (error) return error;
 if (!als_reply_received) return ETIMEDOUT;
 als_from_io(als_snapshot,als_rx,0,sizeof(als_snapshot));
 const uint8_t *p = als_snapshot, *h = p+8, *n = als_reply+8;
 uint32_t length = le32(h+8), capacity = le32(p+20), gain = 0;
 const uint8_t *prox_info = NULL;
 if (le32(p) != 0xdeadbeef || le32(p+8192-4) != 0xdeadbeef ||
     h[1] || h[2] || h[3] || le32(h+4) || n[1] || n[2] || n[3] ||
     le32(n+4) || le32(n+8) != length || length > 4096 || length%40 ||
     length > capacity || capacity > 4096 || le32(p+4) != als_crc32(h,16+length)) return EBADMSG;
 for (unsigned i = 0; i < length; i += 40) {
  const uint8_t *info = p+24+i;
  if (info[0] == X9_PROX_MTK_TYPE) {
   if (prox_info || !le32(info+4)) return EBADMSG;
   prox_info = info; continue;
  }
  if (info[0] != 5) continue; /* LIGHT, not LUX_AOD / rear proximity */
  if (gain || !le32(info+4)) return EBADMSG;
  gain = le32(info+4);
  printf("X9 ALS SCP: LIGHT name=%.*s vendor=%.*s gain=%u\n",16,info+8,16,info+24,gain);
 }
 if (!gain && !prox_info) return ENODEV;
 /* Install neither sensor from a partially validated LIST. Keep a complete
  * type8 info snapshot so a late proximity HID can attach without a new IPI. */
 if (prox_info) {
  memcpy(als_prox_info,prox_info,sizeof(als_prox_info)); als_prox_present = 1;
  printf("X9 PROX SCP: LIST name=%.*s vendor=%.*s gain=%u\n",16,prox_info+8,
      16,prox_info+24,le32(prox_info+4));
 }
 als_list_complete = 1;
 lck_mtx_lock(&x9_als_lock);
 mtk_gain = status.gain = gain; last_scp_time = 0; valid = 0;
 lck_mtx_unlock(&x9_als_lock); return 0;
}
static int als_enable(uint32_t interval_us)
{
 als_stage(X9_ALS_ENABLE);
 uint8_t batch[16] = {0}; als_put64(batch,(uint64_t)interval_us*1000);
 lck_mtx_lock(&x9_als_lock); status.control_enabled = valid = 0; lck_mtx_unlock(&x9_als_lock);
 int error = als_control(5,1,batch,sizeof(batch));
 if (!error) {
  lck_mtx_lock(&x9_als_lock);
  status.control_enabled = 1; status.report_interval_us = interval_us;
  lck_mtx_unlock(&x9_als_lock); als_stage(X9_ALS_STREAM);
 }
 return error;
}
static int als_prox_service(void)
{
 struct x9_prox_status prox; x9_prox_copy_status(&prox);
 if (als_list_complete && prox.mode == 3 && !prox.error && !prox.configured) {
  if (!als_prox_present) x9_prox_native_disconnect(ENODEV);
  else {
   int error = x9_prox_native_configure(als_prox_info,sizeof(als_prox_info));
   if (error && error != EAGAIN && error != ENXIO) return error;
  }
 }
 uint32_t pending,enabled,interval;
 lck_mtx_lock(&x9_als_lock);
 pending = als_prox_pending; enabled = als_prox_enabled; interval = als_prox_interval;
 als_prox_pending = 0;
 lck_mtx_unlock(&x9_als_lock);
 if (!pending) return 0;
 /* A request can expire while the shared worker is busy with another
  * sensor. Never send a queued control after its owner was quarantined. */
 x9_prox_copy_status(&prox);
 if (prox.mode != 3 || prox.error || !prox.pending) return 0;
 uint8_t batch[16] = {0}; als_put64(batch,(uint64_t)interval*1000);
 int error = als_control(X9_PROX_MTK_TYPE,enabled ? 1 : 0,
     enabled ? batch : NULL,enabled ? sizeof(batch) : 0);
 x9_prox_native_control(enabled,error);
 return error;
}
static int als_drain(volatile uint32_t *base, uint32_t wp, uint32_t bytes, unsigned item_size)
{
 uint32_t rp = als_rr(base,0);
 if (als_rr(base,8) != bytes || als_rr(base,12) != item_size || rp >= bytes || rp%item_size) return EBADMSG;
 for (unsigned i = 0; rp != wp && i < 128; i++) {
  uint8_t record[76]; als_from_io(record,base,16+rp,item_size);
  if (record[0] == 5 && record[1] == 0) {
   /* Super records share the header/value[0] prefix, but only the first
    * complete normal 36-byte record is passed to the existing decoder. */
   int error = x9_als_receive_mtk(record,36);
   lck_mtx_lock(&x9_als_lock); status.records++;
   if (error) status.rejected++;
   lck_mtx_unlock(&x9_als_lock);
   /* Already queued DATA may outlive the LIGHT DISABLE ACK. It is not a
    * transport fault, just as a type8 sample before ENABLE is not one. */
   if (error && error != EALREADY && error != ERANGE && error != ENOTSUP &&
       error != ENXIO && error != EAGAIN && error != EBUSY) return error;
  }
  if (record[0] == X9_PROX_MTK_TYPE && record[1] == 0) {
   /* Both FIFO forms preserve the 36B prefix, including PS value[0..2]. */
   int error = x9_prox_native_data(record,X9_PROX_DATA_BYTES);
   if (error && error != EALREADY && error != ERANGE && error != ENOTSUP &&
       error != ENXIO && error != EAGAIN && error != EBUSY) return error;
  }
  rp = (rp+item_size)%bytes;
  /* Finish the copy before handing this slot back to firmware. */
  __asm__ volatile("dmb sy" ::: "memory"); als_wr(base,0,rp);
 }
 return 0;
}
static void als_worker(void *arg __unused, wait_result_t result __unused)
{
 int error = als_prepare();
 if (!error && !als_alive()) error = ECANCELED;
 if (!error) error = als_share();
 if (!error) { als_stage(X9_ALS_TIMESYNC); error = als_timesync(); }
 if (!error) error = als_list();
 lck_mtx_lock(&x9_als_lock); int light = ingress_claimed && mtk_gain;
 lck_mtx_unlock(&x9_als_lock);
 if (!error && light) error = als_enable(requested_interval_us);
 if (!error) { als_stage(X9_ALS_STREAM); error = als_prox_service(); }
 uint64_t sync_deadline = als_now_ns()+10000000000ULL;
 while (!error && als_alive()) {
  error = als_poll();
  if (!error) error = als_prox_service();
  if (!error) error = als_drain(als_data,als_wp,als_data_bytes,36);
  if (!error) error = als_drain(als_super,als_super_wp,als_super_bytes,76);
  lck_mtx_lock(&x9_als_lock);
  uint32_t wanted = requested_interval_us, applied = status.report_interval_us;
  int want_light = ingress_claimed && mtk_gain, light_enabled = status.control_enabled;
  lck_mtx_unlock(&x9_als_lock);
  if (!error && want_light && (!light_enabled || wanted != applied)) error = als_enable(wanted);
  if (!error && !want_light && light_enabled) {
   error = als_control(5,0,NULL,0);
   if (!error) {
    lck_mtx_lock(&x9_als_lock); status.control_enabled = valid = 0;
    lck_mtx_unlock(&x9_als_lock);
   }
  }
  if (!error && als_now_ns() >= sync_deadline) {
   error = als_timesync(); sync_deadline = als_now_ns()+10000000000ULL;
  }
  if (!error) als_sleep(5);
 }
 if (error) als_fault(error);
 else {
  als_stage(X9_ALS_DISABLE);
  /* A matching ACK, not shutdown(), proves the remote accepted DISABLE. */
  error = als_control(5,0,NULL,0);
  if (!error && als_prox_present) error = als_control(X9_PROX_MTK_TYPE,0,NULL,0);
  if (!error) x9_prox_native_disconnect(ECANCELED);
  if (error) als_fault(error);
  else {
   lck_mtx_lock(&x9_als_lock); status.control_enabled = 0; lck_mtx_unlock(&x9_als_lock);
   if (als_awake) {
    /* Exact OPPO unlock request, no forced halt or power removal. */
    als_wr(als_infra,0xb14,0xa2);
    uint64_t until = als_now_ns()+100000000;
    while ((als_rr(als_infra,0xb10)&2) && als_now_ns() < until) delay(10);
    if (als_rr(als_infra,0xb10)&2) error = ETIMEDOUT;
    als_wr(als_infra,0xb18,0xa2);
    if (error) als_fault(error); else als_awake = 0;
   }
   if (!error) als_stage(X9_ALS_STOPPED);
  }
 }
 struct x9_als_status st; x9_als_copy_status(&st);
 printf("X9 ALS SCP: exit stage=%u errno=%d firmware=%u sensor=%u packets=%llu records=%llu; secure resources retained\n",
     st.stage,st.error,st.firmware_ready,st.sensor_ready,st.packets,st.records);
 lck_mtx_lock(&x9_als_lock); status.worker_running = 0; lck_mtx_unlock(&x9_als_lock);
 thread_terminate(current_thread());
}
static int als_start_worker(int shared)
{
 int binding = x9_prox_bind_transport(&als_prox_transport);
 if (binding) return binding;
 lck_mtx_lock(&x9_als_lock);
 if (test_mode || (!active && !shared)) {
  lck_mtx_unlock(&x9_als_lock); return EBUSY;
 }
 if (shared) shared_transport_requested = 1;
 if (transport_started) {
  int error = status.error ? status.error : !status.worker_running ? ENXIO : 0;
  lck_mtx_unlock(&x9_als_lock); return error;
 }
 active = 1; valid = 0; mtk_gain = 0; last_scp_time = 0;
 transport_started = physical_mode = 1; status.worker_running = 1;
 lck_mtx_unlock(&x9_als_lock);
 thread_t thread;
 kern_return_t kr = kernel_thread_start(als_worker,NULL,&thread);
 if (kr == KERN_SUCCESS) { thread_deallocate(thread); return 0; }
 als_fault(ENOMEM);
 lck_mtx_lock(&x9_als_lock); status.worker_running = 0; lck_mtx_unlock(&x9_als_lock);
 return ENOMEM;
}
int x9_als_start_transport(void) { return als_start_worker(0); }
int x9_als_start_shared_transport(void)
{
 extern int sweet_x9_arg(const char *, int);
 if (sweet_x9_arg("x9_als",0) == 2) return EBUSY;
 return als_start_worker(1);
}
