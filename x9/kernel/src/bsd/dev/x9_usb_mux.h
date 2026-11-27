/* x9_usb_mux.h - device-side Apple USB multiplexor (usbmux) protocol engine.
 *
 * Pure protocol logic: no kernel, socket or USB dependencies. The kernel
 * glue (x9_usb_mux.inc) feeds it USB bulk-OUT bytes, transmits the packets
 * it emits as single USB bulk-IN transfers, and bridges each accepted
 * connection to a loopback TCP service (lockdownd:62078 and the ports it
 * hands out via StartService).
 *
 * Wire behaviour is implemented from the libimobiledevice usbmuxd sources
 * (device.c / usb.c), recovered 2026-11-27, which define the de-facto
 * protocol every host implementation (macOS usbmuxd, libusbmuxd, i4Tools)
 * speaks with a stock iPhone:
 *  - Packet framing: {protocol,length} big-endian; at protocol version 2
 *    the header grows {magic=0xfeedface, tx_seq, rx_seq}. The host sends
 *    VERSION first (8-byte header), the device replies VERSION with the
 *    8-byte header; for major 2 the host then sends SETUP (protocol 2,
 *    payload 0x07) and all later packets use 16-byte headers.
 *  - The host matches VID 0x05AC and an interface of class 0xFF /
 *    subclass 0xFE / protocol 2 with exactly two bulk endpoints.
 *  - Inside, a mini-TCP: the host initiates every connection with SYN
 *    (seq 0). The device must answer exactly SYN|ACK, or RST to refuse.
 *    Data packets carry the ACK flag only; close is RST. The host ACKs
 *    bytes only once its client has consumed them and never retransmits
 *    payload (USB bulk is the reliable layer), so this engine sends each
 *    byte exactly once, in order, paced by the host's window and ACKs.
 *  - Every device->host mux packet must occupy exactly one USB transfer
 *    (short-packet terminated), so packets are capped at one 4096-byte
 *    GPD: 16 (mux) + 20 (TCP) + 4060 payload.
 */
#ifndef X9_USB_MUX_H
#define X9_USB_MUX_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define X9_MUX_MAX_PACKET   49168u  /* host USB_MTU is 49152; +16 slack */
#define X9_MUX_CONNS        4
#define X9_MUX_RX_WINDOW    32768u  /* bytes; wire th_win is this >> 8 */
#define X9_MUX_TXBUF        16384u
#define X9_MUX_SEG_PAYLOAD  4060u   /* 4096 - 16 mux - 20 TCP */
#define X9_MUX_STALL_MS     30000u  /* unacked-data stall before RST */

#define X9_MUX_PROTO_VERSION 0u
#define X9_MUX_PROTO_CONTROL 1u
#define X9_MUX_PROTO_SETUP   2u
#define X9_MUX_PROTO_TCP     6u

#define X9_MUX_TH_FIN 0x01u
#define X9_MUX_TH_SYN 0x02u
#define X9_MUX_TH_RST 0x04u
#define X9_MUX_TH_ACK 0x10u

struct x9_mux_ops {
    /* Transmit one complete mux packet; returns n when queued, <=0 when
     * the transport cannot take it now (engine retries control packets
     * from its pending slot and data from its TX buffer). */
    int (*tx_packet)(void *ctx, const uint8_t *bytes, uint32_t n);
    /* Open a loopback connection to a service port; >=0 handle, <0 refuse. */
    int (*local_open)(void *ctx, uint16_t dport);
    /* Push received bytes into the local connection; returns bytes
     * consumed (0 = would block, <0 = local error). */
    int (*local_send)(void *ctx, int handle, const uint8_t *bytes, uint32_t n);
    void (*local_close)(void *ctx, int handle);
};

struct x9_mux_conn {
    uint8_t used;
    uint8_t established;
    uint8_t peer_closed;   /* host sent FIN (defensive; protocol uses RST) */
    uint8_t local_eof;     /* local service closed its side */
    int handle;
    uint16_t sport;        /* host ephemeral port */
    uint16_t dport;        /* service port */
    uint32_t isn;          /* our initial sequence number */
    uint32_t rx_base;      /* host_isn + 1: seq of first data byte */
    uint32_t rx_expect;    /* next host seq accepted into pend */
    uint32_t tx_next;      /* next seq we assign (includes SYN's 1) */
    uint32_t tx_ackd;      /* latest cumulative ACK from the host */
    uint32_t host_win;     /* host's latest advertised window, bytes */
    uint32_t last_ack_sent;
    uint64_t last_progress_ms;
    uint8_t pend[X9_MUX_RX_WINDOW];  /* host->local, not yet consumed */
    uint32_t pend_head, pend_len;
    uint8_t txbuf[X9_MUX_TXBUF];     /* local->host, not yet packetized */
    uint32_t tx_head, tx_len;
};

struct x9_mux_engine {
    struct x9_mux_ops ops;
    void *ctx;
    uint8_t state;         /* 0 wait-version, 1 wait-setup, 2 active */
    uint8_t version;       /* negotiated major: 1 or 2 */
    uint16_t tx_seq;
    uint16_t last_rx_seq;
    uint32_t isn_counter;
    uint64_t now_ms;
    uint8_t rxbuf[X9_MUX_MAX_PACKET + 16];
    uint32_t rx_len;
    uint8_t pend_pkt[64];  /* one unsent control packet awaiting transport */
    uint32_t pend_pkt_len;
    struct x9_mux_conn conns[X9_MUX_CONNS];
    uint32_t stat_version, stat_setup, stat_bad_magic, stat_framing;
    uint32_t stat_control, stat_syn, stat_refused, stat_rst_in, stat_rst_out;
    uint64_t stat_bytes_in, stat_bytes_out;
};

static inline uint16_t x9_mux_rd16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static inline uint32_t x9_mux_rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
        ((uint32_t)p[2] << 8) | p[3];
}
static inline void x9_mux_wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}
static inline void x9_mux_wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static inline uint32_t x9_mux_hdr_size(const struct x9_mux_engine *e)
{
    return e->version == 2 ? 16u : 8u;
}
static inline uint32_t x9_mux_ack_value(const struct x9_mux_conn *c)
{
    /* Bytes the local service has consumed = received minus pending. */
    return c->rx_base + (c->rx_expect - c->rx_base) - c->pend_len;
}

static inline void x9_mux_init(struct x9_mux_engine *e,
    const struct x9_mux_ops *ops, void *ctx, uint32_t isn_seed)
{
    memset(e, 0, sizeof(*e));
    e->ops = *ops;
    e->ctx = ctx;
    e->isn_counter = isn_seed & 0x7fffffff;
}

static inline void x9_mux_conn_free(struct x9_mux_engine *e,
    struct x9_mux_conn *c)
{
    if (c->used && c->handle >= 0)
        e->ops.local_close(e->ctx, c->handle);
    memset(c, 0, sizeof(*c));
    c->handle = -1;
}

/* Drop all protocol state (USB bus reset / cable disconnect). Local
 * connections are closed through the ops callback. */
static inline void x9_mux_reset(struct x9_mux_engine *e)
{
    for (unsigned i = 0; i < X9_MUX_CONNS; i++)
        if (e->conns[i].used)
            x9_mux_conn_free(e, &e->conns[i]);
    e->state = 0;
    e->version = 0;
    e->tx_seq = 0;
    e->last_rx_seq = 0;
    e->rx_len = 0;
    e->pend_pkt_len = 0;
}

/* Build one mux packet into out (caller provides 4096 bytes) and return
 * its total length. Stamps sequence fields at protocol version 2. */
static inline uint32_t x9_mux_build(struct x9_mux_engine *e, uint32_t proto,
    const uint8_t *payload, uint32_t n, uint8_t *out)
{
    uint32_t hdr = x9_mux_hdr_size(e);
    x9_mux_wr32(out + 0, proto);
    x9_mux_wr32(out + 4, hdr + n);
    if (hdr == 16) {
        x9_mux_wr32(out + 8, 0xfeedfaceu);
        x9_mux_wr16(out + 12, e->tx_seq++);
        x9_mux_wr16(out + 14, e->last_rx_seq);
    }
    if (n)
        memcpy(out + hdr, payload, n);
    return hdr + n;
}

/* Send a small control packet (version reply, SYN/ACK, ACK, RST). If the
 * transport is busy the packet is stashed and retried by x9_mux_pump();
 * control packets are <= 64 bytes by construction. */
static inline void x9_mux_send_ctrl(struct x9_mux_engine *e, uint32_t proto,
    const uint8_t *payload, uint32_t n)
{
    uint8_t pkt[64];
    uint32_t total = x9_mux_build(e, proto, payload, n, pkt);
    if (total > sizeof(pkt))
        return;
    if (e->pend_pkt_len) {
        if (e->ops.tx_packet(e->ctx, e->pend_pkt, e->pend_pkt_len) > 0)
            e->pend_pkt_len = 0;
        else
            return; /* keep the older packet; drop this one (ACKs coalesce) */
    }
    if (e->ops.tx_packet(e->ctx, pkt, (int)total) <= 0) {
        memcpy(e->pend_pkt, pkt, total);
        e->pend_pkt_len = total;
    }
}

/* Emit one payload-free TCP packet (SYN/ACK, ACK, RST). Data packets are
 * built by x9_mux_pump(), which must see transport refusals so it can
 * retry from the TX buffer. */
static inline void x9_mux_send_tcp(struct x9_mux_engine *e,
    uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack,
    uint8_t flags, uint32_t window, const uint8_t *payload, uint32_t n)
{
    uint8_t tcp[20];
    (void)payload;
    (void)n;
    x9_mux_wr16(tcp + 0, sport);
    x9_mux_wr16(tcp + 2, dport);
    x9_mux_wr32(tcp + 4, seq);
    x9_mux_wr32(tcp + 8, ack);
    tcp[12] = 5 << 4;
    tcp[13] = flags;
    x9_mux_wr16(tcp + 14, (uint16_t)(window >> 8));
    x9_mux_wr16(tcp + 16, 0); /* checksum: unused by the host daemon */
    x9_mux_wr16(tcp + 18, 0);
    x9_mux_send_ctrl(e, X9_MUX_PROTO_TCP, tcp, 20);
}

static inline void x9_mux_send_rst(struct x9_mux_engine *e,
    uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack)
{
    x9_mux_send_tcp(e, sport, dport, seq, ack, X9_MUX_TH_RST | X9_MUX_TH_ACK,
        0, NULL, 0);
    e->stat_rst_out++;
}

static inline struct x9_mux_conn *x9_mux_find(struct x9_mux_engine *e,
    uint16_t sport, uint16_t dport)
{
    for (unsigned i = 0; i < X9_MUX_CONNS; i++)
        if (e->conns[i].used && e->conns[i].sport == sport &&
            e->conns[i].dport == dport)
            return &e->conns[i];
    return NULL;
}

static inline struct x9_mux_conn *x9_mux_find_handle(
    struct x9_mux_engine *e, int handle)
{
    for (unsigned i = 0; i < X9_MUX_CONNS; i++)
        if (e->conns[i].used && e->conns[i].handle == handle)
            return &e->conns[i];
    return NULL;
}

static inline void x9_mux_handle_syn(struct x9_mux_engine *e,
    uint16_t sport, uint16_t dport, uint32_t seq, uint32_t win)
{
    struct x9_mux_conn *c = x9_mux_find(e, sport, dport);
    if (c) {
        /* Retransmitted SYN for a pending connection: resend SYN/ACK. */
        if (!c->established)
            x9_mux_send_tcp(e, c->dport, c->sport, c->isn, c->rx_base,
                X9_MUX_TH_SYN | X9_MUX_TH_ACK, X9_MUX_RX_WINDOW, NULL, 0);
        return;
    }
    e->stat_syn++;
    int handle = e->ops.local_open(e->ctx, dport);
    if (handle < 0) {
        e->stat_refused++;
        x9_mux_send_rst(e, dport, sport, 0, seq + 1);
        return;
    }
    struct x9_mux_conn *slot = NULL;
    for (unsigned i = 0; i < X9_MUX_CONNS; i++)
        if (!e->conns[i].used) { slot = &e->conns[i]; break; }
    if (!slot) {
        e->ops.local_close(e->ctx, handle);
        e->stat_refused++;
        x9_mux_send_rst(e, dport, sport, 0, seq + 1);
        return;
    }
    memset(slot, 0, sizeof(*slot));
    slot->used = 1;
    slot->handle = handle;
    slot->sport = sport;
    slot->dport = dport;
    slot->isn = e->isn_counter;
    e->isn_counter += 0x10000u;
    slot->rx_base = seq + 1;
    slot->rx_expect = seq + 1;
    slot->tx_next = slot->isn;
    slot->tx_ackd = slot->isn;
    slot->host_win = win;
    slot->last_ack_sent = slot->rx_base;
    slot->last_progress_ms = e->now_ms;
    x9_mux_send_tcp(e, dport, sport, slot->isn, slot->rx_base,
        X9_MUX_TH_SYN | X9_MUX_TH_ACK, X9_MUX_RX_WINDOW, NULL, 0);
    slot->tx_next = slot->isn + 1;
}

static inline void x9_mux_handle_tcp(struct x9_mux_engine *e,
    const uint8_t *p, uint32_t n)
{
    if (n < 20)
        return;
    uint16_t sport = x9_mux_rd16(p + 0), dport = x9_mux_rd16(p + 2);
    uint32_t seq = x9_mux_rd32(p + 4), ack = x9_mux_rd32(p + 8);
    uint32_t off = (uint32_t)(p[12] >> 4) * 4;
    uint8_t flags = p[13];
    uint32_t win = (uint32_t)x9_mux_rd16(p + 14) << 8;
    if (off < 20 || off > n)
        return;
    const uint8_t *payload = p + off;
    uint32_t plen = n - off;
    struct x9_mux_conn *c = x9_mux_find(e, sport, dport);

    if (flags & X9_MUX_TH_RST) {
        if (c) {
            e->stat_rst_in++;
            x9_mux_conn_free(e, c);
        }
        return;
    }
    if ((flags & X9_MUX_TH_SYN) && !(flags & X9_MUX_TH_ACK)) {
        x9_mux_handle_syn(e, sport, dport, seq, win);
        return;
    }
    if (!c) {
        /* Data/ACK for a connection we no longer have: reset it. */
        x9_mux_send_rst(e, dport, sport, (flags & X9_MUX_TH_ACK) ? ack : 0,
            (flags & X9_MUX_TH_ACK) ? seq + plen : 0);
        return;
    }
    c->host_win = win;
    if ((int32_t)(ack - c->tx_ackd) > 0 && (int32_t)(ack - c->tx_next) <= 0) {
        c->tx_ackd = ack;
        c->last_progress_ms = e->now_ms;
    }
    if (!c->established && (flags & X9_MUX_TH_ACK) && ack == c->isn + 1)
        c->established = 1;
    if (plen) {
        uint32_t skip = 0;
        if ((int32_t)(seq - c->rx_expect) < 0)
            skip = c->rx_expect - seq; /* retransmission overlap */
        if (skip < plen && (int32_t)(seq + skip - c->rx_expect) == 0) {
            uint32_t want = plen - skip;
            if (c->pend_head + c->pend_len + want > X9_MUX_RX_WINDOW) {
                memmove(c->pend, c->pend + c->pend_head, c->pend_len);
                c->pend_head = 0;
            }
            if (c->pend_len + want <= X9_MUX_RX_WINDOW) {
                memcpy(c->pend + c->pend_head + c->pend_len,
                    payload + skip, want);
                c->pend_len += want;
                c->rx_expect += want;
                e->stat_bytes_in += want;
            }
        }
    }
    if (flags & X9_MUX_TH_FIN)
        c->peer_closed = 1;
}

static inline void x9_mux_process_packet(struct x9_mux_engine *e,
    const uint8_t *pkt, uint32_t n)
{
    uint32_t proto = x9_mux_rd32(pkt + 0);
    uint32_t hdr = e->state == 0 ? 8u : x9_mux_hdr_size(e);
    if (n < hdr)
        return;
    if (e->version == 2) {
        if (x9_mux_rd32(pkt + 8) != 0xfeedfaceu) {
            e->stat_bad_magic++;
            return;
        }
        e->last_rx_seq = x9_mux_rd16(pkt + 12);
    }
    const uint8_t *payload = pkt + hdr;
    uint32_t plen = n - hdr;

    if (e->state == 0) {
        if (proto != X9_MUX_PROTO_VERSION || plen < 8)
            return;
        uint32_t major = x9_mux_rd32(payload + 0);
        uint8_t reply[8];
        x9_mux_wr32(reply + 0, major == 2 ? 2u : 1u);
        x9_mux_wr32(reply + 4, 0);
        /* The version reply always uses the 8-byte header: the host is
         * still parsing at version 0 when it reads it. Build it manually
         * instead of x9_mux_build(). */
        uint8_t pkt16[16];
        x9_mux_wr32(pkt16 + 0, X9_MUX_PROTO_VERSION);
        x9_mux_wr32(pkt16 + 4, 16);
        memcpy(pkt16 + 8, reply, 8);
        if (e->ops.tx_packet(e->ctx, pkt16, 16) <= 0) {
            memcpy(e->pend_pkt, pkt16, 16);
            e->pend_pkt_len = 16;
        }
        e->stat_version++;
        if (major == 2) {
            e->version = 2;
            e->state = 1; /* wait for SETUP */
        } else {
            e->version = 1;
            e->state = 2;
        }
        return;
    }
    if (proto == X9_MUX_PROTO_SETUP) {
        if (e->state == 1) {
            e->state = 2;
            e->stat_setup++;
        }
        return;
    }
    if (proto == X9_MUX_PROTO_CONTROL) {
        e->stat_control++;
        return;
    }
    if (proto == X9_MUX_PROTO_TCP && e->state == 2)
        x9_mux_handle_tcp(e, payload, plen);
    /* Anything else (incl. duplicate VERSION) is dropped. */
}

/* Feed host->device USB bytes (arbitrary chunking). */
static inline void x9_mux_feed(struct x9_mux_engine *e,
    const uint8_t *bytes, uint32_t n)
{
    while (n) {
        uint32_t room = sizeof(e->rxbuf) - e->rx_len;
        uint32_t take = n < room ? n : room;
        memcpy(e->rxbuf + e->rx_len, bytes, take);
        e->rx_len += take;
        bytes += take;
        n -= take;
        for (;;) {
            if (e->rx_len < 8)
                break;
            uint32_t total = x9_mux_rd32(e->rxbuf + 4);
            uint32_t hdr = e->state == 0 ? 8u : x9_mux_hdr_size(e);
            if (total < hdr || total > X9_MUX_MAX_PACKET) {
                /* Unrecoverable framing: drop the buffer. In practice
                 * USB transfers keep packets aligned; this is a guard. */
                e->stat_framing++;
                e->rx_len = 0;
                break;
            }
            if (e->rx_len < total)
                break;
            x9_mux_process_packet(e, e->rxbuf, total);
            memmove(e->rxbuf, e->rxbuf + total, e->rx_len - total);
            e->rx_len -= total;
        }
        if (take == 0)
            break; /* rxbuf full of one incomplete packet: wait for pump */
    }
}

/* Close a connection toward the host with RST (the protocol's only close)
 * and free it. */
static inline void x9_mux_close_conn(struct x9_mux_engine *e,
    struct x9_mux_conn *c, int send_rst)
{
    if (send_rst)
        x9_mux_send_rst(e, c->dport, c->sport, c->tx_next,
            x9_mux_ack_value(c));
    x9_mux_conn_free(e, c);
}

/* Drive delivery, ACKs, data segmentation and connection teardown.
 * Call on every worker pass with a millisecond clock. */
static inline void x9_mux_pump(struct x9_mux_engine *e, uint64_t now_ms)
{
    e->now_ms = now_ms;
    if (e->pend_pkt_len &&
        e->ops.tx_packet(e->ctx, e->pend_pkt, e->pend_pkt_len) > 0)
        e->pend_pkt_len = 0;

    for (unsigned i = 0; i < X9_MUX_CONNS; i++) {
        struct x9_mux_conn *c = &e->conns[i];
        if (!c->used)
            continue;

        /* Deliver pending host->local bytes; ACK follows consumption. */
        while (c->pend_len) {
            int sent = e->ops.local_send(e->ctx, c->handle,
                c->pend + c->pend_head, c->pend_len);
            if (sent < 0) {
                x9_mux_close_conn(e, c, 1);
                break;
            }
            if (sent == 0)
                break;
            c->pend_head += (uint32_t)sent;
            c->pend_len -= (uint32_t)sent;
            if (!c->pend_len)
                c->pend_head = 0;
        }
        if (!c->used)
            continue;

        /* Cumulative ACK whenever consumption advanced. */
        uint32_t ack = x9_mux_ack_value(c);
        if (ack != c->last_ack_sent) {
            x9_mux_send_tcp(e, c->dport, c->sport, c->tx_next, ack,
                X9_MUX_TH_ACK, X9_MUX_RX_WINDOW - c->pend_len, NULL, 0);
            c->last_ack_sent = ack;
        }

        /* Segment local->host data, paced by the host window and ACKs.
         * Each byte is sent exactly once; nothing is retransmitted. */
        while (c->tx_len) {
            uint32_t unacked = c->tx_next - c->tx_ackd;
            if (unacked >= c->host_win)
                break;
            uint32_t seg = c->tx_len;
            if (seg > X9_MUX_SEG_PAYLOAD)
                seg = X9_MUX_SEG_PAYLOAD;
            if (seg > c->host_win - unacked)
                seg = c->host_win - unacked;
            uint8_t tcp[20 + X9_MUX_SEG_PAYLOAD];
            x9_mux_wr16(tcp + 0, c->dport);
            x9_mux_wr16(tcp + 2, c->sport);
            x9_mux_wr32(tcp + 4, c->tx_next);
            x9_mux_wr32(tcp + 8, ack);
            tcp[12] = 5 << 4;
            tcp[13] = X9_MUX_TH_ACK; /* host rejects any other data flags */
            x9_mux_wr16(tcp + 14,
                (uint16_t)((X9_MUX_RX_WINDOW - c->pend_len) >> 8));
            x9_mux_wr16(tcp + 16, 0);
            x9_mux_wr16(tcp + 18, 0);
            if (c->tx_head + seg > X9_MUX_TXBUF) {
                memmove(c->txbuf, c->txbuf + c->tx_head, c->tx_len);
                c->tx_head = 0;
            }
            memcpy(tcp + 20, c->txbuf + c->tx_head, seg);
            uint8_t pkt[4096];
            uint32_t total = x9_mux_build(e, X9_MUX_PROTO_TCP, tcp,
                20 + seg, pkt);
            if (e->ops.tx_packet(e->ctx, pkt, (int)total) <= 0)
                break; /* transport busy; retry next pump */
            c->tx_head += seg;
            c->tx_len -= seg;
            if (!c->tx_len)
                c->tx_head = 0;
            c->tx_next += seg;
            c->last_ack_sent = ack;
            e->stat_bytes_out += seg;
        }

        /* Orderly close: local EOF (or host FIN) with everything flushed
         * and acknowledged ends the connection with RST. */
        int drained = !c->tx_len && c->tx_next == c->tx_ackd;
        if ((c->local_eof || c->peer_closed) && !c->pend_len && drained) {
            x9_mux_close_conn(e, c, 1);
            continue;
        }
        /* Stall: unacked data and no ACK progress for 30s. */
        if (c->tx_next != c->tx_ackd &&
            now_ms - c->last_progress_ms > X9_MUX_STALL_MS) {
            x9_mux_close_conn(e, c, 1);
            continue;
        }
    }
}

/* Glue: bytes read from the local service. Returns bytes consumed. */
static inline uint32_t x9_mux_local_data(struct x9_mux_engine *e,
    int handle, const uint8_t *bytes, uint32_t n)
{
    struct x9_mux_conn *c = x9_mux_find_handle(e, handle);
    if (!c)
        return 0;
    if (c->tx_head + c->tx_len + n > X9_MUX_TXBUF) {
        memmove(c->txbuf, c->txbuf + c->tx_head, c->tx_len);
        c->tx_head = 0;
    }
    uint32_t room = X9_MUX_TXBUF - c->tx_len;
    uint32_t take = n < room ? n : room;
    memcpy(c->txbuf + c->tx_head + c->tx_len, bytes, take);
    c->tx_len += take;
    return take;
}

static inline uint32_t x9_mux_tx_room(struct x9_mux_engine *e, int handle)
{
    struct x9_mux_conn *c = x9_mux_find_handle(e, handle);
    return c ? X9_MUX_TXBUF - c->tx_len : 0;
}

static inline void x9_mux_local_eof(struct x9_mux_engine *e, int handle)
{
    struct x9_mux_conn *c = x9_mux_find_handle(e, handle);
    if (c)
        c->local_eof = 1;
}

static inline void x9_mux_local_error(struct x9_mux_engine *e, int handle)
{
    struct x9_mux_conn *c = x9_mux_find_handle(e, handle);
    if (c)
        x9_mux_close_conn(e, c, 1);
}

static inline unsigned x9_mux_active_conns(struct x9_mux_engine *e)
{
    unsigned n = 0;
    for (unsigned i = 0; i < X9_MUX_CONNS; i++)
        n += e->conns[i].used ? 1 : 0;
    return n;
}

#endif /* X9_USB_MUX_H */
