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
static int x9_lk_gui_disarm(void) {
 if(__atomic_exchange_n(&x9_lk_submit_lock,1u,__ATOMIC_ACQUIRE))return 0;
 x9_lk_gui_armed=0;
 __atomic_store_n(&x9_lk_submit_lock,0u,__ATOMIC_RELEASE);
 return 1;
}

#endif
