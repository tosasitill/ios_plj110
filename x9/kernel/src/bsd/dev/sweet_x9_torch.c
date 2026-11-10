/* MT6993/MT6379 torch-only bring-up, default OFF.
 * References: captured X9 DT /spmi@1c01a000/mt6379@e/led-controller;
 * OPPO kernel_device_modules-6.12 drivers/leds/flash/leds-mt6379.c,
 * drivers/mfd/mt6379{,-spmi}.c; hackios hakios.torch.c (bounded /dev flow).
 * This is NOT the Qualcomm PM8350C backend. Only ITORCH(channel) and
 * FLED_EN are writable; no strobe, charger, IRQ acknowledge, reset or rail
 * changes. Every write is read back, including shutdown. PMIF-P SWINF2 is
 * serialized with the existing gauge/touch X9PMIF lock.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/kauth.h>
#include <sys/uio.h>
#include <miscfs/devfs/devfs.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <pexpert/pexpert.h>
#include "x9_torch.h"

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern int sweet_early_is_x9(void);
extern int sweet_x9_arg(const char *, int);
extern int X9PMIFAcquire(void);
extern void X9PMIFRelease(void);
extern int X9BatteryTorchTemperature(int *centi);

LCK_GRP_DECLARE(x9_torch_group, "x9-torch");
LCK_MTX_DECLARE(x9_torch_lock, &x9_torch_group);
static vm_offset_t bus;
static struct x9_torch_status st = { .version = 1 };
static uint64_t lit_at;
static bool writer, owned;
static unsigned enable_mask;
void sweet_x9_torch_init(void);

static uint64_t now_ns(void)
{
    uint64_t t, ns; clock_get_uptime(&t); absolutetime_to_nanoseconds(t, &ns); return ns;
}
static uint32_t rr(unsigned off) { return *(volatile uint32_t *)(bus + off); }
static void rw(unsigned off, uint32_t v)
{
    *(volatile uint32_t *)(bus + off) = v; __asm__ volatile("dsb sy" ::: "memory");
}
static int wait_state(unsigned expected)
{
    for (unsigned i = 0; i < 1000; i++) {
        uint32_t s = rr(0x8a8);
        if (s & (1u << 18)) return EIO;
        if (((s >> 1) & 7) == expected) return 0;
        delay(1);
    }
    return ETIMEDOUT; /* Never clear/recover an unknown SWINF transaction. */
}
/* Called only while X9PMIF is held. Preserve the MT6379 >=8us
 * overlapping-write/read spacing (mt6379-spmi.c). */
static int get_byte(unsigned reg, uint8_t *v)
{
    int e = wait_state(0); if (e) return e;
    rw(0x880, (3u << 30) | (14u << 24) | reg);
    e = wait_state(6); if (e) return e;
    *v = (uint8_t)rr(0x894); rw(0x8a4, 1); return wait_state(0);
}
static int put_byte(unsigned reg, uint8_t value)
{
    if (st.mode < 2 || (reg != 0x20e && reg != 0x204 + st.channel * 3)) return EPERM;
    int e = wait_state(0); if (e) return e;
    rw(0x884, value); rw(0x880, (3u << 30) | (1u << 29) | (14u << 24) | reg);
    e = wait_state(0); if (e) return e;
    delay(9); uint8_t back;
    e = get_byte(reg, &back); if (e) return e;
    return back == value ? 0 : EIO;
}
static int snapshot(void)
{
    uint8_t a, b, c, en;
    int e = get_byte(0x80, &a); if (!e) e = get_byte(0x81, &b);
    if (!e) e = get_byte(0x82, &c); if (!e) e = get_byte(0x20e, &en);
    if (!e) { st.enable = en; st.fault = a | ((unsigned)b << 8) | ((unsigned)c << 16); }
    return e;
}
static bool faulted(void)
{
    return (st.fault & 0xe8) || ((st.fault >> 8) & (1u << (4 - st.channel))) ||
        ((st.fault >> 16) & (1u << (7 - st.channel)));
}
/* Never announce dark until readback proves it. Attempts remain bounded;
 * failed shutdown is latched and retried by the watchdog, not hidden. */
static int dark(void)
{
    if (!owned) return 0;
    uint8_t en; int e = X9PMIFAcquire() ? EBUSY : 0;
    if (!e) {
        e = get_byte(0x20e, &en);
        if (!e) e = put_byte(0x20e, en & ~enable_mask);
        if (!e) st.enable = en & ~enable_mask;
        X9PMIFRelease();
    }
    if (e) { st.flags |= X9_TORCH_LATCHED | X9_TORCH_OFF_FAILED; st.last_errno = e; }
    else { st.level = st.current_ua = 0; lit_at = 0; owned = false; st.flags &= ~X9_TORCH_OFF_FAILED; }
    return e;
}
static int set_level(uint32_t level)
{
    if (level > 1000) return EINVAL;
    if (st.mode < 2 || !(st.flags & X9_TORCH_READY)) return ENXIO;
    if (!level) {
        int e = dark(); if (!e) st.flags &= ~X9_TORCH_NEEDS_ZERO; return e;
    }
    if (st.flags & (X9_TORCH_LATCHED | X9_TORCH_NEEDS_ZERO)) return EIO;
    if (lit_at && now_ns() - lit_at >= (uint64_t)st.max_seconds * 1000000000) {
        st.flags |= X9_TORCH_NEEDS_ZERO; (void)dark(); return ETIMEDOUT;
    }
    int temp;
    if (X9BatteryTorchTemperature(&temp) || temp < 0 || temp >= 4500) {
        (void)dark(); return EAGAIN; /* Missing/stale gauge is not a safe temperature. */
    }
    int e = X9PMIFAcquire() ? EBUSY : 0;
    unsigned ua = st.max_current_ua * level / 1000;
    if (ua < 25000) ua = 25000;
    unsigned code = (ua - 25000) / 5000;
    if (!e) {
        e = snapshot();
        if (!e && (faulted() || (st.enable & 8) || (st.enable & 7 & ~enable_mask))) e = EIO;
        if (!e) e = put_byte(0x204 + st.channel * 3, (uint8_t)code);
        if (!e) {
            owned = true; /* Even an ambiguous enable write must be shut down. */
            e = put_byte(0x20e, (uint8_t)((st.enable & ~enable_mask) | enable_mask));
        }
        if (!e) st.enable = (st.enable & ~enable_mask) | enable_mask;
        X9PMIFRelease();
    }
    if (e) { st.flags |= X9_TORCH_LATCHED; st.last_errno = e; (void)dark(); return e; }
    if (!lit_at) lit_at = now_ns(); /* Level changes NEVER extend continuous-on timeout. */
    st.current_ua = 25000 + code * 5000;
    st.level = st.current_ua * 1000 / st.max_current_ua;
    printf("X9 TORCH: requested=%u confirmed=%u current=%uuA enable=%02x\n", level, st.level, st.current_ua, st.enable);
    return 0;
}
static int torch_open(__unused dev_t dev, int flags, __unused int type, struct proc *p)
{
    int e = 0; lck_mtx_lock(&x9_torch_lock);
    if (flags & FWRITE) {
        char name[32]; proc_name(proc_pid(p), name, sizeof(name));
        if (st.mode != 3) e = EPERM;
        else if (kauth_cred_getuid(kauth_cred_get()) != 0 &&
                 (kauth_cred_getuid(kauth_cred_get()) != 501 || strcmp(name, "mediaserverd"))) e = EPERM;
        else if (writer) e = EBUSY;
        else writer = true;
    }
    lck_mtx_unlock(&x9_torch_lock); return e;
}
static int torch_close(__unused dev_t dev, int flags, __unused int type, __unused struct proc *p)
{
    lck_mtx_lock(&x9_torch_lock);
    int e = 0; if (flags & FWRITE) { e = dark(); writer = false; }
    lck_mtx_unlock(&x9_torch_lock); return e;
}
static int torch_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
    if (uio_resid(uio) != sizeof(uint32_t)) return EINVAL;
    uint32_t level = 0; int e = uiomove((char *)&level, sizeof(level), uio); if (e) return e;
    lck_mtx_lock(&x9_torch_lock); e = set_level(level); lck_mtx_unlock(&x9_torch_lock); return e;
}
static int torch_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
    if (uio_resid(uio) != sizeof(uint32_t)) return EINVAL;
    lck_mtx_lock(&x9_torch_lock); uint32_t level = st.level; lck_mtx_unlock(&x9_torch_lock);
    return uiomove((char *)&level, sizeof(level), uio);
}
static int torch_ioctl(__unused dev_t dev, u_long cmd, caddr_t data, __unused int flags, __unused struct proc *p)
{
    if (cmd != X9_TORCH_GET_STATUS) return ENOTTY;
    lck_mtx_lock(&x9_torch_lock); memcpy(data, &st, sizeof(st)); lck_mtx_unlock(&x9_torch_lock); return 0;
}
static const struct cdevsw torch_cdev = {
    .d_open = torch_open, .d_close = torch_close, .d_read = torch_read, .d_write = torch_write,
    .d_ioctl = torch_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_ttys = NULL,
    .d_select = eno_select, .d_mmap = eno_mmap, .d_strategy = eno_strat,
    .d_type = 0
};
static void torch_worker(__unused void *arg, __unused wait_result_t result)
{
    bool pulse_done = false; uint64_t start = now_ns();
    for (;;) {
        lck_mtx_lock(&x9_torch_lock);
        if (owned) {
            int temp, e = X9PMIFAcquire() ? EBUSY : 0;
            if (!e) { e = snapshot(); X9PMIFRelease(); }
            if (e || faulted() || (st.enable & 0x1f) != enable_mask ||
                X9BatteryTorchTemperature(&temp) || temp < 0 || temp >= 4500) {
                st.flags |= X9_TORCH_LATCHED; st.last_errno = e ? e : EIO;
                if (!(st.flags & X9_TORCH_OFF_FAILED))
                    printf("X9 TORCH: fault/stale/thermal cutoff fault=%06x errno=%u\n", st.fault, st.last_errno);
                (void)dark();
            } else if (now_ns() - lit_at >= (uint64_t)st.max_seconds * 1000000000) {
                st.flags |= X9_TORCH_NEEDS_ZERO; printf("X9 TORCH: continuous-on timeout\n"); (void)dark();
            }
        }
        if (st.mode == 2 && (st.flags & X9_TORCH_READY) && !pulse_done) {
            int temp;
            if (!X9BatteryTorchTemperature(&temp) && temp >= 0 && temp < 4500) {
                pulse_done = true; int e = set_level(1000);
                printf("X9 TORCH: 25mA/2s pulse result errno=%d\n", e);
            } else if (now_ns() - start > 15000000000ULL) {
                pulse_done = true; printf("X9 TORCH: pulse skipped: no safe fresh battery sample\n");
            }
        }
        lck_mtx_unlock(&x9_torch_lock);
        uint64_t deadline; clock_interval_to_deadline(100, NSEC_PER_MSEC, &deadline);
        assert_wait_deadline((event_t)&st, THREAD_UNINT, deadline); thread_block(THREAD_CONTINUE_NULL);
    }
}
void sweet_x9_torch_init(void)
{
    static bool attempted;
    if (!sweet_early_is_x9() || attempted) return; attempted = true;
    int mode = sweet_x9_arg("x9_torch", 0);
    if (!mode) return;
    if (mode < 1 || mode > 3) { printf("X9 TORCH: invalid mode, disabled\n"); return; }
    st.mode = mode; int channel = sweet_x9_arg("x9_torch_ch", 0);
    if (channel < 0 || channel > 2) return; st.channel = channel;
    int ma = sweet_x9_arg("x9_torch_ma", 100), secs = sweet_x9_arg("x9_torch_s", 15);
    if (ma < 25 || ma > 100 || secs < 1 || secs > 30) return;
    st.max_current_ua = mode == 2 ? 25000 : (unsigned)(ma / 5 * 5000);
    st.max_seconds = mode == 2 ? 2 : secs;
    enable_mask = 0x10 | (1u << (2 - channel));
    bus = ml_io_map(0x1c018000, 0x2000);
    if (!bus || !(rr(0) & 1) || X9PMIFAcquire()) return;
    uint8_t id = 0, current = 0; int e = get_byte(0, &id);
    if (!e) e = snapshot(); if (!e) e = get_byte(0x204 + channel * 3, &current);
    X9PMIFRelease(); st.device_info = id; st.last_errno = e;
    printf("X9 TORCH: mode=%u SID14 dev=%02x ch=%u itorch=%02x enable=%02x fault=%06x errno=%d\n",
        st.mode, id, st.channel, current, st.enable, st.fault, e);
    if (e || (id & 0xf0) != 0x70 || (mode > 1 && ((st.enable & 0x1f) || faulted()))) {
        printf("X9 TORCH: identity/inherited-state rejected; no PMIC writes\n"); return;
    }
    if (mode > 1) {
        thread_t t; kern_return_t kr = kernel_thread_start(torch_worker, NULL, &t);
        if (kr != KERN_SUCCESS) return; thread_deallocate(t);
    }
    int major = cdevsw_add(-1, &torch_cdev);
    if (major < 0) return;
    if (!devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL,
                        mode == 3 ? 0666 : 0444, "x9-torch")) {
        (void)cdevsw_remove(major, &torch_cdev); return;
    }
    lck_mtx_lock(&x9_torch_lock); st.flags |= X9_TORCH_READY; lck_mtx_unlock(&x9_torch_lock);
    printf("X9 TORCH: %s ready, max=%uuA/%us (mode1 read-only; mode3 starts dark)\n",
        X9_TORCH_DEVICE, st.max_current_ua, st.max_seconds);
}
