/* linux_v2 uart/btmtktty.c: btmtk_sp_pre_open, with bounded, strict ACKs.
 * No default chip ID, uninitialized XO padding, ignored connfem error,
 * firmware assert, ambiguous TX retry or fabricated native-ready state. */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include "x9_bt_session.h"
#include "../../../generated/x9_bt_board.inc"
#include "../../../generated/x9_bt_vendor.inc"

static uint32_t le32(const uint8_t *p)
{ return p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }

/* All inputs/packets must be valid before pre_on acquires SSPM/resources or
 * changes pins. Empty passive-build stubs cannot become a radio-ready path.
 * No partial prefix is applied if any later packet is malformed. */
static int controller_preflight(void)
{
    if (!X9_BT_VENDOR_COMMANDS) return ENOENT;
    if (X9_BT_VENDOR_COMMANDS > 64 || X9_BT_VENDOR_COMMANDS !=
        sizeof(x9_bt_vendor_packets)/sizeof(x9_bt_vendor_packets[0])) return EINVAL;
    for (unsigned i=0;i<X9_BT_VENDOR_COMMANDS;i++) {
        const struct x9_bt_vendor_packet *p=&x9_bt_vendor_packets[i];
        if (p->length<4 || p->length>sizeof(p->bytes) || p->bytes[0]!=1 ||
            p->length!=(unsigned)p->bytes[3]+4) return EPROTO;
        uint16_t opcode=p->bytes[1]|((uint16_t)p->bytes[2]<<8);
        if ((opcode>>10)!=0x3f || opcode==0xfc6f) return EPROTO;
    }
    /* Exact selected generator path must retain preferred A2DP append. */
    static const uint8_t append[]={1,0x37,0xfc,3,0x0d,0,1};
    const struct x9_bt_vendor_packet *last=&x9_bt_vendor_packets[
        sizeof(x9_bt_vendor_packets)/sizeof(x9_bt_vendor_packets[0])-1];
    if (last->constructor || last->length!=sizeof(append) ||
        memcmp(last->bytes,append,sizeof(append))) return EPROTO;
    return 0;
}

static int controller_config(const struct x9_bt_session_ops *o,
                             struct x9_bt_session_result *r)
{
    for (unsigned i=0;i<X9_BT_VENDOR_COMMANDS;i++) {
        const struct x9_bt_vendor_packet *p=&x9_bt_vendor_packets[i];
        r->vendor_constructor=p->constructor;
        r->vendor_opcode=p->bytes[1]|((uint16_t)p->bytes[2]<<8);
        r->vendor_status=0xff;
        int error=x9_bt_session_vendor_command(o,p->bytes,p->length,&r->vendor_status);
        if (error) {
            printf("X9 BT: vendor step=%u constructor=%x opcode=%04x status=%02x errno=%d\n",
                   i,p->constructor,r->vendor_opcode,r->vendor_status,error);
            return error; /* No ambiguous TX/reply retries or failed-step skips. */
        }
        r->vendor_completed++;
    }
    printf("X9 BT: Android vendor configuration completed %u commands\n",r->vendor_completed);
    return 0;
}

/* Full H4 WMT envelope; the transport must select the matching WMT opcode
 * and preserve unrelated HCI packets. Status/length are NOT prefix matches. */
static int command(const struct x9_bt_download_ops *o, const uint8_t *tx,
                   size_t txlen, uint8_t opcode, uint8_t *rx, size_t *rxlen)
{
    int error = o->exchange(o->context, tx, txlen, rx, rxlen, 1000);
    if (error) return error;
    if (*rxlen < 8 || rx[0] != 4 || rx[1] != 0xe4 ||
        (size_t)rx[2]+3 != *rxlen || rx[3] != 2 || rx[4] != opcode ||
        ((size_t)rx[5] | ((size_t)rx[6]<<8))+7 != *rxlen) return EPROTO;
    return rx[7] ? EIO : 0;
}

static int exact(const struct x9_bt_download_ops *o, const uint8_t *tx, size_t txlen,
                 const uint8_t *expected, size_t expected_len)
{
    uint8_t rx[260]; size_t n = sizeof(rx);
    int error = command(o, tx, txlen, expected[4], rx, &n);
    if (error) return error;
    return n == expected_len && !memcmp(rx, expected, n) ? 0 : EPROTO;
}

int x9_bt_session_connfem(const struct x9_bt_download_ops *o)
{
    static const uint8_t ack[] = {4,0xe4,6,2,0x55,2,0,0,1};
    if (!o || !o->exchange) return EINVAL;
    if (!X9_BT_CONNFEM_BYTES) return ENOENT;
    return exact(o, x9_bt_connfem_command, X9_BT_CONNFEM_BYTES, ack, sizeof(ack));
}

int x9_bt_session_vendor_command(const struct x9_bt_session_ops *o,
                                 const uint8_t *tx, size_t txlen, uint8_t *status)
{
    if (!status) return EINVAL;
    *status = 0xff;
    if (!o || !o->hci_exchange || !tx || txlen < 4 || tx[0] != 1 ||
        txlen != (size_t)tx[3]+4) return EINVAL;
    uint16_t opcode = tx[1] | ((uint16_t)tx[2]<<8);
    /* The production GORM sequence/cfg appends are vendor commands. WMT
     * FC6F belongs to command()/exchange() and must not steal HCI credits. */
    if ((opcode & 0xfc00) != 0xfc00 || opcode == 0xfc6f) return EINVAL;
    uint8_t rx[260]; size_t n = sizeof(rx);
    int error = o->hci_exchange(o->io.context, tx, txlen, rx, &n, 5000);
    if (error) return error;
    if (n < 3 || rx[0] != 4 || (size_t)rx[2]+3 != n) return EPROTO;
    if (rx[1] == 0x0e) {
        if (n < 7 || rx[4] != tx[1] || rx[5] != tx[2]) return EPROTO;
        *status = rx[6];
        return *status ? EIO : 0;
    }
    if (rx[1] == 0x0f) {
        if (n != 7 || rx[5] != tx[1] || rx[6] != tx[2]) return EPROTO;
        *status = rx[3];
        /* Original GORMevt_HCE_Common_Complete (28a90..28b18) accepts a
         * matching Command Status without testing its status byte. Do not
         * reproduce that false-success path; accepted is not completed. */
        return *status ? EIO : EPROTO;
    }
    return EPROTO;
}

static int query(const struct x9_bt_download_ops *o, uint8_t id,
                 uint8_t *rx, size_t *n)
{
    const uint8_t tx[] = {1,0x6f,0xfc,5,1,4,1,0,id};
    int error = command(o, tx, sizeof(tx), 4, rx, n);
    if (error) return error;
    if (*n != (id == 0x22 ? 14u : 13u) || rx[8] != id) return EPROTO;
    return 0;
}

static int set_uart(const struct x9_bt_download_ops *o, uint8_t fc, uint8_t cfg)
{
    const uint8_t tx[] = {1,0x6f,0xfc,0x0b,1,4,7,0,1,0,0x36,0x6e,1,fc,cfg};
    static const uint8_t ack[] = {4,0xe4,6,2,4,2,0,0,1};
    return exact(o, tx, sizeof(tx), ack, sizeof(ack));
}

static int wake(const struct x9_bt_download_ops *o)
{
    /* btmtk_uart_send_wakeup_cmd is converted by UART send path to raw ff.
     * MT6653 uses event2 even before patch download (the MT6639 exception
     * must not be applied to this board). */
    static const uint8_t tx[] = {0xff};
    static const uint8_t ack[] = {4,0xe4,7,2,3,3,0,0,3,1};
    return exact(o, tx, sizeof(tx), ack, sizeof(ack));
}

int x9_bt_session_fw_own(const struct x9_bt_session_ops *o,
                         struct x9_bt_session_result *r)
{
    if (!o || !o->io.exchange || !r) return EINVAL;
    if (r->stage!=X9_BT_SESSION_DONE || !r->function_powered || !r->driver_owned)
        return ENXIO;
    if (!r->hardware_sleep) return ENOTSUP;
    /* Hardware-mechanism path keeps byte9=1. The other-host wake bitmap is
     * only part of Android's SOFTWARE-mechanism path; do not insert it here.
     * Firmware may reply with its no-sleep status or a driver-own event. */
    static const uint8_t tx[]={1,0x6f,0xfc,6,1,3,2,0,1,1};
    static const uint8_t prefix[]={4,0xe4,7,2,3,3,0};
    uint8_t rx[260]; size_t n=sizeof(rx);
    r->driver_owned=false; /* TX/timeout/parser failure is unknown, not READY. */
    int error=o->io.exchange(o->io.context,tx,sizeof(tx),rx,&n,1000);
    if (error) return error;
    if (n!=10 || memcmp(rx,prefix,sizeof(prefix)) || rx[9]!=1 ||
        (rx[8]!=1 && rx[8]!=3)) return EPROTO;
    if (rx[7] || rx[8]==3) {
        r->driver_owned=true;
        return EBUSY;
    }
    return 0;
}

int x9_bt_session_driver_own(const struct x9_bt_session_ops *o,
                             struct x9_bt_session_result *r)
{
    if (!o || !o->io.exchange || !r) return EINVAL;
    if (r->stage!=X9_BT_SESSION_DONE || !r->function_powered) return ENXIO;
    if (!r->hardware_sleep) return ENOTSUP;
    if (r->driver_owned) return EALREADY;
    static const uint8_t tx[]={1,0x6f,0xfc,6,1,3,2,0,3,1};
    static const uint8_t ack[]={4,0xe4,7,2,3,3,0,0,3,1};
    int error=exact(&o->io,tx,sizeof(tx),ack,sizeof(ack));
    if (!error) r->driver_owned=true;
    /* No Android failure path that sets DRV_OWN for an assert/debug SOP. */
    return error;
}

static int function_on(const struct x9_bt_download_ops *o)
{
    /* chip/btmtk_chip_common.c WMT_POWER_ON_CMD/EVT. Generic command()
     * treats byte7 as failure, but THIS opcode explicitly defines 2=busy.
     * Retry ONLY that complete reply, never a timeout/partial TX/parse error.
     * btmtk_send_wmt_power_on_cmd uses 50ms busy backoff. Unlike its loop,
     * exhaustion is an error, not success with power_state still unset. */
    static const uint8_t tx[] = {1,0x6f,0xfc,6,1,6,2,0,0,1};
    static const uint8_t prefix[] = {4,0xe4,5,2,6,1,0};
    uint64_t deadline = o->now_ms(o->context) + 1000;
    for (unsigned attempt = 0; attempt < 20; attempt++) {
        uint8_t rx[260]; size_t n = sizeof(rx);
        uint64_t now = o->now_ms(o->context);
        if (now >= deadline) return EBUSY;
        int error = o->exchange(o->context, tx, sizeof(tx), rx, &n,
                               (uint32_t)(deadline - now));
        if (error) return error;
        if (n != 8 || memcmp(rx, prefix, sizeof(prefix))) return EPROTO;
        if (!rx[7]) return 0;
        if (rx[7] != 2) return EIO;
        o->sleep_ms(o->context, 50);
    }
    return EBUSY;
}

int x9_bt_session_run(const struct x9_bt_session_ops *o,
                     const struct x9_bt_session_board *b, struct x9_bt_session_result *r)
{
    if (!r) return EINVAL;
    bzero(r, sizeof(*r));
    r->vendor_status=0xff;
    if (!o || !b || !o->io.exchange || !o->io.raw_write || !o->io.drain ||
        !o->io.sleep_ms || !o->io.now_ms || !o->io.connfem ||
        !o->pre_on || !o->power_on || !o->rx_and_32k || !o->host_uart ||
        !o->hub_features || !o->post_download || !o->verify_ownership ||
        !o->hci_exchange) return EINVAL;
    if (!b->calibration_present) return ENOENT;
    if (!X9_BT_CONNFEM_BYTES) return ENOENT;
    if (b->flow_control != 0 && b->flow_control != 0x40 && b->flow_control != 0x80) return EINVAL;
    int error = x9_bt_download_preflight_builtin();
    if (error) return error;
    error=controller_preflight();
    if (error) return error;
    void *ctx = o->io.context;
    bool hw_sleep = false, inband = false;
    uint8_t rx[260]; size_t n;
#define STEP(stage_value, operation) do { r->stage = (stage_value); \
    error = (operation); if (error) return error; } while (0)
    STEP(X9_BT_SESSION_PRE_ON, o->pre_on(ctx));
    STEP(X9_BT_SESSION_POWER, o->power_on(ctx));
    /* Android waits 10 x 10ms for reset to settle before RX aux / 32K. */
    o->io.sleep_ms(ctx, 100);
    STEP(X9_BT_SESSION_32K, o->rx_and_32k(ctx));
    STEP(X9_BT_SESSION_ROM, o->host_uart(ctx, 115200, 0));
    r->baud = 115200;
    STEP(X9_BT_SESSION_ROM, o->hub_features(ctx, &hw_sleep, &inband));
    r->hardware_sleep=hw_sleep;
    n = sizeof(rx);
    STEP(X9_BT_SESSION_ROM, query(&o->io, 2, rx, &n));
    if (b->flow_control == 0x40) {
        static const uint8_t tx[] = {
            1,0x6f,0xfc,0x2c,1,8,0x28,0,1,1,0,3,0x10,0,9,0x80,
            2,0,0,0,0xff,0xff,0xff,0xff,0x34,0x53,0,0x70,0,0,2,0,
            0xff,0xff,0xff,0xff,0x44,0x53,0,0x70,0,0,4,0,0xff,0xff,0xff,0xff};
        static const uint8_t ack[] = {4,0xe4,8,2,8,4,0,0,0,0,3};
        STEP(X9_BT_SESSION_ROM, exact(&o->io, tx, sizeof(tx), ack, sizeof(ack)));
    }
    n = sizeof(rx);
    STEP(X9_BT_SESSION_ROM, query(&o->io, 9, rx, &n));
    r->chip_id = le32(rx+9);
    if (r->chip_id != 0x6653) return ENODEV;
    r->ap_flags = 3 | (hw_sleep ? 0 : 0x20);
    STEP(X9_BT_SESSION_BAUD, set_uart(&o->io, b->flow_control, r->ap_flags));
    STEP(X9_BT_SESSION_BAUD, o->io.drain(ctx, 1000));
    STEP(X9_BT_SESSION_BAUD, o->host_uart(ctx, 24000000, b->flow_control));
    r->baud = 24000000;
    STEP(X9_BT_SESSION_BAUD, wake(&o->io));
    /* All 64 data bytes initialized. Vendor code only initialized 29;
     * bytes25..59 are reserved, not a source of calibration data. */
    uint8_t xo[75] = {1,0x6f,0xfc,0x47,1,0x19,0x43,0,1,0,0};
    memcpy(xo+11, b->xo, sizeof(b->xo));
    for (unsigned k = 0; k < 4; k++) xo[71+k] = (uint8_t)(b->platform_config >> (8*k));
    n = sizeof(rx);
    STEP(X9_BT_SESSION_XO, command(&o->io, xo, sizeof(xo), 0x19, rx, &n));
    STEP(X9_BT_SESSION_DOWNLOAD, x9_bt_download_builtin(&o->io, &r->download));
    n = sizeof(rx);
    STEP(X9_BT_SESSION_FEATURES, query(&o->io, 0x22, rx, &n));
    r->max_baud = le32(rx+9); r->firmware_flags = rx[13];
    if (r->max_baud < r->baud || !(r->firmware_flags & 0x10)) return ENOTSUP;
    /* No wakeup-enhance without an actual registered board wake callback. */
    r->ap_flags = 0x10 | (hw_sleep ? 0 : 0x20) | (r->firmware_flags & 0x40);
    inband = inband && (r->firmware_flags & 4);
    if (inband) r->ap_flags |= 4 | (r->firmware_flags & 0x80);
    if (b->wakeup_callback) r->ap_flags |= r->firmware_flags & 8;
    STEP(X9_BT_SESSION_ROUTING, set_uart(&o->io, b->flow_control, r->ap_flags));
    STEP(X9_BT_SESSION_ROUTING, o->io.drain(ctx, 1000));
    STEP(X9_BT_SESSION_ROUTING, o->post_download(ctx, inband));
    /* btmtk_uart_load_rom_patch after bypass(0): wakeup 0xFF; on failure
     * ("fw get dirty data trigger EINT") btmtk_uart_driver_own_cmd. */
    error = wake(&o->io);
    if (error) {
        static const uint8_t own[] = {1,0x6f,0xfc,6,1,3,2,0,3,1};
        static const uint8_t own_ack[] = {4,0xe4,7,2,3,3,0,0,3,1};
        int first = error;
        r->stage = X9_BT_SESSION_OWN;
        error = exact(&o->io, own, sizeof(own), own_ack, sizeof(own_ack));
        printf("X9 BT: post-download wake errno=%d; driver-own fallback errno=%d (as Android)\n",
               first, error);
        if (error) return error;
    }
    bool bt_awake = false;
    STEP(X9_BT_SESSION_OWN, o->verify_ownership(ctx, &bt_awake));
    if (!bt_awake) {
        /* open_done step 28: 0xFF with no expected event, one 500 ms try.
         * Consume a late wake event so it cannot reach the next command. */
        static const uint8_t ff[] = {0xff};
        uint8_t wrx[260]; size_t wn = sizeof(wrx);
        int werr = o->io.exchange(ctx, ff, sizeof(ff), wrx, &wn, 500);
        printf("X9 BT: DEV0_BT_AWAKE=0, sent 0xFF (no event required) reply errno=%d len=%u\n",
               werr, (unsigned)(werr ? 0 : wn));
    }
    r->driver_owned = true;
    /* Both UART tty/serdev calibration callbacks are real Android no-ops;
     * XO/connfem have already been sent. Ownership is NOT function power-on.
     * Normal btmtk_fops_open calls btmtk_send_init_cmds before OPENED, except
     * DVT builds. Never apply that DVT skip to this production session. */
    STEP(X9_BT_SESSION_FUNCTION, function_on(&o->io));
    r->function_powered = true;
    STEP(X9_BT_SESSION_CONTROLLER_CONFIG, controller_config(o,r));
    r->stage = X9_BT_SESSION_DONE;
#undef STEP
    return 0;
}
#endif
