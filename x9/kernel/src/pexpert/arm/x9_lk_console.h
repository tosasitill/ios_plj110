/* X9 inherited LK command-mode scanout. Included only by pe_serial.c.
 * All addresses/fields match the owner-verified pan50/51 shim path.
 * Preserve CLK/PHY/DSC/route/mutex configuration. GUI also acknowledges only
 * TE_RDY in DSI_INTSTA before waiting for a fresh panel TE.
 * Macros let host tests run this exact code against MMIO/mapping mocks.
 */
#ifndef X9_LK_CONSOLE_H
#define X9_LK_CONSOLE_H
#define X9_LK_FB 0xfc910000ULL
#define X9_LK_RESERVE 0x3660000ULL
#define X9_LK_BYTES (5120u * 2760u)
#define X9_LK_SLOT (5120u * 2784u)
#ifndef X9_LK_MAP_IO
#define X9_LK_MAP_IO(pa) ((volatile uint32_t *)(uintptr_t)ml_io_map((pa), 0x1000))
#define X9_LK_MAP_FB(pa) ((volatile uint32_t *)(uintptr_t)ml_io_map_wcomb((pa), (X9_LK_BYTES + 0x3fff) & ~0x3fff))
#define X9_LK_READ(p, off) ((p)[(off) / 4])
#define X9_LK_WRITE(p, off, value) ((p)[(off) / 4] = (value))
#define X9_LK_FENCE() __asm__ volatile("dsb sy" ::: "memory")
#define X9_LK_PET() sweet_watchdog_pet()
#endif
static volatile uint32_t *x9_lk_dsi, *x9_lk_mutex[4];
static volatile uint32_t *x9_lk_buffers[4];
static uint64_t x9_lk_addresses[4];
static unsigned x9_lk_count, x9_lk_id, x9_lk_mask, x9_lk_active;
static volatile uint32_t *x9_lk_canonical;
static unsigned x9_lk_submit_lock;
static unsigned x9_lk_gui_armed;
/* Every scanned EXDMA engine, kept for the read-only layer dump. */
static volatile uint32_t *x9_lk_engines[13];
static uint64_t x9_lk_engine_pa[13];

static int
x9_lk_layer(volatile uint32_t *p, uint64_t *address, unsigned *swap)
{
 uint32_t en = X9_LK_READ(p, 0x20), layer = X9_LK_READ(p, 0x40);
 uint32_t size = X9_LK_READ(p, 0x48), fmt = X9_LK_READ(p, 0x50);
 uint32_t high = X9_LK_READ(p, 0xf4c);
 *address = X9_LK_READ(p, 0xf40) | ((uint64_t)(high & 15) << 32);
 *swap = (fmt >> 16) & 1u;
 return (en & 1) && (layer & 1) && !(layer & 0x310u) &&
        !(X9_LK_READ(p, 0x300) & 0x600u) &&
        (X9_LK_READ(p, 0x2f4) & 0xffffu) == 5120 &&
        !(X9_LK_READ(p, 0x2f0) & 15u) &&
        (size & 0x3fffu) == 1256 && ((size >> 16) & 0x3fffu) == 2760 &&
        (fmt & 0x2031fu) == 2 && !(high & ~15u) &&
        !X9_LK_READ(p, 0x44) && !X9_LK_READ(p, 0x4c) &&
        *address >= X9_LK_FB && *address <= X9_LK_FB + X9_LK_RESERVE - X9_LK_BYTES &&
        (*address - X9_LK_FB) % X9_LK_SLOT == 0;
}

static int
x9_lk_prepare(volatile uint32_t *canonical)
{
 static const uint64_t engine[] = {
  0x32930000, 0x32950000, 0x32960000, 0x32970000,
  0x32980000, 0x32990000, 0x329a0000,
  0x32c50000, 0x32c60000, 0x32c70000, 0x32c80000, 0x32c90000, 0x32ca0000,
 };
 static const uint64_t bank[] = {0x3e320000,0x3e720000,0x32920000,0x32c20000};
 unsigned found = 0, format_seen = 0, first_swap = 0, matches = 0;
 x9_lk_active = x9_lk_count = 0;
 x9_lk_canonical = canonical;
 if (!canonical) return 0;
 x9_lk_dsi = X9_LK_MAP_IO(0x3e730000);
 if (!x9_lk_dsi || (X9_LK_READ(x9_lk_dsi, 0x34) & 3)) return 0;
 for (unsigned i = 0; i < 4; i++) {
  x9_lk_mutex[i] = X9_LK_MAP_IO(bank[i]);
  if (!x9_lk_mutex[i]) return 0;
 }
 for (unsigned id = 0; id < 8; id++) {
  unsigned mask = 0, single = 1;
  uint32_t mod0[4], mod1[4];
  for (unsigned b = 0; b < 4; b++) {
   unsigned o = id * 0x20;
   mod0[b] = X9_LK_READ(x9_lk_mutex[b], o + 0x34);
   mod1[b] = X9_LK_READ(x9_lk_mutex[b], o + 0x38);
   if (mod0[b] || mod1[b]) {
    mask |= 1u << b;
    if (X9_LK_READ(x9_lk_mutex[b], o + 0x30) & 7) single = 0;
   }
  }
  if (single && (mod1[1] & 1) && (mod0[2] & 0x18)) {
   matches++; x9_lk_id = id; x9_lk_mask = mask;
  }
 }
 if (matches != 1) return 0;
 for (unsigned i = 0; i < sizeof(engine) / sizeof(engine[0]); i++) {
  volatile uint32_t *p = X9_LK_MAP_IO(engine[i]);
  uint64_t address;
  unsigned swap;
  if (!p) return 0;
  x9_lk_engines[i] = p; x9_lk_engine_pa[i] = engine[i];
  if (!x9_lk_layer(p, &address, &swap)) continue;
  if (format_seen && first_swap != swap) return 0;
  format_seen = 1; first_swap = swap;
  found |= address == X9_LK_FB;
  unsigned duplicate = 0;
  for (unsigned j = 0; j < x9_lk_count; j++) duplicate |= x9_lk_addresses[j] == address;
  if (duplicate) continue;
  if (x9_lk_count == 4) return 0;
  x9_lk_addresses[x9_lk_count] = address;
  x9_lk_buffers[x9_lk_count] = address == X9_LK_FB ? canonical : X9_LK_MAP_FB(address);
  if (!x9_lk_buffers[x9_lk_count]) return 0;
  x9_lk_count++;
 }
 /* Serial glyphs are black/white, so the accepted RGB swap needs no conversion. */
 x9_lk_active = found && x9_lk_count;
 return (int)x9_lk_active;
}

/* -1: unavailable, 0: busy, 1: DSI start issued (not completion proof). */
static void x9_lk_mirror(void) {
 /* Publish GUI writes before reading the canonical alias for mirroring. */
 X9_LK_FENCE();
 for (unsigned i = 0; i < x9_lk_count; i++) {
  if (x9_lk_buffers[i] == x9_lk_canonical) continue;
  for (unsigned j = 0; j < X9_LK_BYTES / 4; j++) {
   x9_lk_buffers[i][j] = x9_lk_canonical[j];
   if (!(j & 0xffffu)) X9_LK_PET();
  }
 }
 X9_LK_FENCE();
}
static void x9_lk_start(void) {
 for (unsigned b = 0; b < 4; b++)
  if (x9_lk_mask & (1u << b)) X9_LK_WRITE(x9_lk_mutex[b], 0x20 + x9_lk_id * 0x20, 1);
 X9_LK_WRITE(x9_lk_dsi, 0, 0);
 X9_LK_WRITE(x9_lk_dsi, 0, 1);
 X9_LK_FENCE();
}
static int
x9_lk_present(void)
{
 if (!x9_lk_active) return -1;
 if (__atomic_exchange_n(&x9_lk_submit_lock, 1u, __ATOMIC_ACQUIRE)) return 0;
 int result = 0;
 if (x9_lk_gui_armed || (X9_LK_READ(x9_lk_dsi, 0x0c) & 0x80000000u)) goto out;
 x9_lk_mirror();
 if (X9_LK_READ(x9_lk_dsi, 0x0c) & 0x80000000u) goto out;
 x9_lk_start();
 result = 1;
out:
 __atomic_store_n(&x9_lk_submit_lock, 0u, __ATOMIC_RELEASE);
 return result;
}

/* GUI-only two-phase trigger. Stock MT6993 mtk_drm_crtc.c clears EVENT_TE
 * then waits for a new TE before enabling mutex/DSI. DSI_INTSTA bit2 is
 * TE_RDY; mtk_dsi.c IRQ handler acknowledges W0C status with ~status.
 * First call mirrors the completed frame and discards stale TE; later
 * workloop timer calls poll without holding the GPU owner or spinning.
 * No interrupt mask, TE routing, DCS, PHY or panel mode is reprogrammed. */
static int x9_lk_gui_step(void) {
 if (!x9_lk_active) return -1;
 if (__atomic_exchange_n(&x9_lk_submit_lock, 1u, __ATOMIC_ACQUIRE)) return 0;
 int result=0;
 uint32_t status=X9_LK_READ(x9_lk_dsi,0x0c);
 if(status==0xffffffffu){result=-1;goto out;}
 if(status&0x80000000u)goto out;
 if(!x9_lk_gui_armed) {
  x9_lk_mirror();
  X9_LK_WRITE(x9_lk_dsi,0x0c,~(1u<<2));
  X9_LK_FENCE();
  x9_lk_gui_armed=1;
  goto out;
 }
 if(!(status&(1u<<2)))goto out;
 x9_lk_start();
 x9_lk_gui_armed=0;
 result=1;
out:
 __atomic_store_n(&x9_lk_submit_lock,0u,__ATOMIC_RELEASE);
 return result;
}
/* Panel DCS long write (brightness, DCS 0x51) between frames.
 * Stock mtk_dsi_host_send_cmd: wait idle, write CMDQ0.. + CMDQ_SIZE|SIZE_SEL,
 * DSI_START 0->1->0, wait idle. Stock re-writes CMDQ0=0x002c3909/SIZE=1 before
 * every frame trigger (mtk_dsi.c cmd-mode trigger); x9_lk_start does not, it
 * reuses LK's values, so CMDQ0/1, CMDQ_CON (+0x44) and RX_CON (+0xa0) are
 * restored before the submit lock is released. LP only (config LONG, no
 * HSTX): pan36 lit the glass this way, pan35's HS variant got SoT errors.
 * Readback (optional) is the shim's verified V2 DCS read of 0x52 and 0x0A.
 * Returns 1 sent, 0 busy (frame in flight or lock held; retry), -1 unavailable,
 * -2 sent but BUSY never cleared. rd = raw 0x52 read word, raw 0x0A read
 * word ([7:0] DI, [15:8] data0, [23:16] data1), saved CMDQ0, saved CMDQ_CON. */
#ifndef X9_LK_DELAY_US
#define X9_LK_DELAY_US(us) delay(us)
#endif
static int x9_lk_wait_idle(unsigned us) {
 for (unsigned t = 0; t < us / 50; t++) {
  if (!(X9_LK_READ(x9_lk_dsi, 0x0c) & 0x80000000u)) return 1;
  X9_LK_DELAY_US(50);
 }
 return !(X9_LK_READ(x9_lk_dsi, 0x0c) & 0x80000000u);
}
static void x9_lk_kick(uint32_t size) {
 X9_LK_WRITE(x9_lk_dsi, 0x44, (X9_LK_READ(x9_lk_dsi, 0x44) & ~(0x3ffu | 0x8000u)) | size | 0x8000u);
 X9_LK_FENCE();
 X9_LK_WRITE(x9_lk_dsi, 0x00, 0);
 X9_LK_WRITE(x9_lk_dsi, 0x00, 1);
 X9_LK_FENCE();
 X9_LK_WRITE(x9_lk_dsi, 0x00, 0);
}
static uint32_t x9_lk_dcs_read(uint8_t cmd, uint8_t ret) {
 X9_LK_WRITE(x9_lk_dsi, 0x0c, ~1u);                       /* clear LPRX_RD_RDY only */
 X9_LK_WRITE(x9_lk_dsi, 0xa0, X9_LK_READ(x9_lk_dsi, 0xa0) | 1u); /* RX_DATA_SRAM_MODE */
 X9_LK_WRITE(x9_lk_dsi, 0x400, ((uint32_t)ret << 16) | (0x37u << 8));
 X9_LK_WRITE(x9_lk_dsi, 0x404, ((uint32_t)cmd << 16) | (0x06u << 8) | 0x4u);
 x9_lk_kick(2);
 for (unsigned t = 0; t < 200 && !(X9_LK_READ(x9_lk_dsi, 0x0c) & 1u); t++) X9_LK_DELAY_US(100);
 X9_LK_WRITE(x9_lk_dsi, 0x0c, ~1u);
 X9_LK_WRITE(x9_lk_dsi, 0x44, (X9_LK_READ(x9_lk_dsi, 0x44) & ~0x70000u) | 0x30000u);
 uint32_t w = X9_LK_READ(x9_lk_dsi, 0x5fc);
 X9_LK_WRITE(x9_lk_dsi, 0x44, X9_LK_READ(x9_lk_dsi, 0x44) & ~0x70000u);
 X9_LK_WRITE(x9_lk_dsi, 0xb4, X9_LK_READ(x9_lk_dsi, 0xb4) | 1u); /* RACK */
 (void)x9_lk_wait_idle(20000);
 return w;                 /* [7:0] response DI, [15:8] data0, [23:16] data1 */
}
static int x9_lk_dcs_long(const uint8_t *data, unsigned len, int readback, uint32_t rd[4]) {
 if (!x9_lk_active || len < 3 || len > 8) return -1;
 if (__atomic_exchange_n(&x9_lk_submit_lock, 1u, __ATOMIC_ACQUIRE)) return 0;
 int result = 0;
 uint32_t status = X9_LK_READ(x9_lk_dsi, 0x0c);
 if (status == 0xffffffffu) { result = -1; goto out; }
 if (status & 0x80000000u) goto out;      /* frame in flight */
 {
  uint32_t q0 = X9_LK_READ(x9_lk_dsi, 0x400), q1 = X9_LK_READ(x9_lk_dsi, 0x404);
  uint32_t q2 = X9_LK_READ(x9_lk_dsi, 0x408), con = X9_LK_READ(x9_lk_dsi, 0x44);
  uint32_t rx = X9_LK_READ(x9_lk_dsi, 0xa0);
  uint32_t words = (len + 3) / 4;
  X9_LK_WRITE(x9_lk_dsi, 0x400, 0);
  for (unsigned w = 0; w < words; w++) {
   uint32_t v = 0;
   for (unsigned b = 0; b < 4 && w * 4 + b < len; b++) v |= (uint32_t)data[w * 4 + b] << (b * 8);
   X9_LK_WRITE(x9_lk_dsi, 0x404 + w * 4, v);
  }
  X9_LK_WRITE(x9_lk_dsi, 0x400, ((uint32_t)len << 16) | (0x39u << 8) | 0x2u);
  x9_lk_kick(1 + words);
  result = x9_lk_wait_idle(20000) ? 1 : -2;
  X9_LK_WRITE(x9_lk_dsi, 0x0c, ~2u);     /* ack CMD_DONE only; TE_RDY untouched */
  if (rd) {
   rd[0] = rd[1] = 0; rd[2] = q0; rd[3] = con;
   if (readback && result == 1) {
    rd[0] = x9_lk_dcs_read(0x52, 2);
    rd[1] = x9_lk_dcs_read(0x0a, 1);
    X9_LK_WRITE(x9_lk_dsi, 0x0c, ~2u);
   }
  }
  X9_LK_WRITE(x9_lk_dsi, 0x400, q0);
  X9_LK_WRITE(x9_lk_dsi, 0x404, q1);
  X9_LK_WRITE(x9_lk_dsi, 0x408, q2);
  X9_LK_WRITE(x9_lk_dsi, 0xa0, rx);
  X9_LK_WRITE(x9_lk_dsi, 0x44, con);
  X9_LK_FENCE();
 }
out:
 __atomic_store_n(&x9_lk_submit_lock, 0u, __ATOMIC_RELEASE);
 return result;
}
static int x9_lk_gui_disarm(void) {
 if(__atomic_exchange_n(&x9_lk_submit_lock,1u,__ATOMIC_ACQUIRE))return 0;
 x9_lk_gui_armed=0;
 __atomic_store_n(&x9_lk_submit_lock,0u,__ATOMIC_RELEASE);
 return 1;
}

#endif
