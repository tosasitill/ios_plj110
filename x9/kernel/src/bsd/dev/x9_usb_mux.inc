/* x9_usb_mux.inc - kernel glue for the usbmux engine (x9_usb=3).
 *
 * Included once by x9_usb_device.c after complete()/kick_tx(), so it can
 * use the endpoint machinery directly. Everything here runs on the USB
 * worker thread only, exactly like the ECM/raw data path: the engine is
 * fed from complete(), pumped from data_poll(), and reset from
 * reset_data(). Local services are reached over loopback TCP sockets
 * (lockdownd listens on 62078; StartService ports are TCP listeners too).
 */
#include "x9_usb_mux.h"

static struct x9_mux_engine mux_engine;
static unsigned mux_engine_ready;
static socket_t mux_socks[X9_MUX_CONNS];
static unsigned mux_rx_logged, mux_open_logged;
static uint8_t mux_scratch[4096];

#define X9_MUX_TXQ 4
static struct { uint8_t bytes[4096]; uint32_t n; } mux_txq[X9_MUX_TXQ];
static unsigned mux_txq_head, mux_txq_count;

static uint64_t mux_now_ms(void)
{
    uint64_t ns;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000000;
}

static int mux_glue_tx(void *ctx, const uint8_t *bytes, uint32_t n)
{
    (void)ctx;
    if (n > 4096 || mux_txq_count == X9_MUX_TXQ)
        return 0;
    unsigned slot = (mux_txq_head + mux_txq_count) % X9_MUX_TXQ;
    memcpy(mux_txq[slot].bytes, bytes, n);
    mux_txq[slot].n = n;
    mux_txq_count++;
    return (int)n;
}

static int mux_glue_open(void *ctx, uint16_t dport)
{
    (void)ctx;
    int slot = -1;
    for (unsigned i = 0; i < X9_MUX_CONNS; i++)
        if (!mux_socks[i]) { slot = (int)i; break; }
    if (slot < 0)
        return -1;
    socket_t so = NULL;
    if (sock_socket(PF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, NULL, &so) || !so)
        return -1;
    struct timeval tv = {0, 200000};
    sock_setsockopt(so, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    sock_setsockopt(so, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in sin = {sizeof(sin), AF_INET, htons(dport), {0},
        {0, 0, 0, 0, 0, 0, 0, 0}};
    sin.sin_addr.s_addr = htonl(0x7f000001u);
    if (sock_connect(so, (struct sockaddr *)&sin, 0)) {
        sock_close(so);
        return -1;
    }
    mux_socks[slot] = so;
    if (!mux_open_logged) {
        mux_open_logged = 1;
        printf("X9 USB MUX: first loopback service connection (port %u)\n",
            dport);
    }
    return slot;
}

static int mux_glue_send(void *ctx, int handle, const uint8_t *bytes,
    uint32_t n)
{
    (void)ctx;
    if (handle < 0 || handle >= X9_MUX_CONNS || !mux_socks[handle])
        return -1;
    struct iovec iov;
    iov.iov_base = (void *)(uintptr_t)bytes;
    iov.iov_len = n;
    struct msghdr msg;
    bzero(&msg, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    size_t nsent = 0;
    errno_t err = sock_send(mux_socks[handle], &msg, MSG_DONTWAIT, &nsent);
    if (err == EWOULDBLOCK || err == EAGAIN)
        return 0;
    if (err)
        return -1;
    return (int)nsent;
}

static void mux_glue_close(void *ctx, int handle)
{
    (void)ctx;
    if (handle >= 0 && handle < X9_MUX_CONNS && mux_socks[handle]) {
        sock_close(mux_socks[handle]);
        mux_socks[handle] = NULL;
    }
}

static const struct x9_mux_ops mux_ops = {
    mux_glue_tx, mux_glue_open, mux_glue_send, mux_glue_close
};

static void x9_mux_glue_ensure(void)
{
    if (!mux_engine_ready) {
        x9_mux_init(&mux_engine, &mux_ops, NULL,
            (uint32_t)(mach_absolute_time() >> 7) | 0x1000u);
        mux_engine_ready = 1;
    }
}

/* EP1 OUT completion data (called from complete()). */
static void x9_mux_glue_rx(const uint8_t *bytes, unsigned n)
{
    if (!mux_rx_logged) {
        mux_rx_logged = 1;
        printf("X9 USB MUX: first host mux traffic (%u bytes); engine live\n",
            n);
    }
    x9_mux_glue_ensure();
    x9_mux_feed(&mux_engine, bytes, n);
}

static void x9_mux_glue_reset(void)
{
    if (mux_engine_ready) {
        if (mux_engine.stat_syn)
            printf("X9 USB MUX: reset syn=%u refused=%u rst_in=%u rst_out=%u "
                "bytes_in=%llu bytes_out=%llu framing=%u bad_magic=%u\n",
                mux_engine.stat_syn, mux_engine.stat_refused,
                mux_engine.stat_rst_in, mux_engine.stat_rst_out,
                mux_engine.stat_bytes_in, mux_engine.stat_bytes_out,
                mux_engine.stat_framing, mux_engine.stat_bad_magic);
        x9_mux_reset(&mux_engine);
    }
    mux_txq_head = mux_txq_count = 0;
}

static unsigned x9_mux_glue_hot(void)
{
    if (!mux_engine_ready)
        return 0;
    return mux_txq_count || x9_mux_active_conns(&mux_engine);
}

/* Pull bytes out of local service sockets into the engine. */
static void mux_service_sockets(void)
{
    for (unsigned i = 0; i < X9_MUX_CONNS; i++) {
        struct x9_mux_conn *c = &mux_engine.conns[i];
        if (!c->used || c->handle < 0 || !mux_socks[c->handle])
            continue;
        uint32_t room = x9_mux_tx_room(&mux_engine, c->handle);
        if (!room)
            continue;
        uint32_t want = room < sizeof(mux_scratch) ? room
            : (uint32_t)sizeof(mux_scratch);
        struct iovec iov;
        iov.iov_base = mux_scratch;
        iov.iov_len = want;
        struct msghdr msg;
        bzero(&msg, sizeof(msg));
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        size_t got = 0;
        errno_t err = sock_receive(mux_socks[c->handle], &msg,
            MSG_DONTWAIT, &got);
        if (!err && got)
            x9_mux_local_data(&mux_engine, c->handle, mux_scratch,
                (uint32_t)got);
        else if (!err)
            x9_mux_local_eof(&mux_engine, c->handle);
        else if (err != EWOULDBLOCK && err != EAGAIN)
            x9_mux_local_error(&mux_engine, c->handle);
    }
}

static void mux_data_poll(void)
{
    uint32_t txerr = R(0x8c), rxerr = R(0x88);
    if ((txerr & 0xffff0000) || (rxerr & 0xfffffeee)) {
        fault(EIO, txerr ? txerr : rxerr);
        return;
    }
    complete(&eps[0]);
    complete(&eps[1]);
    if (st.state != X9_USB_CONFIGURED || failed)
        return;
    if (!eps[0].busy)
        submit(&eps[0], NULL, 4096, 0, 0, 0);
    x9_mux_glue_ensure();
    mux_service_sockets();
    x9_mux_pump(&mux_engine, mux_now_ms());
    if (!eps[1].busy && mux_txq_count) {
        unsigned slot = mux_txq_head;
        mux_txq_head = (mux_txq_head + 1) % X9_MUX_TXQ;
        mux_txq_count--;
        submit(&eps[1], mux_txq[slot].bytes, mux_txq[slot].n, 0, 0, 0);
    }
}
