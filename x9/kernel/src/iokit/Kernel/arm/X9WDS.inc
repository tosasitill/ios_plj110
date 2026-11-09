/* Native WDS preflight/lifecycle boundary. Included by X9QMIAT.inc.
 * The host's mux/family binding is logical intent, NOT proof of a MediaTek
 * CID/ccmni route. No CGATT -> connected shortcut, guessed CID-1 channel,
 * fabricated packet handle, IP address, DNS, gateway or MTU.
 * Start performs real CGACT/CGDCONT discovery and once-per-boot read-only
 * CGDATA/CGCONTRDP capability queries. It never activates a context without
 * a proven bearer route. Capability acceptance alone is NOT that proof.
 */
struct x9w_client {
    uint8_t mux, family; bool starting;
};
static x9w_client x9w_clients[CBQ_MAX_CLIENTS];
struct x9w_discovery {
    uint32_t present, active, defined; // bit CID-1 is a bitmap, NEVER a ccmni index
    unsigned ipv4, ipv6, dual, other;
    bool caps_attempted, caps_complete, cgdata, contrdp;
};
static x9w_discovery x9w;
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
static void x9w_start(x9q_job &j) { x9q_command(j, "AT+CGACT?\r", 90); }
static bool x9w_cgact(const char *s, uint32_t &present, uint32_t &active) {
    present = active = 0;
    while (*s) {
        const char *end = s; while (*end && *end != '\r' && *end != '\n') end++;
        if (end - s >= 7 && !memcmp(s, "+CGACT:", 7)) {
            char line[64]; unsigned size = (unsigned)(end - s - 7);
            if (size >= sizeof(line)) return false;
            memcpy(line, s + 7, size); line[size] = 0;
            const char *p = line; unsigned cid, state;
            if (!x9q_number(p, cid) || *p++ != ',' || !x9q_number(p, state) || *p ||
                !cid || cid > 16 || state > 1 || (present & (1u << (cid - 1)))) return false;
            present |= 1u << (cid - 1); if (state) active |= 1u << (cid - 1);
        }
        s = end; while (*s == '\r' || *s == '\n') s++;
    }
    // Empty successful response is a valid zero-context discovery.
    return true;
}
static unsigned x9w_count(uint32_t bits) {
    unsigned n = 0; while (bits) { bits &= bits - 1; n++; } return n;
}
// A bounded quoted field; strings stay in the response buffer and are wiped by
// the AT owner. APNs/addresses are neither logged nor persisted by discovery.
static bool x9w_quoted(const char *&p, const char *&text, unsigned &n) {
    while (*p == ' ') p++; if (*p != '"') return false;
    text = ++p; n = 0;
    while (*p && *p != '"') {
        if ((unsigned char)*p < 32 || (unsigned char)*p > 126 || *p == '\\' || ++n > 255) return false;
        p++;
    }
    if (*p != '"') return false; p++; while (*p == ' ') p++; return true;
}
static bool x9w_cgdcont(const char *s) {
    uint32_t defined = 0; unsigned ipv4 = 0, ipv6 = 0, dual = 0, other = 0;
    while (*s) {
        const char *end = s; while (*end && *end != '\r' && *end != '\n') end++;
        if (end - s >= 9 && !memcmp(s, "+CGDCONT:", 9)) {
            char line[512]; unsigned size = (unsigned)(end - s - 9);
            if (size >= sizeof(line)) return false;
            memcpy(line, s + 9, size); line[size] = 0;
            const char *p = line, *type, *apn; unsigned cid, tn, an;
            if (!x9q_number(p, cid) || !cid || cid > 16 || (defined & (1u << (cid - 1))) ||
                *p++ != ',' || !x9w_quoted(p, type, tn) || *p++ != ',' || !x9w_quoted(p, apn, an)) {
                bzero(line, sizeof(line)); return false;
            }
            // Address and optional vendor fields are skipped only as complete
            // quoted strings or decimal fields; reject malformed/truncated CSV.
            bool valid = true;
            while (*p && valid) {
                if (*p++ != ',') { valid = false; break; }
                while (*p == ' ') p++;
                if (*p == '"') { const char *v; unsigned k; valid = x9w_quoted(p, v, k); }
                else if (*p && *p != ',') { unsigned value; valid = x9q_number(p, value); }
            }
            if (!valid) { bzero(line, sizeof(line)); return false; }
            defined |= 1u << (cid - 1);
            if (tn == 2 && !memcmp(type, "IP", 2)) ipv4++;
            else if (tn == 4 && !memcmp(type, "IPV6", 4)) ipv6++;
            else if (tn == 6 && !memcmp(type, "IPV4V6", 6)) dual++;
            else other++;
            bzero(line, sizeof(line));
        }
        s = end; while (*s == '\r' || *s == '\n') s++;
    }
    x9w.defined = defined; x9w.ipv4 = ipv4; x9w.ipv6 = ipv6; x9w.dual = dual; x9w.other = other;
    return true;
}
static void x9w_start_blocked(x9q_job &j) {
    printf("X9 WDS: Start discovery contexts %u active %u defined %u families %u/%u/%u other %u, capability tests %u/%u complete %u, DPMAIF ready %u; blocked at %s (no activation)\n",
        x9w_count(x9w.present), x9w_count(x9w.active), x9w_count(x9w.defined), x9w.ipv4, x9w.ipv6, x9w.dual, x9w.other,
        x9w.cgdata, x9w.contrdp, x9w.caps_complete, x9d_ready(),
        x9d_ready() ? "unproven CID/ccmni binding" : "DPMAIF readiness");
    x9q_error(j, x9d_ready() ? X9Q_UNSUPPORTED : X9Q_NOT_READY);
}
static void x9w_result(x9q_job &j, const char *response, int result) {
    if (j.stage == 92 || j.stage == 93) {
        // MODEM_ERROR is a completed negative capability response, not a
        // reason to retry or to send the execution form of the command.
        if (result != X9_AT_OK && result != X9_AT_MODEM_ERROR) { x9q_error(j, X9Q_NOT_READY); return; }
        if (j.stage == 92) {
            x9w.cgdata = result == X9_AT_OK;
            x9q_command(j, "AT+CGCONTRDP=?\r", 93); return;
        }
        x9w.contrdp = result == X9_AT_OK; x9w.caps_complete = true;
        x9w_start_blocked(j); return;
    }
    if (result != X9_AT_OK) { x9q_error(j, X9Q_NOT_READY); return; }
    if (j.stage == 91) {
        if (!x9w_cgdcont(response)) { x9q_error(j, X9Q_INTERNAL); return; }
        if (!x9w.caps_attempted) {
            x9w.caps_attempted = true; x9q_command(j, "AT+CGDATA=?\r", 92); return;
        }
        x9w_start_blocked(j); return;
    }
    uint32_t contexts, active;
    if (j.stage != 90 || !x9w_cgact(response, contexts, active)) { x9q_error(j, X9Q_INTERNAL); return; }
    if (j.id == 0x22) {
        // Per-client packet status, not global CGATT/CGACT. This version has
        // created no bearer handles; an already-active MD context is unowned.
        x9q_packet p(j.req); uint8_t disconnected = 1; p.tlv(1, &disconnected, 1);
        printf("X9 WDS: CGACT discovery contexts %u active %u; client owns no routed bearer\n", x9w_count(contexts), x9w_count(active));
        x9q_finish(j, p); return;
    }
    x9w.present = contexts; x9w.active = active;
    x9q_command(j, "AT+CGDCONT?\r", 91);
}
