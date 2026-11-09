/* Native WDS preflight/lifecycle boundary. Included by X9QMIAT.inc.
 * The host's mux/family binding is logical intent, NOT proof of a MediaTek
 * CID/ccmni route. No CGATT -> connected shortcut, guessed CID-1 channel,
 * fabricated packet handle, IP address, DNS, gateway or MTU.
 * Start currently performs real CGACT discovery then fails at the first
 * missing gate; it never activates a context without a proven bearer route.
 */
struct x9w_client {
    uint8_t mux, family; bool starting;
};
static x9w_client x9w_clients[CBQ_MAX_CLIENTS];
static bool x9w_supported(unsigned id) {
    return id == 0x20 || id == 0x21 || id == 0x22 || id == 0x2d || id == 0x4d || id == 0xa2;
}
static bool x9w_apn(const uint8_t *p, unsigned n) {
    if (!n || n > 100) return false;
    unsigned label = 0;
    for (unsigned i = 0; i < n; i++) {
        uint8_t c = p[i];
        if (c == '.') { if (!label || p[i - 1] == '-') return false; label = 0; continue; }
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) return false;
        if ((!label && c == '-') || ++label > 63) return false;
    }
    return label && p[n - 1] != '-';
}
static uint16_t x9w_start_args(const uint8_t *req, unsigned n, uint8_t family) {
    for (unsigned at = 7; at < n; at += 3 + x9q_u16(req + at + 1)) {
        const uint8_t *v = req + at + 3; unsigned k = x9q_u16(req + at + 1);
        switch (req[at]) {
        case 0x14: if (!x9w_apn(v, k)) return X9Q_ARG; break;
        case 0x19: if (k != 1 || !cbp_family_bit(v[0]) || v[0] != family) return X9Q_ARG; break;
        case 0x16: if (k != 1 || v[0]) return X9Q_UNSUPPORTED; break; // no auth-NV mutation
        case 0x33: if (k != 1 || v[0]) return X9Q_UNSUPPORTED; break; // no persistent auto-connect
        default: return X9Q_UNSUPPORTED; // profiles, credentials, IP preferences not translated yet
        }
    }
    return 0;
}
static void x9w_release_job(x9q_job &j) {
    if (j.port == X9BB_PORT_BASE + 1 && j.id == 0x20 && j.local >= CBQ_PORT_BASE &&
        j.local - CBQ_PORT_BASE < CBQ_MAX_CLIENTS &&
        j.generation == x9q_clients[j.local - CBQ_PORT_BASE].generation)
        x9w_clients[j.local - CBQ_PORT_BASE].starting = false;
}
static void x9w_del(unsigned slot) {
    // No physical PDP context has been activated by this version; do not issue
    // CGACT=0 against an unowned context on DEL_CLIENT. Invalidate queued TX only.
    unsigned mux = x9w_clients[slot].mux;
    bzero(&x9w_clients[slot], sizeof(x9w_clients[slot]));
    // x9d_bind has no producer until a real MTK binding contract is established.
    // Avoid unbinding a sibling logical WDS client that shares the same mux.
    bool shared = false;
    for (unsigned i = 0; i < CBQ_MAX_CLIENTS; i++) if (i != slot && x9w_clients[i].mux == mux) shared = true;
    if (!shared) x9d_unbind(mux);
}
static bool x9w_request(uint32_t local, uint32_t port, const uint8_t *req, unsigned n) {
    unsigned slot = local - CBQ_PORT_BASE, id = x9q_u16(req + 3);
    x9w_client &c = x9w_clients[slot];
    if (!x9w_supported(id)) return false;
    if (!c.family) c.family = 4;
    uint16_t error = 0;
    if (c.starting) error = X9Q_BUSY;
    else if (id == 0xa2) {
        x9q_view mux = x9q_tlv(req, n, 0x11), ep = x9q_tlv(req, n, 0x10), type = x9q_tlv(req, n, 0x13);
        if (!mux.p || mux.n != 1 || !cbp_mux_ok(mux.p[0])) error = X9Q_ARG;
        // The shim publishes Apple's PCIe interface 4. Accept that logical
        // endpoint or mux-only, never a manufactured embedded-IPA endpoint.
        if (ep.p && (ep.n != 8 || x9q_u32(ep.p) != 3 || x9q_u32(ep.p + 4) != 4)) error = X9Q_UNSUPPORTED;
        if (type.p) error = X9Q_UNSUPPORTED; // client-type routing not yet established
        for (unsigned at = 7; at < n; at += 3 + x9q_u16(req + at + 1))
            if (req[at] != 0x10 && req[at] != 0x11 && req[at] != 0x13) error = X9Q_ARG;
        if (!error) c.mux = mux.p[0];
    } else if (id == 0x4d) {
        x9q_view f = x9q_tlv(req, n, 1);
        if (!f.p || f.n != 1 || n != 11 || !cbp_family_bit(f.p[0])) error = X9Q_ARG;
        else c.family = f.p[0];
    } else if (id == 0x21) {
        x9q_view handle = x9q_tlv(req, n, 1);
        // No handles are manufactured; neither wildcard nor another client's
        // handle can deactivate a modem-owned context.
        error = !handle.p || handle.n != 4 ? X9Q_ARG : 9; // INVALID_HANDLE
    } else if (id == 0x2d) {
        x9q_view mask = x9q_tlv(req, n, 0x10);
        error = mask.p && mask.n == 4 && n == 14 ? 15 : X9Q_ARG; // OUT_OF_CALL
    } else if (id == 0x22) {
        if (n != 7) error = X9Q_ARG;
    } else if (id == 0x20) {
        x9q_view family = x9q_tlv(req, n, 0x19);
        uint8_t requested = family.p && family.n == 1 ? family.p[0] : c.family;
        if (!c.mux) error = X9Q_ARG;
        else if (!cbp_family_bit(requested)) error = X9Q_ARG;
        else error = x9w_start_args(req, n, requested);
        // Family changes are committed only by successful SetIPFamily. A
        // successful future Start must atomically commit its override/route.
    }
    if (error) { x9bb_reply_error(local, port, req, n, error); return true; }
    if (id == 0x20 || id == 0x22) {
        if (!x9q_enqueue(local, port, req, n)) x9bb_reply_error(local, port, req, n, X9Q_BUSY);
        else if (id == 0x20) c.starting = true;
        return true;
    }
    x9q_packet p(req); p.send(local, port);
    printf("X9 WDS: logical %s accepted, no physical bearer implied\n", id == 0xa2 ? "mux bind" : "family");
    return true;
}
static bool x9w_cgact(const char *s, unsigned &contexts, unsigned &active) {
    bool seen[17] = {}; contexts = active = 0;
    while (*s) {
        const char *end = s; while (*end && *end != '\r' && *end != '\n') end++;
        if (end - s >= 7 && !memcmp(s, "+CGACT:", 7)) {
            char line[64]; unsigned size = (unsigned)(end - s - 7);
            if (size >= sizeof(line)) return false;
            memcpy(line, s + 7, size); line[size] = 0;
            const char *p = line; unsigned cid, state;
            if (!x9q_number(p, cid) || *p++ != ',' || !x9q_number(p, state) || *p ||
                !cid || cid > 16 || state > 1 || seen[cid]) return false;
            seen[cid] = true; contexts++; active += state;
        }
        s = end; while (*s == '\r' || *s == '\n') s++;
    }
    // Empty successful response is a valid zero-context discovery.
    return true;
}
static void x9w_result(x9q_job &j, const char *response, int result) {
    unsigned contexts, active;
    if (result != X9_AT_OK) { x9q_error(j, X9Q_NOT_READY); return; }
    if (!x9w_cgact(response, contexts, active)) { x9q_error(j, X9Q_INTERNAL); return; }
    if (j.id == 0x22) {
        // Per-client packet status, not global CGATT/CGACT. This version has
        // created no bearer handles; an already-active MD context is unowned.
        x9q_packet p(j.req); uint8_t disconnected = 1; p.tlv(1, &disconnected, 1);
        printf("X9 WDS: CGACT discovery contexts %u active %u; client owns no routed bearer\n", contexts, active);
        x9q_finish(j, p); return;
    }
    printf("X9 WDS: Start preflight CGACT contexts %u active %u, DPMAIF ready %u; blocked at %s (no activation)\n",
        contexts, active, x9d_ready(), x9d_ready() ? "unproven CID/ccmni binding" : "DPMAIF readiness");
    x9q_error(j, x9d_ready() ? X9Q_UNSUPPORTED : X9Q_NOT_READY);
}
