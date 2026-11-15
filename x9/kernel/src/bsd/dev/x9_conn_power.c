/* OPPO connv3_hw_pwr_on / mt6993_pmic.c / mt6993_pinctrl.c, co-clock=0.
 * No PMIC regmap transactions: MT6993 vsel is a no-op and its antenna/32K
 * callbacks are absent. All common GPIO transitions have one lifecycle lock.
 * An untracked, already-high PMIC is NOT silently adopted as Wi-Fi-ready. */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/locks.h>
#include <kern/clock.h>
#include <machine/machine_routines.h>
#include "x9_conn_power.h"
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
static LCK_GRP_DECLARE(conn_group, "x9.conn.power");
static LCK_MTX_DECLARE(conn_lock, &conn_group);
static volatile uint32_t *gpio, *tm1, *lm0;
static uint32_t radios, completed;
static bool shared_done;
static void write_reg(volatile uint32_t *p, unsigned off, uint32_t v)
{ p[off/4] = v; __asm__ volatile("dsb sy" ::: "memory"); }
static void field(volatile uint32_t *p, unsigned off, uint32_t mask, uint32_t v)
{ write_reg(p, off, (p[off/4] & ~mask) | (v & mask)); }
static void sleep_ms(unsigned ms)
{ delay_for_interval(ms, 1000 * NSEC_PER_USEC); }
static unsigned input(unsigned pin)
{ return (gpio[(0x200+(pin/32)*0x10)/4] >> (pin%32)) & 1; }
/* GPIO SET/CLR aliases avoid races with the private BT_RST/WF_RST bits in
 * the same mode/DO/DIR words; never write their entire saved words back. */
static void mode(unsigned pin, unsigned function)
{
    unsigned off=0x300+(pin/4)*0x10, shift=(pin%4)*8;
    write_reg(gpio, off+8, 0x1fu<<shift);
    if (function) write_reg(gpio, off+4, function<<shift);
}
static void output(unsigned pin, unsigned high)
{
    mode(pin, 0);
    write_reg(gpio, 0x100+(pin/32)*0x10+(high?4:8), 1u<<(pin%32));
    write_reg(gpio, (pin/32)*0x10+4, 1u<<(pin%32));
}
static void combo(unsigned function, bool pullup)
{
    const uint32_t mask=(1u<<26)|(1u<<25); /* GPIO243/244, IOCFG_TM1 */
    mode(243, function); mode(244, function);
    if (!function) {
        /* DT slew-rate=0 maps to DIR, not a guessed drive-strength field. */
        write_reg(gpio, (243/32)*0x10+8, (1u<<(243%32))|(1u<<(244%32)));
        field(tm1, 0xb0, mask, pullup?0:mask);
        field(tm1, 0xc0, mask, pullup?mask:0);
    }
}
static void faultb(bool pullup)
{
    mode(65, 0); write_reg(gpio, (65/32)*0x10+8, 1u<<(65%32));
    field(lm0, 0x50, 2, 2); field(lm0, 0x90, 2, pullup?0:2);
    field(lm0, 0xb0, 2, 0); field(lm0, 0xc0, 2, 2); /* R1R0=10 */
}
static int maps(void)
{
    if (!gpio) gpio=(volatile uint32_t *)ml_io_map(0x1002d000, 0x1000);
    if (!tm1) tm1=(volatile uint32_t *)ml_io_map(0x13820000, 0x1000);
    if (!lm0) lm0=(volatile uint32_t *)ml_io_map(0x13020000, 0x1000);
    return gpio && tm1 && lm0 ? 0 : ENOMEM;
}
static bool valid(enum x9_conn_radio r) { return r==X9_CONN_WIFI || r==X9_CONN_BT; }
static void common_off(void)
{
    /* mt6993 common_power_ctrl: wait FW off, faultb default, 20ms, EN low. */
    sleep_ms(210); faultb(false); sleep_ms(20); output(251, 0);
    combo(0, false);
    mode(246, 0); write_reg(gpio, (246/32)*0x10+8, 1u<<(246%32));
}
int x9_conn_power_acquire(enum x9_conn_radio r)
{
    if (!valid(r)) return EINVAL;
    lck_mtx_lock(&conn_lock);
    int error=maps();
    if (!error && (radios & r)) error=EALREADY;
    if (!error && radios) {
        /* Android curr_status!=0: no commonpower/pre-pin replay. */
        if (!input(251) || !input(65)) error=EIO;
    } else if (!error) {
        if (input(251)) error=EBUSY;
        else {
            sleep_ms(30); output(251, 1); sleep_ms(20); faultb(true);
            combo(0, true); output(246, 0); /* setup_pre + DFD release */
            sleep_ms(10);
            if (!input(251) || !input(65)) { error=EIO; common_off(); }
        }
    }
    if (!error) radios |= r;
    printf("X9 CONN: acquire radio=%u mask=%u errno=%d\n", r, radios, error);
    lck_mtx_unlock(&conn_lock); return error;
}
int x9_conn_power_done(enum x9_conn_radio r)
{
    if (!valid(r)) return EINVAL;
    lck_mtx_lock(&conn_lock);
    int error=!(radios&r)?ENXIO:(!input(251)||!input(65))?EIO:0;
    if (!error) {
        /* Don't temporarily put live combo UART pins back in GPIO mode
         * when a second radio reports power-done. */
        if (!shared_done) { combo(1, true); shared_done=true; }
        completed |= r;
    }
    lck_mtx_unlock(&conn_lock); return error;
}
int x9_conn_power_release(enum x9_conn_radio r)
{
    if (!valid(r)) return EINVAL;
    lck_mtx_lock(&conn_lock);
    int error=!(radios&r)?ENXIO:0;
    if (!error) {
        radios &= ~r; completed &= ~r;
        /* The first completed owner may leave while another owner is still
         * booting. Its departure does NOT undo shared pin configuration.
         * Replaying combo(1) would momentarily disconnect the live UART. */
        if (!radios) { common_off(); shared_done=false; }
    }
    printf("X9 CONN: release radio=%u mask=%u errno=%d\n", r, radios, error);
    lck_mtx_unlock(&conn_lock); return error;
}
uint32_t x9_conn_power_radios(void)
{ lck_mtx_lock(&conn_lock); uint32_t r=radios; lck_mtx_unlock(&conn_lock); return r; }
int x9_conn_bt_pins(enum x9_conn_bt_pin_state s)
{
    if (s<X9_CONN_BT_PRE || s>X9_CONN_BT_RESET_OFF) return EINVAL;
    lck_mtx_lock(&conn_lock);
    int error=maps();
    if (!error) switch (s) {
    case X9_CONN_BT_PRE:
        mode(241, 0); mode(242, 0);
        write_reg(gpio, (241/32)*0x10+8, (1u<<(241%32))|(1u<<(242%32)));
        field(tm1, 0x80, 0x60, 0x60); /* input enable, not guessed pull */
        field(tm1, 0xb0, 0x60, 0); field(tm1, 0xc0, 0x60, 0x60);
        break;
    case X9_CONN_BT_TX: mode(241, 1); output(249, 1); break;
    case X9_CONN_BT_RX: mode(242, 1); break;
    case X9_CONN_BT_RESET_OFF: output(249, 0); break;
    }
    lck_mtx_unlock(&conn_lock); return error;
}
#endif
