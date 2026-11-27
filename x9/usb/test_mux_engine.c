/* Host harness for x9_usb_mux.h: mocks the usbmuxd host and the local
 * loopback services, then scripts full conversations and asserts the
 * wire behaviour the real daemon requires. Built/run by
 * test_mux_engine.py. */
#include <stdio.h>
#include <stdlib.h>
#include "x9_usb_mux.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

/* ---- mock transport ---------------------------------------------------- */
static uint8_t tx_capture[1 << 20];
static uint32_t tx_len;
static uint32_t tx_pkts[256];
static unsigned tx_pkt_count;
static int tx_busy; /* when set, tx_packet refuses */

static int mock_tx(void *ctx, const uint8_t *bytes, uint32_t n)
{
    (void)ctx;
    if (tx_busy)
        return 0;
    if (tx_pkt_count < 256)
        tx_pkts[tx_pkt_count++] = n;
    memcpy(tx_capture + tx_len, bytes, n);
    tx_len += n;
    return (int)n;
}

static const uint8_t *pkt_at(unsigned idx, uint32_t *n)
{
    uint32_t off = 0;
    for (unsigned i = 0; i < idx; i++)
        off += tx_pkts[i];
    *n = tx_pkts[idx];
    return tx_capture + off;
}

/* ---- mock local services ---------------------------------------------- */
#define MOCK_MAX_HANDLE 16
static uint8_t local_rx[MOCK_MAX_HANDLE][65536];
static uint32_t local_rx_len[MOCK_MAX_HANDLE];
static int local_closed[MOCK_MAX_HANDLE];
static uint32_t local_consume_cap; /* per-call cap; 0 = unlimited */

static int mock_open(void *ctx, uint16_t dport)
{
    (void)ctx;
    if (dport != 62078 && dport != 1234)
        return -1;
    for (int h = 0; h < MOCK_MAX_HANDLE; h++)
        if (!local_closed[h] && local_rx_len[h] == 0) {
            /* find a fresh handle slot: use open_count tracking instead */
        }
    static int next_handle = 0;
    int h = next_handle++;
    local_closed[h] = 0;
    local_rx_len[h] = 0;
    return h;
}

static int mock_send(void *ctx, int handle, const uint8_t *bytes, uint32_t n)
{
    (void)ctx;
    if (handle < 0 || handle >= MOCK_MAX_HANDLE || local_closed[handle])
        return -1;
    uint32_t take = n;
    if (local_consume_cap && take > local_consume_cap)
        take = local_consume_cap;
    memcpy(local_rx[handle] + local_rx_len[handle], bytes, take);
    local_rx_len[handle] += take;
    return (int)take;
}

static void mock_close(void *ctx, int handle)
{
    (void)ctx;
    if (handle >= 0 && handle < MOCK_MAX_HANDLE)
        local_closed[handle] = 1;
}

static struct x9_mux_engine eng;
static struct x9_mux_ops ops = { mock_tx, mock_open, mock_send, mock_close };

static void reset_all(void)
{
    memset(&eng, 0, sizeof(eng));
    memset(tx_capture, 0, sizeof(tx_capture));
    memset(local_rx_len, 0, sizeof(local_rx_len));
    memset(local_closed, 0, sizeof(local_closed));
    tx_len = 0;
    tx_pkt_count = 0;
    tx_busy = 0;
    local_consume_cap = 0;
    x9_mux_init(&eng, &ops, NULL, 0x1000);
}

/* ---- host packet builders ---------------------------------------------- */
static uint8_t host_pkt[65536];
static uint32_t build_mux(uint32_t proto, const uint8_t *payload, uint32_t n,
    int v2, uint16_t tx_seq)
{
    uint32_t hdr = v2 ? 16 : 8;
    x9_mux_wr32(host_pkt + 0, proto);
    x9_mux_wr32(host_pkt + 4, hdr + n);
    if (v2) {
        x9_mux_wr32(host_pkt + 8, 0xfeedfaceu);
        x9_mux_wr16(host_pkt + 12, tx_seq);
        x9_mux_wr16(host_pkt + 14, 0xffff);
    }
    memcpy(host_pkt + hdr, payload, n);
    return hdr + n;
}

static uint32_t build_tcp(uint16_t sport, uint16_t dport, uint32_t seq,
    uint32_t ack, uint8_t flags, uint16_t win_wire,
    const uint8_t *payload, uint32_t n)
{
    uint8_t tcp[20 + 8192];
    x9_mux_wr16(tcp + 0, sport);
    x9_mux_wr16(tcp + 2, dport);
    x9_mux_wr32(tcp + 4, seq);
    x9_mux_wr32(tcp + 8, ack);
    tcp[12] = 5 << 4;
    tcp[13] = flags;
    x9_mux_wr16(tcp + 14, win_wire);
    x9_mux_wr16(tcp + 16, 0);
    x9_mux_wr16(tcp + 18, 0);
    if (n)
        memcpy(tcp + 20, payload, n);
    return build_mux(6, tcp, 20 + n, eng.version == 2, 7);
}

static void feed_handshake_v2(void)
{
    uint8_t ver[8];
    x9_mux_wr32(ver + 0, 2);
    x9_mux_wr32(ver + 4, 0);
    uint32_t n = build_mux(0, ver, 8, 0, 0);
    x9_mux_feed(&eng, host_pkt, n);
    uint8_t setup = 0x07;
    n = build_mux(2, &setup, 1, 1, 0);
    x9_mux_feed(&eng, host_pkt, n);
    tx_len = 0; /* drop the version reply from capture */
    tx_pkt_count = 0;
}

/* Parse the idx-th captured packet as mux+tcp (v2). */
struct parsed { uint32_t proto; uint16_t sport, dport; uint32_t seq, ack;
    uint8_t flags; uint16_t win; const uint8_t *payload; uint32_t plen; };
static void parse_pkt(unsigned idx, struct parsed *out)
{
    uint32_t n;
    const uint8_t *p = pkt_at(idx, &n);
    uint32_t hdr = eng.version == 2 ? 16 : 8;
    out->proto = x9_mux_rd32(p + 0);
    const uint8_t *t = p + hdr;
    out->sport = x9_mux_rd16(t + 0);
    out->dport = x9_mux_rd16(t + 2);
    out->seq = x9_mux_rd32(t + 4);
    out->ack = x9_mux_rd32(t + 8);
    out->flags = t[13];
    out->win = x9_mux_rd16(t + 14);
    out->payload = t + 20;
    out->plen = n - hdr - 20;
}

int main(void)
{
    struct parsed P;

    /* 1. Version handshake v2, byte-exact reply. */
    reset_all();
    {
        uint8_t ver[8];
        x9_mux_wr32(ver + 0, 2);
        x9_mux_wr32(ver + 4, 0);
        uint32_t n = build_mux(0, ver, 8, 0, 0);
        x9_mux_feed(&eng, host_pkt, n);
        CHECK(tx_pkt_count == 1);
        uint32_t pn;
        const uint8_t *p = pkt_at(0, &pn);
        static const uint8_t want[16] = {0,0,0,0, 0,0,0,16, 0,0,0,2, 0,0,0,0};
        CHECK(pn == 16 && memcmp(p, want, 16) == 0);
        CHECK(eng.state == 1);
        uint8_t setup = 0x07;
        n = build_mux(2, &setup, 1, 1, 0);
        x9_mux_feed(&eng, host_pkt, n);
        CHECK(eng.state == 2);
        CHECK(tx_pkt_count == 1); /* SETUP gets no reply */
    }

    /* 2. SYN -> SYN/ACK fields. */
    reset_all();
    feed_handshake_v2();
    {
        uint32_t n = build_tcp(0xBEEF, 62078, 0, 0, X9_MUX_TH_SYN, 512, NULL, 0);
        x9_mux_feed(&eng, host_pkt, n);
        CHECK(tx_pkt_count == 1);
        parse_pkt(0, &P);
        CHECK(P.proto == 6 && P.sport == 62078 && P.dport == 0xBEEF);
        CHECK(P.flags == (X9_MUX_TH_SYN | X9_MUX_TH_ACK));
        CHECK(P.seq == 0x1000 && P.ack == 1);
        CHECK(P.win == (X9_MUX_RX_WINDOW >> 8));
        CHECK(x9_mux_active_conns(&eng) == 1);
    }

    /* 3. ACK establishes; host data delivered; cumulative ACK. */
    {
        uint32_t n = build_tcp(0xBEEF, 62078, 1, 0x1001, X9_MUX_TH_ACK, 512, NULL, 0);
        x9_mux_feed(&eng, host_pkt, n);
        n = build_tcp(0xBEEF, 62078, 1, 0x1001, X9_MUX_TH_ACK, 512,
            (const uint8_t *)"hello", 5);
        x9_mux_feed(&eng, host_pkt, n);
        tx_pkt_count = 0; tx_len = 0;
        x9_mux_pump(&eng, 1000);
        CHECK(local_rx_len[0] == 5 && memcmp(local_rx[0], "hello", 5) == 0);
        CHECK(tx_pkt_count == 1);
        parse_pkt(0, &P);
        CHECK(P.flags == X9_MUX_TH_ACK && P.ack == 6 && P.plen == 0);
    }

    /* 4. Duplicate data is not delivered twice. */
    {
        uint32_t n = build_tcp(0xBEEF, 62078, 1, 0x1001, X9_MUX_TH_ACK, 512,
            (const uint8_t *)"hello", 5);
        x9_mux_feed(&eng, host_pkt, n);
        x9_mux_pump(&eng, 1001);
        CHECK(local_rx_len[0] == 5);
    }

    /* 5. Device->host data: ACK flag only, seq continues from ISN+1. */
    {
        CHECK(x9_mux_local_data(&eng, 0, (const uint8_t *)"world!", 6) == 6);
        tx_pkt_count = 0; tx_len = 0;
        x9_mux_pump(&eng, 1002);
        CHECK(tx_pkt_count == 1);
        parse_pkt(0, &P);
        CHECK(P.flags == X9_MUX_TH_ACK);
        CHECK(P.seq == 0x1001 && P.plen == 6);
        CHECK(memcmp(P.payload, "world!", 6) == 0);
    }

    /* 6. Local EOF after drain closes with RST and closes the socket. */
    {
        uint32_t n = build_tcp(0xBEEF, 62078, 6, 0x1007, X9_MUX_TH_ACK, 512, NULL, 0);
        x9_mux_feed(&eng, host_pkt, n); /* host ACKs our 6 bytes */
        x9_mux_local_eof(&eng, 0);
        tx_pkt_count = 0; tx_len = 0;
        x9_mux_pump(&eng, 1003);
        CHECK(tx_pkt_count == 1);
        parse_pkt(0, &P);
        CHECK(P.flags & X9_MUX_TH_RST);
        CHECK(local_closed[0] == 1);
        CHECK(x9_mux_active_conns(&eng) == 0);
    }

    /* 7. SYN to a refused port -> RST with ack = host_isn+1. */
    reset_all();
    feed_handshake_v2();
    {
        uint32_t n = build_tcp(0x1234, 9999, 0, 0, X9_MUX_TH_SYN, 512, NULL, 0);
        x9_mux_feed(&eng, host_pkt, n);
        CHECK(tx_pkt_count == 1);
        parse_pkt(0, &P);
        CHECK(P.sport == 9999 && P.dport == 0x1234);
        CHECK(P.flags & X9_MUX_TH_RST);
        CHECK(P.ack == 1);
        CHECK(x9_mux_active_conns(&eng) == 0);
        CHECK(eng.stat_refused == 1);
    }

    /* 8. Window pacing: host window caps unacked data. */
    reset_all();
    feed_handshake_v2();
    {
        uint32_t n = build_tcp(0x2222, 1234, 0, 0, X9_MUX_TH_SYN, 4, NULL, 0);
        x9_mux_feed(&eng, host_pkt, n); /* win = 4<<8 = 1024 */
        parse_pkt(0, &P);
        uint32_t isn = P.seq;
        n = build_tcp(0x2222, 1234, 1, isn + 1, X9_MUX_TH_ACK, 4, NULL, 0);
        x9_mux_feed(&eng, host_pkt, n);
        static uint8_t big[3000];
        memset(big, 0xAB, sizeof(big));
        int h = eng.conns[0].handle;
        CHECK(x9_mux_local_data(&eng, h, big, 3000) == 3000);
        tx_pkt_count = 0; tx_len = 0;
        x9_mux_pump(&eng, 2000);
        uint32_t sent1 = 0;
        for (unsigned i = 0; i < tx_pkt_count; i++) {
            parse_pkt(i, &P);
            CHECK(P.flags == X9_MUX_TH_ACK);
            sent1 += P.plen;
        }
        CHECK(sent1 == 1024); /* SYN already ACKed: full window for data */
        n = build_tcp(0x2222, 1234, 1, isn + 1 + 1024, X9_MUX_TH_ACK, 4, NULL, 0);
        x9_mux_feed(&eng, host_pkt, n);
        tx_pkt_count = 0; tx_len = 0;
        x9_mux_pump(&eng, 2001);
        uint32_t sent2 = 0;
        for (unsigned i = 0; i < tx_pkt_count; i++) {
            parse_pkt(i, &P);
            sent2 += P.plen;
        }
        CHECK(sent2 == 1024);
        n = build_tcp(0x2222, 1234, 1, isn + 1 + 2048, X9_MUX_TH_ACK, 4, NULL, 0);
        x9_mux_feed(&eng, host_pkt, n);
        tx_pkt_count = 0; tx_len = 0;
        x9_mux_pump(&eng, 2002);
        uint32_t sent3 = 0;
        for (unsigned i = 0; i < tx_pkt_count; i++) {
            parse_pkt(i, &P);
            sent3 += P.plen;
        }
        CHECK(sent3 == 952);
    }

    /* 9. Host RST tears the connection down locally. */
    {
        uint32_t n = build_tcp(0x2222, 1234, 1, 0, X9_MUX_TH_RST, 4, NULL, 0);
        int h = eng.conns[0].handle;
        x9_mux_feed(&eng, host_pkt, n);
        CHECK(x9_mux_active_conns(&eng) == 0);
        CHECK(local_closed[h] == 1);
    }

    /* 10. Byte-by-byte and coalesced stream reassembly. */
    reset_all();
    {
        uint8_t ver[8];
        x9_mux_wr32(ver + 0, 2);
        x9_mux_wr32(ver + 4, 0);
        uint32_t n = build_mux(0, ver, 8, 0, 0);
        uint8_t saved[64];
        memcpy(saved, host_pkt, n);
        for (uint32_t i = 0; i < n; i++)
            x9_mux_feed(&eng, saved + i, 1);
        CHECK(eng.state == 1 && tx_pkt_count == 1);
        uint8_t setup = 0x07;
        n = build_mux(2, &setup, 1, 1, 0);
        /* coalesce SETUP + SYN in one feed */
        uint8_t both[256];
        memcpy(both, host_pkt, n);
        uint32_t m = build_tcp(0x3333, 62078, 0, 0, X9_MUX_TH_SYN, 512, NULL, 0);
        memcpy(both + n, host_pkt, m);
        x9_mux_feed(&eng, both, n + m);
        CHECK(eng.state == 2);
        CHECK(x9_mux_active_conns(&eng) == 1);
    }

    /* 11. Version 1 handshake: 8-byte headers throughout. */
    reset_all();
    {
        uint8_t ver[8];
        x9_mux_wr32(ver + 0, 1);
        x9_mux_wr32(ver + 4, 0);
        uint32_t n = build_mux(0, ver, 8, 0, 0);
        x9_mux_feed(&eng, host_pkt, n);
        CHECK(eng.state == 2 && eng.version == 1);
        uint32_t pn;
        const uint8_t *p = pkt_at(0, &pn);
        CHECK(pn == 16 && x9_mux_rd32(p + 8) == 1);
        tx_pkt_count = 0; tx_len = 0;
        n = build_tcp(0x4444, 62078, 0, 0, X9_MUX_TH_SYN, 512, NULL, 0);
        x9_mux_feed(&eng, host_pkt, n);
        CHECK(tx_pkt_count == 1);
        p = pkt_at(0, &pn);
        CHECK(pn == 28); /* 8-byte mux header + 20-byte TCP */
        parse_pkt(0, &P);
        CHECK(P.flags == (X9_MUX_TH_SYN | X9_MUX_TH_ACK) && P.ack == 1);
    }

    if (failures) {
        printf("RESULT FAIL (%d)\n", failures);
        return 1;
    }
    printf("RESULT PASS\n");
    return 0;
}
