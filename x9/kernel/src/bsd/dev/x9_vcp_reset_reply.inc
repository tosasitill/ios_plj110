/* Consumed by x9_vcp_wire.c; use its bounded LE helpers, never packed casts.
 * Firmware: a003 branch 30630c..306320 constructs b003 and passes service1;
 * helper 30b568/30b570 retains service/payload, 30b5fe..30b608 passes length40.
 * Host: payload8 live AP token lookup, payload16 signed status, b003/b004
 * waiter branches at 21d24..21d4c and 21ba8..21bcc. Static framing only:
 * unknown vendor gates/epilogues, codec RESET effects and runtime pairing
 * remain outside this parser. b004 is deliberately not an alternate ACK.
 */
int x9_vcp_parse_reset_reply(const struct x9_vcp_init *instance,
    const uint8_t *input, uint32_t bytes, struct x9_vcp_reset_result *out)
{
    struct x9_vcp_init q;
    struct x9_vcp_reset_result r;
    uint8_t snapshot[X9_VCP_WIRE_BYTES];
    const uint8_t *p;
    uint32_t status;
    if (!out) return EINVAL;
    if (!instance || !input || bytes != sizeof(snapshot)) {
        memset(out, 0, sizeof(*out));
        return EINVAL;
    }
    /* Own both inputs before clearing a possibly overlapping output. */
    memcpy(&q, instance, sizeof(q));
    memcpy(snapshot, input, sizeof(snapshot));
    memset(out, 0, sizeof(*out));
    memset(&r, 0, sizeof(r));
    p = snapshot + 8;
    if (!instance_matches(&q, get32(snapshot), get64(p + 8)) ||
        get32(snapshot + 4) != X9_VCP_RESET_REPLY_BYTES ||
        get32(p) != X9_VCP_RESET_DONE) return EINVAL;
    status = get32(p + 16);
    memcpy(&r.firmware_status, &status, sizeof(status));
    r.reply_context_id = get32(p + 4);
    memcpy(r.opaque, p + 20, sizeof(r.opaque));
    memcpy(out, &r, sizeof(r));
    return status ? EIO : 0;
}
