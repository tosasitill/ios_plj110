#include "hevc_bitstream.h"
#include <sys/errno.h>
#include <string.h>

struct bits {
    const uint8_t *p;
    size_t n, pos;
    unsigned zeroes, left, byte;
    int error;
};
static unsigned bit(struct bits *b)
{
    if (!b->left) {
        if (b->pos == b->n) { b->error = 1; return 0; }
        unsigned v = b->p[b->pos++];
        if (b->zeroes == 2 && v == 3) {
            if (b->pos == b->n || b->p[b->pos] > 3) { b->error = 1; return 0; }
            b->zeroes = 0;
            v = b->p[b->pos++];
        } else if (b->zeroes == 2 && v < 3) {
            b->error = 1; return 0; /* unescaped start-code emulation */
        }
        b->zeroes = v == 0 ? b->zeroes + 1 : 0;
        b->byte = v; b->left = 8;
    }
    return (b->byte >> --b->left) & 1;
}
static uint32_t get(struct bits *b, unsigned n)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < n && !b->error; i++) v = (v << 1) | bit(b);
    return v;
}
static void skip(struct bits *b, unsigned n)
{
    for (unsigned i = 0; i < n && !b->error; i++) (void)bit(b);
}
static uint32_t ue(struct bits *b)
{
    unsigned n = 0;
    while (!b->error && !bit(b)) {
        if (++n > 30) { b->error = 1; return 0; }
    }
    return b->error ? 0 : ((1u << n) - 1) + get(b, n);
}
static unsigned be16(const uint8_t *p) { return ((unsigned)p[0] << 8) | p[1]; }
static uint32_t be32(const uint8_t *p)
{ return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
#define MAIN_COMPATIBILITY (UINT32_C(1) << (31 - X9_HEVC_PROFILE_MAIN))
static int nal(const uint8_t *p, size_t n, unsigned type)
{
    if (n < 3 || (p[0] & 0x80) || ((p[0] >> 1) & 63) != type ||
        (p[0] & 1) || (p[1] >> 3) || !(p[1] & 7)) return EINVAL;
    return 0;
}
static int sps(const uint8_t *p, size_t n, struct x9_hevc_config *c)
{
    struct bits b = { .p = p + 2, .n = n - 2 };
    c->vps_id = get(&b, 4);
    unsigned sub = get(&b, 3);
    if (sub > 6) return EINVAL;
    (void)get(&b, 1);
    unsigned space = get(&b, 2); (void)get(&b, 1);
    c->profile = get(&b, 5);
    c->profile_compatibility = get(&b, 32);
    skip(&b, 48); c->level = get(&b, 8);
    unsigned pp[7] = {0}, lp[7] = {0};
    for (unsigned i = 0; i < sub; i++) { pp[i] = bit(&b); lp[i] = bit(&b); }
    if (sub) for (unsigned i = sub; i < 8; i++) if (get(&b, 2)) return EINVAL;
    for (unsigned i = 0; i < sub; i++) {
        if (pp[i]) skip(&b, 88);
        if (lp[i]) skip(&b, 8);
    }
    c->sps_id = ue(&b);
    unsigned chroma = ue(&b);
    if (chroma != 1 || space ||
        (c->profile != X9_HEVC_PROFILE_MAIN && c->profile != X9_HEVC_PROFILE_MAIN10 &&
         c->profile != X9_HEVC_PROFILE_MAIN_STILL)) return ENOTSUP;
    /* Main Still Picture is an independent-picture subset, not Main10 or
     * Rext. Keep its original profile/NALs; require explicit Main compatibility
     * before selecting the provider's existing Main capability. */
    c->decoder_profile = c->profile;
    if (c->profile == X9_HEVC_PROFILE_MAIN_STILL) {
        if (!(c->profile_compatibility & MAIN_COMPATIBILITY)) return ENOTSUP;
        c->decoder_profile = X9_HEVC_PROFILE_MAIN;
    }
    c->coded_width = ue(&b); c->coded_height = ue(&b);
    uint32_t l = 0, r = 0, t = 0, bot = 0;
    if (bit(&b)) { l = ue(&b); r = ue(&b); t = ue(&b); bot = ue(&b); }
    unsigned yd = ue(&b), cd = ue(&b);
    if (b.error || c->sps_id > 15 || !c->coded_width || !c->coded_height ||
        c->coded_width > X9_HEVC_MAX_DIM || c->coded_height > X9_HEVC_MAX_DIM ||
        (c->coded_width & 1) || (c->coded_height & 1) ||
        (uint64_t)l + r >= c->coded_width / 2 ||
        (uint64_t)t + bot >= c->coded_height / 2) return EINVAL;
    if (yd != cd || (yd != 0 && yd != 2) ||
        (c->decoder_profile == X9_HEVC_PROFILE_MAIN && yd)) return ENOTSUP;
    c->bit_depth = yd + 8;
    c->crop_x = l * 2; c->crop_y = t * 2;
    c->width = c->coded_width - (l + r) * 2;
    c->height = c->coded_height - (t + bot) * 2;
    return 0;
}
int x9_hevc_config_parse(const uint8_t *p, size_t n, struct x9_hevc_config *c)
{
    if (!p || !c || n < 23 || n > X9_HEVC_MAX_CONFIG || p[0] != 1) return EINVAL;
    memset(c, 0, sizeof(*c));
    if ((p[13] & 0xf0) != 0xf0 || (p[15] & 0xfc) != 0xfc ||
        (p[16] & 0xfc) != 0xfc || (p[17] & 0xf8) != 0xf8 ||
        (p[18] & 0xf8) != 0xf8) return EINVAL;
    c->length_size = (p[21] & 3) + 1;
    if (c->length_size == 3) return ENOTSUP;
    unsigned seen = 0;
    size_t at = 23;
    for (unsigned i = 0; i < p[22]; i++) {
        if (n - at < 3 || (p[at] & 0x40)) return EINVAL;
        unsigned type = p[at++] & 63, count = be16(p + at); at += 2;
        if (type != 32 && type != 33 && type != 34) return ENOTSUP;
        unsigned k = type - 32;
        if (count != 1 || (seen & (1u << k))) return ENOTSUP;
        seen |= 1u << k;
        if (n - at < 2) return EINVAL;
        unsigned bytes = be16(p + at); at += 2;
        if (bytes > n - at || nal(p + at, bytes, type)) return EINVAL;
        c->ps_offset[k] = (uint32_t)at; c->ps_bytes[k] = bytes;
        c->annexb_bytes += bytes + 4;
        at += bytes;
    }
    if (at != n || seen != 7) return EINVAL;
    int e = sps(p + c->ps_offset[1], c->ps_bytes[1], c);
    if (e) return e;
    if ((p[1] >> 6) || (p[1] & 31) != c->profile || p[12] != c->level ||
        (p[16] & 3) != 1 || (p[17] & 7) + 8 != c->bit_depth ||
        (p[18] & 7) + 8 != c->bit_depth) return EINVAL;
    const uint8_t *vp = p + c->ps_offset[0];
    if ((vp[2] >> 4) != c->vps_id) return EINVAL;
    if (c->profile == X9_HEVC_PROFILE_MAIN_STILL) {
        /* Do not infer Main support from profile_idc alone, or from an hvcC
         * label that disagrees with the actual parameter sets. VPS PTL starts
         * after its 32-bit RBSP prefix; read through the emulation-prevention
         * reader rather than indexing escaped bytes. This is still a syntax
         * gate, not validation of the entire VPS or proof of HW support. */
        struct bits v = { .p = vp + 2, .n = c->ps_bytes[0] - 2 };
        unsigned space, profile, level;
        uint32_t compatibility;
        skip(&v, 32);
        space = get(&v, 2); (void)get(&v, 1);
        profile = get(&v, 5);
        compatibility = get(&v, 32);
        skip(&v, 48); level = get(&v, 8);
        if (v.error || space || profile != c->profile || level != c->level) return EINVAL;
        if (!(compatibility & MAIN_COMPATIBILITY) ||
            !(be32(p + 2) & MAIN_COMPATIBILITY)) return ENOTSUP;
    }
    struct bits b = { .p = p + c->ps_offset[2] + 2, .n = c->ps_bytes[2] - 2 };
    c->pps_id = ue(&b); unsigned sid = ue(&b);
    if (b.error || c->pps_id > 63 || sid != c->sps_id) return EINVAL;
    return 0;
}
int x9_hevc_idr_annexb(const struct x9_hevc_config *c, const uint8_t *hvcc,
    const uint8_t *p, size_t n, uint8_t *dst, size_t cap, size_t *written)
{
    if (!c || !hvcc || !p || !written || !n || n > X9_HEVC_MAX_INPUT ||
        (c->length_size != 1 && c->length_size != 2 && c->length_size != 4)) return EINVAL;
    *written = 0;
    size_t at = 0, size = c->annexb_bytes;
    unsigned pictures = 0, vcl = 0, slices = 0, nals = 0;
    /* Validation pass before writing anything, including length overflow. */
    while (at < n) {
        if (++nals > 1024 || n - at < c->length_size) return EINVAL;
        uint32_t bytes = 0;
        for (unsigned j = 0; j < c->length_size; j++) bytes = (bytes << 8) | p[at++];
        if (bytes > n - at || bytes < 3) return EINVAL;
        unsigned type = (p[at] >> 1) & 63;
        if (nal(p + at, bytes, type)) return EINVAL;
        if (type < 32) {
            if (type != 19 && type != 20) return ENOTSUP;
            /* IDR pictures have TemporalId=0 (nuh_temporal_id_plus1=1).
             * A nonzero temporal layer is not an independent IDR fallback. */
            if ((p[at + 1] & 7) != 1) return EINVAL;
            struct bits b = { .p = p + at + 2, .n = bytes - 2 };
            unsigned first = bit(&b); (void)bit(&b);
            unsigned pid = ue(&b);
            if (b.error || pid != c->pps_id || (!slices && !first) ||
                (slices && type != vcl)) return EINVAL;
            pictures += first; slices++; vcl = type;
        } else if (type != 35 && type != 39 && type != 40) return ENOTSUP;
        if (size > X9_HEVC_MAX_INPUT - 4 || bytes > X9_HEVC_MAX_INPUT - size - 4) return E2BIG;
        size += bytes + 4; at += bytes;
    }
    if (pictures != 1) return ENOTSUP;
    *written = size;
    if (!dst) return 0;
    if (cap < size) return EMSGSIZE;
    size = 0;
    for (unsigned i = 0; i < 3; i++) {
        memcpy(dst + size, "\0\0\0\1", 4); size += 4;
        memcpy(dst + size, hvcc + c->ps_offset[i], c->ps_bytes[i]); size += c->ps_bytes[i];
    }
    at = 0;
    while (at < n) {
        uint32_t bytes = 0;
        for (unsigned j = 0; j < c->length_size; j++) bytes = (bytes << 8) | p[at++];
        memcpy(dst + size, "\0\0\0\1", 4); size += 4;
        memcpy(dst + size, p + at, bytes); size += bytes; at += bytes;
    }
    return 0;
}
