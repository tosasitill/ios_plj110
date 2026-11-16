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
        /* All supported UE syntax is <= UINT32_MAX-1. 31 leading zeroes
         * cover that complete domain without ever shifting by 32. */
        if (++n > 31) { b->error = 1; return 0; }
    }
    return b->error ? 0 : ((1u << n) - 1) + get(b, n);
}
static uint32_t bounded_ue(struct bits *b, uint32_t maximum)
{
    uint32_t v = ue(b);
    if (v > maximum) b->error = 1;
    return b->error ? 0 : v;
}
static void bounded_se(struct bits *b, int minimum, int maximum)
{
    uint32_t code = ue(b);
    int64_t value = code & 1u ? ((int64_t)code + 1) / 2 : -(int64_t)(code / 2);
    if (value < minimum || value > maximum) b->error = 1;
}
static void scaling_lists(struct bits *b)
{
    for (unsigned size = 0; size < 4 && !b->error; size++)
        for (unsigned matrix = 0; matrix < 6 && !b->error; matrix += size == 3 ? 3 : 1) {
            if (!bit(b)) (void)bounded_ue(b,size == 3 ? matrix / 3 : matrix);
            else {
                if (size > 1) bounded_se(b,-7,247);
                unsigned count = size ? 64 : 16;
                for (unsigned i = 0; i < count && !b->error; i++) bounded_se(b,-128,127);
            }
        }
}
/* Only the preceding SPS RPS can be referenced by SPS prediction. Keep two
 * small ordered sets, not 64 large tables on the shared kernel stack. */
struct short_rps { int32_t negative[15], positive[15]; unsigned negatives, positives; };
static void rps_insert(struct bits *b, struct short_rps *r, int64_t value)
{
    if (!value || b->error) return; /* shifted zero is not a derived ref pic */
    if (value < INT32_MIN || value > INT32_MAX || r->negatives + r->positives == 15) {
        b->error = 1; return;
    }
    int32_t *list = value < 0 ? r->negative : r->positive;
    unsigned *count = value < 0 ? &r->negatives : &r->positives, i = *count;
    /* Negative POCs nearest zero first; positive POCs increasing. */
    while (i && (value < 0 ? list[i-1] < value : list[i-1] > value)) {
        list[i] = list[i-1]; i--;
    }
    if ((i && list[i-1] == value) || (i < *count && list[i] == value)) {
        b->error = 1; return;
    }
    list[i] = (int32_t)value; (*count)++;
}
static void short_term_sets(struct bits *b, unsigned count)
{
    struct short_rps previous = {0};
    for (unsigned index = 0; index < count && !b->error; index++) {
        struct short_rps next = {0};
        if (index && bit(b)) {
            unsigned sign = bit(b), refs = previous.negatives + previous.positives;
            uint32_t absolute = bounded_ue(b,INT16_MAX);
            int64_t shift = sign ? -(int64_t)(absolute + 1u) : (int64_t)(absolute + 1u);
            unsigned selected = 0;
            for (unsigned j = 0; j <= refs && !b->error; j++) {
                unsigned used = bit(b), use_delta = used ? 1 : bit(b);
                if (!use_delta) continue;
                /* Count includes a selected shift-to-zero entry (syntax
                 * limit); derived NumDeltaPocs excludes that zero entry. */
                if (++selected > 15) { b->error = 1; break; }
                int64_t delta = j < previous.negatives ? previous.negative[j] :
                    j < refs ? previous.positive[j-previous.negatives] : 0;
                rps_insert(b,&next,delta + shift);
            }
        } else {
            unsigned negatives = bounded_ue(b,15);
            unsigned positives = bounded_ue(b,15-negatives);
            int64_t delta = 0;
            for (unsigned j = 0; j < negatives && !b->error; j++) {
                delta -= (int64_t)bounded_ue(b,INT16_MAX) + 1;
                (void)bit(b); rps_insert(b,&next,delta);
            }
            delta = 0;
            for (unsigned j = 0; j < positives && !b->error; j++) {
                delta += (int64_t)bounded_ue(b,INT16_MAX) + 1;
                (void)bit(b); rps_insert(b,&next,delta);
            }
        }
        previous = next;
    }
}
static void hrd_parameters(struct bits *b, unsigned sub)
{
    unsigned nal_hrd = bit(b), vcl_hrd = bit(b), sub_pic = 0;
    if (nal_hrd || vcl_hrd) {
        sub_pic = bit(b);
        if (sub_pic) skip(b,8+5+1+5);
        skip(b,4+4);
        if (sub_pic) skip(b,4);
        skip(b,5+5+5);
    }
    for (unsigned i = 0; i <= sub && !b->error; i++) {
        unsigned general = bit(b), within = general ? 1 : bit(b), low = 0, cpb = 0;
        if (within) (void)bounded_ue(b,2047);
        else low = bit(b);
        if (!low) cpb = bounded_ue(b,31);
        for (unsigned kind = 0; kind < 2 && !b->error; kind++) {
            if (!(kind ? vcl_hrd : nal_hrd)) continue;
            for (unsigned j = 0; j <= cpb && !b->error; j++) {
                (void)ue(b); (void)ue(b); /* rate/size <= UINT32_MAX-1 */
                if (sub_pic) { (void)ue(b); (void)ue(b); }
                (void)bit(b);
            }
        }
    }
}
static void vui_defaults(struct x9_hevc_vui *v)
{
    memset(v,0,sizeof(*v));
    v->video_format = 5;
    v->colour_primaries = v->transfer = v->matrix = 2;
    v->motion_over_boundaries = 1;
    v->max_bytes_per_pic_denom = 2; v->max_bits_per_min_cu_denom = 1;
    v->log2_max_mv_horizontal = v->log2_max_mv_vertical = 15;
}
static void vui_parameters(struct bits *b, unsigned sub, struct x9_hevc_vui *v)
{
    static const uint16_t sar[][2] = { {0,0},{1,1},{12,11},{10,11},{16,11},
        {40,33},{24,11},{20,11},{32,11},{80,33},{18,11},{15,11},{64,33},
        {160,99},{4,3},{3,2},{2,1} };
    v->aspect_ratio_present = (uint8_t)bit(b);
    if (v->aspect_ratio_present) {
        v->aspect_ratio_idc = (uint8_t)get(b,8);
        if (v->aspect_ratio_idc == 255) {
            v->sar_width = get(b,16); v->sar_height = get(b,16);
        } else if (v->aspect_ratio_idc < sizeof(sar)/sizeof(sar[0])) {
            v->sar_width = sar[v->aspect_ratio_idc][0];
            v->sar_height = sar[v->aspect_ratio_idc][1];
        } /* unspecified/reserved idc stays raw, no invented ratio */
    }
    v->overscan_present = (uint8_t)bit(b);
    if (v->overscan_present) v->overscan_appropriate = (uint8_t)bit(b);
    v->video_signal_present = (uint8_t)bit(b);
    if (v->video_signal_present) {
        v->video_format = (uint8_t)get(b,3); v->full_range = (uint8_t)bit(b);
        v->colour_description_present = (uint8_t)bit(b);
        if (v->colour_description_present) {
            v->colour_primaries = (uint8_t)get(b,8); v->transfer = (uint8_t)get(b,8); v->matrix = (uint8_t)get(b,8);
        }
    }
    v->chroma_location_present = (uint8_t)bit(b);
    if (v->chroma_location_present) {
        v->chroma_top = (uint8_t)bounded_ue(b,5); v->chroma_bottom = (uint8_t)bounded_ue(b,5);
    }
    v->neutral_chroma = (uint8_t)bit(b); v->field_seq = (uint8_t)bit(b); v->frame_field_info_present = (uint8_t)bit(b);
    v->display_window_present = (uint8_t)bit(b);
    if (v->display_window_present)
        for (unsigned i = 0; i < 4; i++) v->display_window[i] = bounded_ue(b,16384);
    v->timing_present = (uint8_t)bit(b);
    if (v->timing_present) {
        v->num_units_in_tick = get(b,32); v->time_scale = get(b,32);
        if (!v->num_units_in_tick || !v->time_scale) b->error = 1;
        v->poc_proportional = (uint8_t)bit(b);
        if (v->poc_proportional) v->ticks_poc_diff_one_minus1 = ue(b);
        v->hrd_present = (uint8_t)bit(b);
        if (v->hrd_present) hrd_parameters(b,sub);
    }
    v->restriction_present = (uint8_t)bit(b);
    if (v->restriction_present) {
        v->tiles_fixed = (uint8_t)bit(b); v->motion_over_boundaries = (uint8_t)bit(b); v->restricted_ref_lists = (uint8_t)bit(b);
        v->min_spatial_segmentation = bounded_ue(b,4095);
        v->max_bytes_per_pic_denom = bounded_ue(b,16); v->max_bits_per_min_cu_denom = bounded_ue(b,16);
        v->log2_max_mv_horizontal = bounded_ue(b,16); v->log2_max_mv_vertical = bounded_ue(b,16);
    }
}
static int trailing_bits(struct bits *b)
{
    if (!bit(b)) return EINVAL;
    while (b->left && !b->error) if (bit(b)) return EINVAL;
    return b->error || b->pos != b->n ? EINVAL : 0;
}
static unsigned be16(const uint8_t *p) { return ((unsigned)p[0] << 8) | p[1]; }
static uint32_t be32(const uint8_t *p)
{ return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
#define MAIN_COMPATIBILITY (UINT32_C(1) << (31 - X9_HEVC_PROFILE_MAIN))
static int nal(const uint8_t *p, size_t n, unsigned type)
{
    if (n < 2 || (n < 3 && type != 36 && type != 37) ||
        (p[0] & 0x80) || ((p[0] >> 1) & 63) != type ||
        (p[0] & 1) || (p[1] >> 3) || !(p[1] & 7)) return EINVAL;
    return 0;
}
/* Keep temporary SPS syntax separate from the full catalog. A per-SPS
 * x9_hevc_config would needlessly grow the shared parser's kernel stack. */
struct sps_info {
    uint32_t coded_width, coded_height, width, height, crop_x, crop_y;
    uint32_t profile, bit_depth, level, vps_id, sps_id;
    uint32_t decoder_profile, profile_compatibility;
    uint32_t max_sub_layers, max_dpb, max_reorder;
    uint32_t log2_max_poc_lsb, log2_ctb;
};
static int sps(const uint8_t *p, size_t n, struct sps_info *c, struct x9_hevc_vui *out)
{
    if (nal(p,n,33) || (p[1] & 7) != 1) return EINVAL;
    struct bits b = { .p = p + 2, .n = n - 2 };
    c->vps_id = get(&b, 4);
    unsigned sub = get(&b, 3);
    if (sub > 6) return EINVAL;
    unsigned nesting = bit(&b);
    if (!sub && !nesting) return EINVAL;
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
    unsigned poc = ue(&b);
    if (b.error || poc > 12) return EINVAL;
    c->log2_max_poc_lsb = poc + 4;
    c->max_sub_layers = sub + 1;
    unsigned first = bit(&b) ? 0 : sub;
    for (unsigned i = first; i <= sub; i++) {
        unsigned dpb = ue(&b), reorder = ue(&b);
        (void)ue(&b); /* sps_max_latency_increase_plus1 */
        if (b.error || dpb > 15 || reorder > dpb) return EINVAL;
        if (dpb + 1 > c->max_dpb) c->max_dpb = dpb + 1;
        if (reorder > c->max_reorder) c->max_reorder = reorder;
    }
    unsigned min_cb = bounded_ue(&b,3) + 3;
    unsigned ctb = min_cb + bounded_ue(&b,3);
    if (b.error || ctb > 6 || ctb < 4 || c->coded_width % (1u << min_cb) ||
        c->coded_height % (1u << min_cb)) return EINVAL;
    c->log2_ctb = ctb;
    unsigned min_tb = bounded_ue(&b,min_cb-3) + 2;
    unsigned max_tb = ctb < 5 ? ctb : 5;
    if (b.error || min_tb > max_tb) return EINVAL;
    (void)bounded_ue(&b,max_tb-min_tb);
    (void)bounded_ue(&b,ctb-min_tb); (void)bounded_ue(&b,ctb-min_tb);
    if (bit(&b) && bit(&b)) scaling_lists(&b);
    (void)bit(&b); (void)bit(&b); /* AMP, SAO */
    if (bit(&b)) {
        unsigned ypcm = get(&b,4) + 1, cpcm = get(&b,4) + 1;
        unsigned pcm = bounded_ue(&b,max_tb-3) + 3;
        if (b.error || ypcm > c->bit_depth || cpcm > c->bit_depth ||
            pcm < (min_cb < 5 ? min_cb : 5)) return EINVAL;
        (void)bounded_ue(&b,max_tb-pcm); (void)bit(&b);
    }
    unsigned rps_count = bounded_ue(&b,64);
    if (b.error) return EINVAL;
    short_term_sets(&b,rps_count);
    if (bit(&b)) {
        unsigned count = bounded_ue(&b,32);
        for (unsigned i = 0; i < count && !b.error; i++) skip(&b,poc+4+1);
    }
    (void)bit(&b); (void)bit(&b); /* temporal MVP, strong intra smoothing */
    struct x9_hevc_vui v;
    vui_defaults(&v); v.present = (uint8_t)bit(&b);
    if (v.present) vui_parameters(&b,sub,&v);
    if (b.error) return EINVAL;
    if (bit(&b)) {
        unsigned extensions = get(&b,8);
        if (b.error) return EINVAL;
        /* No multilayer/3D/SCC/Rext/unknown extension decoder support. Do
         * not skip guessed extension lengths and claim a valid Main SPS. */
        if (extensions) return ENOTSUP;
    }
    int e = trailing_bits(&b);
    if (!e && out) { v.sps_id = c->sps_id; v.vps_id = c->vps_id; *out = v; }
    return e;
}
static void apply_sps(struct x9_hevc_config *c, const struct sps_info *s)
{
    c->coded_width = s->coded_width; c->coded_height = s->coded_height;
    c->width = s->width; c->height = s->height;
    c->crop_x = s->crop_x; c->crop_y = s->crop_y;
    c->profile = s->profile; c->bit_depth = s->bit_depth; c->level = s->level;
    c->vps_id = s->vps_id; c->sps_id = s->sps_id;
    c->decoder_profile = s->decoder_profile;
    c->profile_compatibility = s->profile_compatibility;
    c->max_sub_layers = s->max_sub_layers;
    c->max_dpb = s->max_dpb; c->max_reorder = s->max_reorder;
}
static const struct x9_hevc_parameter_set *find_set(const struct x9_hevc_config *c,
                                                   unsigned type, unsigned id)
{
    for (unsigned i = 0; i < c->parameter_set_count; i++)
        if (c->parameter_sets[i].type == type && c->parameter_sets[i].id == id)
            return &c->parameter_sets[i];
    return NULL;
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
    c->config_bytes = (uint32_t)n;
    unsigned seen = 0;
    size_t at = 23;
    for (unsigned i = 0; i < p[22]; i++) {
        if (n - at < 3 || (p[at] & 0x40)) return EINVAL;
        unsigned type = p[at++] & 63, count = be16(p + at); at += 2;
        if (type != 32 && type != 33 && type != 34) return ENOTSUP;
        if (!count) return EINVAL;
        for (unsigned j = 0; j < count; j++) {
            if (n - at < 2) return EINVAL;
            unsigned bytes = be16(p + at); at += 2;
            if (bytes > n - at || nal(p + at, bytes, type)) return EINVAL;
            struct bits b = { .p = p + at + 2, .n = bytes - 2 };
            unsigned id, reference = 0;
            if (type == 32) id = get(&b, 4);
            else if (type == 33) {
                struct sps_info one = {0};
                int e = sps(p + at, bytes, &one,NULL);
                if (e) return e;
                id = one.sps_id; reference = one.vps_id;
            } else {
                id = ue(&b); reference = ue(&b);
            }
            if (b.error || id > (type == 34 ? 63u : 15u) || reference > 15) return EINVAL;
            unsigned k = 0;
            while (k < c->parameter_set_count &&
                (c->parameter_sets[k].type != type || c->parameter_sets[k].id != id)) k++;
            if (k < c->parameter_set_count) {
                /* Exact repeats are harmless. Conflicting definitions of one
                 * ID in the same immutable description are NOT silently
                 * selected by an arbitrary array/last-seen ordering. */
                const struct x9_hevc_parameter_set *old = &c->parameter_sets[k];
                if (old->bytes != bytes || memcmp(p + old->offset, p + at, bytes)) return EINVAL;
            } else {
                if (c->parameter_set_count == X9_HEVC_MAX_PARAMETER_SETS) return E2BIG;
                c->parameter_sets[c->parameter_set_count++] = (struct x9_hevc_parameter_set){
                    .offset = (uint32_t)at, .bytes = bytes, .type = (uint8_t)type,
                    .id = (uint8_t)id, .reference_id = (uint8_t)reference };
                c->annexb_bytes += bytes + 4;
            }
            seen |= 1u << (type - 32); at += bytes;
        }
    }
    if (at != n || seen != 7) return EINVAL;
    /* Stable dependency-order catalog; preserve bytes and order within each
     * NAL type, even if the source hvcC arrays put PPS before SPS/VPS. */
    for (unsigned i = 1; i < c->parameter_set_count; i++) {
        struct x9_hevc_parameter_set set = c->parameter_sets[i];
        unsigned j = i;
        while (j && c->parameter_sets[j - 1].type > set.type) {
            c->parameter_sets[j] = c->parameter_sets[j - 1]; j--;
        }
        c->parameter_sets[j] = set;
    }
    const struct x9_hevc_parameter_set *selected_pps = NULL;
    for (unsigned i = 0; i < c->parameter_set_count; i++) {
        const struct x9_hevc_parameter_set *set = &c->parameter_sets[i];
        if (set->type != 32 && !find_set(c, set->type - 1, set->reference_id)) return EINVAL;
        if (set->type == 34 && !selected_pps) selected_pps = set;
    }
    /* Select the representative SPS from the representative PPS's actual
     * reference, never from which array happens to be first in the hvcC. */
    if (!selected_pps) return EINVAL;
    const struct x9_hevc_parameter_set *selected_sps = find_set(c, 33, selected_pps->reference_id);
    if (!selected_sps) return EINVAL;
    const struct x9_hevc_parameter_set *selected_vps = find_set(c, 32, selected_sps->reference_id);
    if (!selected_vps) return EINVAL;
    struct sps_info representative = {0};
    int e = sps(p + selected_sps->offset, selected_sps->bytes, &representative,NULL);
    if (e) return e;
    apply_sps(c, &representative);
    if ((p[1] >> 6) || (p[1] & 31) != c->profile || p[12] != c->level ||
        (p[16] & 3) != 1 || (p[17] & 7) + 8 != c->bit_depth ||
        (p[18] & 7) + 8 != c->bit_depth) return EINVAL;
    for (unsigned i = 0; i < c->parameter_set_count; i++) {
        const struct x9_hevc_parameter_set *set = &c->parameter_sets[i];
        if (set->type == 33) {
            struct sps_info one = {0};
            e = sps(p + set->offset, set->bytes, &one,NULL);
            if (e) return e;
            if (one.coded_width != c->coded_width || one.coded_height != c->coded_height ||
                one.width != c->width || one.height != c->height ||
                one.crop_x != c->crop_x || one.crop_y != c->crop_y ||
                one.profile != c->profile || one.bit_depth != c->bit_depth ||
                one.level != c->level || one.max_sub_layers != c->max_sub_layers ||
                one.decoder_profile != c->decoder_profile) return ENOTSUP;
            if (one.max_dpb > c->max_dpb) c->max_dpb = one.max_dpb;
            if (one.max_reorder > c->max_reorder) c->max_reorder = one.max_reorder;
        }
        if (set->type != 32) continue;
        if (c->profile != X9_HEVC_PROFILE_MAIN_STILL) continue;
        /* Do not infer Main support from profile_idc alone, or from an hvcC
         * label that disagrees with the actual parameter sets. VPS PTL starts
         * after its 32-bit RBSP prefix; read through the emulation-prevention
         * reader rather than indexing escaped bytes. This is still a syntax
         * gate, not validation of the entire VPS or proof of HW support. */
        struct bits v = { .p = p + set->offset + 2, .n = set->bytes - 2 };
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
    c->pps_id = selected_pps->id;
    const struct x9_hevc_parameter_set *selected[3] = {selected_vps, selected_sps, selected_pps};
    for (unsigned i = 0; i < 3; i++) {
        c->ps_offset[i] = selected[i]->offset;
        c->ps_bytes[i] = selected[i]->bytes;
    }
    return 0;
}
static int catalog_valid(const struct x9_hevc_config *c)
{
    if (!c || c->config_bytes < 23 || c->config_bytes > X9_HEVC_MAX_CONFIG ||
        !c->parameter_set_count || c->parameter_set_count > X9_HEVC_MAX_PARAMETER_SETS) return EINVAL;
    uint32_t bytes = 0;
    unsigned seen = 0;
    for (unsigned i = 0; i < c->parameter_set_count; i++) {
        const struct x9_hevc_parameter_set *set = &c->parameter_sets[i];
        if (set->type < 32 || set->type > 34 || set->id > (set->type == 34 ? 63u : 15u) ||
            set->reserved || set->reference_id > 15 || (set->type == 32 && set->reference_id) ||
            set->offset < 23 || set->bytes < 3 || set->bytes > UINT16_MAX ||
            set->offset > c->config_bytes || set->bytes > c->config_bytes - set->offset ||
            (i && c->parameter_sets[i - 1].type > set->type)) return EINVAL;
        if (set->type != 32 && !find_set(c, set->type - 1, set->reference_id)) return EINVAL;
        for (unsigned j = 0; j < i; j++)
            if (c->parameter_sets[j].type == set->type && c->parameter_sets[j].id == set->id) return EINVAL;
        bytes += set->bytes + 4; seen |= 1u << (set->type - 32);
    }
    return seen == 7 && bytes == c->annexb_bytes ? 0 : EINVAL;
}
int x9_hevc_idr_annexb(const struct x9_hevc_config *c, const uint8_t *hvcc,
    const uint8_t *p, size_t n, uint8_t *dst, size_t cap, size_t *written)
{
    if (!c || !hvcc || !p || !written || !n || n > X9_HEVC_MAX_INPUT || catalog_valid(c) ||
        (c->length_size != 1 && c->length_size != 2 && c->length_size != 4)) return EINVAL;
    *written = 0;
    size_t at = 0, size = c->annexb_bytes;
    unsigned pictures = 0, vcl = 0, slices = 0, nals = 0, picture_pps = 0;
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
            if (b.error || !find_set(c, 34, pid) || (!slices && !first) ||
                (slices && (type != vcl || pid != picture_pps))) return EINVAL;
            picture_pps = pid;
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
    for (unsigned i = 0; i < c->parameter_set_count; i++) {
        const struct x9_hevc_parameter_set *set = &c->parameter_sets[i];
        memcpy(dst + size, "\0\0\0\1", 4); size += 4;
        memcpy(dst + size, hvcc + set->offset, set->bytes); size += set->bytes;
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
int x9_hevc_parameter_sets_annexb(const struct x9_hevc_config *c, const uint8_t *hvcc,
    uint8_t *dst, size_t cap, size_t *written)
{
    if (!c || !hvcc || !written || catalog_valid(c)) return EINVAL;
    *written = c->annexb_bytes;
    if (!dst) return 0;
    if (cap < c->annexb_bytes) return EMSGSIZE;
    size_t at = 0;
    for (unsigned i = 0; i < c->parameter_set_count; i++) {
        const struct x9_hevc_parameter_set *set = &c->parameter_sets[i];
        memcpy(dst + at, "\0\0\0\1", 4); at += 4;
        memcpy(dst + at, hvcc + set->offset, set->bytes); at += set->bytes;
    }
    return 0;
}
struct slice_prefix {
    struct sps_info sps;
    unsigned dependent_enabled, output_present, extra_bits, address_bits, ctbs;
};
struct slice_facts { unsigned dependent, type, output, poc_lsb; };
static int slice_prefix(const struct x9_hevc_config *c, const uint8_t *hvcc,
    unsigned pid, struct slice_prefix *out)
{
    const struct x9_hevc_parameter_set *pps = find_set(c,34,pid);
    const struct x9_hevc_parameter_set *selected = pps ? find_set(c,33,pps->reference_id) : NULL;
    if (!selected) return EINVAL;
    struct slice_prefix prefix = {0};
    int e = sps(hvcc+selected->offset,selected->bytes,&prefix.sps,NULL);
    if (e) return e;
    if (prefix.sps.sps_id != selected->id || prefix.sps.vps_id != selected->reference_id)
        return EINVAL;
    struct bits b = { .p = hvcc+pps->offset+2, .n = pps->bytes-2 };
    unsigned pps_id = ue(&b), sps_id = ue(&b);
    prefix.dependent_enabled = bit(&b); prefix.output_present = bit(&b);
    prefix.extra_bits = get(&b,3);
    if (b.error || pps_id != pid || sps_id != selected->id) return EINVAL;
    unsigned ctb = 1u << prefix.sps.log2_ctb;
    prefix.ctbs = ((prefix.sps.coded_width+ctb-1)/ctb) *
        ((prefix.sps.coded_height+ctb-1)/ctb);
    for (unsigned n = prefix.ctbs-1; n; n >>= 1) prefix.address_bits++;
    *out = prefix; return 0;
}
/* b is positioned just AFTER slice_pic_parameter_set_id. The accepted SPS
 * has chroma_format_idc=1, so no separate_colour_plane_flag/header field.
 * Dependent segments inherit their independent segment's facts; their
 * coded data must never be mistaken for another slice header. */
static int slice_facts(struct bits *b, unsigned nal_type, unsigned first,
    const struct slice_prefix *prefix, struct slice_facts *out)
{
    struct slice_facts facts = {0};
    if (!first) {
        if (prefix->dependent_enabled) facts.dependent = bit(b);
        unsigned address = get(b,prefix->address_bits);
        if (!address || address >= prefix->ctbs) return EINVAL;
    }
    if (!facts.dependent) {
        skip(b,prefix->extra_bits); /* reserved fields are not slice_type */
        facts.type = bounded_ue(b,2);
        facts.output = prefix->output_present ? bit(b) : 1;
        if (nal_type != 19 && nal_type != 20)
            facts.poc_lsb = get(b,prefix->sps.log2_max_poc_lsb);
        if (nal_type >= 16 && facts.type != 2) return EINVAL;
    }
    if (b->error) return EINVAL;
    *out = facts; return 0;
}
static int au_annexb(const struct x9_hevc_config *c, const uint8_t *hvcc,
    const uint8_t *p, size_t n, uint8_t *dst, size_t cap, size_t *written,
    int independent, struct x9_hevc_irap_picture *picture,
    struct x9_hevc_au_metadata *metadata)
{
    if (!c || !hvcc || !p || !written || !n || n > X9_HEVC_MAX_INPUT || catalog_valid(c) ||
        (c->length_size != 1 && c->length_size != 2 && c->length_size != 4)) return EINVAL;
    *written = 0;
    size_t at = 0, size = 0;
    unsigned pictures = 0, slices = 0, vcl = 0, temporal = 0, nals = 0, picture_pps = 0;
    /* Stateful AU ordering follows 7.4.2.4.4, including legal mid-picture
     * prefix/suffix NALs. The standalone gate retains its narrower ordering.
     * A prefix after a VCL must be followed by another VCL in THIS AU. */
    unsigned phase = 0, no_output = 0; /* prefix0, VCL1, suffix2, EOS3, EOB4 */
    unsigned prefix_pending = 0, eos = 0, eob = 0;
    struct slice_prefix selected = {0};
    struct slice_facts first_facts = {0};
    while (at < n) {
        if (++nals > 1024 || n - at < c->length_size) return EINVAL;
        uint32_t bytes = 0;
        for (unsigned j = 0; j < c->length_size; j++) bytes = (bytes << 8) | p[at++];
        if (bytes > n - at || bytes < 2) return EINVAL;
        unsigned type = (p[at] >> 1) & 63, tid = (p[at + 1] & 7) - 1;
        if (nal(p + at, bytes, type)) return EINVAL;
        if (phase == 4 || (phase == 3 && type != 37)) return EINVAL;
        if (type < 32) {
            if (type > 21 || (type > 9 && type < 16)) return ENOTSUP;
            if (independent && type < 16) return ENOTSUP;
            if (independent && phase > 1) return EINVAL;
            if (tid >= c->max_sub_layers || (type >= 16 && tid)) return EINVAL;
            if (c->profile == X9_HEVC_PROFILE_MAIN_STILL && type != 19 && type != 20) return ENOTSUP;
            struct bits b = { .p = p + at + 2, .n = bytes - 2 };
            unsigned first = bit(&b);
            unsigned prior = type >= 16 ? bit(&b) : 0;
            unsigned pid = ue(&b);
            if (b.error || !find_set(c, 34, pid) || (!slices && !first) ||
                (slices && (type != vcl || tid != temporal || pid != picture_pps ||
                 prior != no_output))) return EINVAL;
            if (slices && first) return ENOTSUP; /* exactly one picture */
            int e = !slices ? slice_prefix(c,hvcc,pid,&selected) : 0;
            struct slice_facts facts;
            if (!e) e = slice_facts(&b,type,first,&selected,&facts);
            if (e) return e;
            if (!slices) first_facts = facts;
            else if (!facts.dependent && (facts.output != first_facts.output ||
                facts.poc_lsb != first_facts.poc_lsb)) return EINVAL;
            picture_pps = pid;
            pictures += first; slices++; vcl = type; temporal = tid;
            no_output = prior; phase = 1; prefix_pending = 0;
        } else if (type >= 32 && type <= 34) {
            unsigned i = 0;
            while (i < c->parameter_set_count && (c->parameter_sets[i].type != type ||
                c->parameter_sets[i].bytes != bytes ||
                memcmp(p + at, hvcc + c->parameter_sets[i].offset, bytes))) i++;
            if (i == c->parameter_set_count) return ENOTSUP;
            if (slices) return EINVAL; /* next-picture sets cannot follow VCL */
        } else if (type == 35) {
            if (slices || nals != 1) return EINVAL;
        } else if (type == 39) {
            if (independent && phase) return EINVAL;
            if (slices) prefix_pending = 1;
        } else if (type == 40 || type == 38) {
            if (!slices) return EINVAL;
            phase = 2;
        } else if (type == 36 || type == 37) {
            unsigned next = type == 36 ? 3u : 4u;
            if (!slices || phase >= next) return EINVAL;
            /* 7.3.2.6/.7: empty RBSP, NOT rbsp_trailing_bits(). Only the
             * two-byte NAL header is present; 7.4.2.2 requires TemporalId0. */
            if (tid || bytes != 2) return EINVAL;
            phase = next;
            if (type == 36) eos = 1; else eob = 1;
        } else return ENOTSUP;
        if (size > X9_HEVC_MAX_INPUT - 4 || bytes > X9_HEVC_MAX_INPUT - size - 4) return E2BIG;
        size += bytes + 4; at += bytes;
    }
    if (pictures != 1) return ENOTSUP;
    if (prefix_pending) return EINVAL;
    *written = size;
    if (picture) *picture = (struct x9_hevc_irap_picture){ .nal_type = vcl,
        .pps_id = picture_pps, .no_output_of_prior_pics_flag = no_output };
    if (metadata) *metadata = (struct x9_hevc_au_metadata){ .nal_type = vcl,
        .temporal_id = temporal, .pps_id = picture_pps,
        .no_output_of_prior_pics_flag = no_output, .eos_present = eos, .eob_present = eob,
        .log2_max_poc_lsb = selected.sps.log2_max_poc_lsb, .poc_lsb = first_facts.poc_lsb,
        .pic_output_flag = first_facts.output, .first_slice_type = first_facts.type };
    if (!dst) return 0;
    if (cap < size) return EMSGSIZE;
    at = 0; size = 0;
    while (at < n) {
        uint32_t bytes = 0;
        for (unsigned j = 0; j < c->length_size; j++) bytes = (bytes << 8) | p[at++];
        memcpy(dst + size, "\0\0\0\1", 4); size += 4;
        memcpy(dst + size, p + at, bytes); size += bytes; at += bytes;
    }
    return 0;
}
int x9_hevc_au_annexb(const struct x9_hevc_config *c, const uint8_t *hvcc,
    const uint8_t *p, size_t n, uint8_t *dst, size_t cap, size_t *written)
{ return au_annexb(c,hvcc,p,n,dst,cap,written,0,NULL,NULL); }
int x9_hevc_irap_annexb(const struct x9_hevc_config *c, const uint8_t *hvcc,
    const uint8_t *p, size_t n, uint8_t *dst, size_t cap, size_t *written,
    struct x9_hevc_irap_picture *picture)
{
    if (picture) memset(picture,0,sizeof(*picture));
    if (!written) return EINVAL;
    *written = 0;
    size_t bytes = 0;
    struct x9_hevc_irap_picture parsed = {0};
    int e = au_annexb(c,hvcc,p,n,dst,cap,&bytes,1,&parsed,NULL);
    if (!e) {
        *written = bytes;
        if (picture) *picture = parsed;
    }
    return e;
}
int x9_hevc_sample_vui(const struct x9_hevc_config *c, const uint8_t *hvcc,
    size_t hvcc_bytes, const uint8_t *sample, size_t sample_bytes, struct x9_hevc_vui *out)
{
    if (!out) return EINVAL;
    memset(out,0,sizeof(*out));
    if (!c || hvcc_bytes != c->config_bytes) return EINVAL;
    size_t needed = 0;
    int e = x9_hevc_au_annexb(c,hvcc,sample,sample_bytes,NULL,0,&needed);
    if (e) return e;
    /* The full AU gate above already proved lengths, one picture and a
     * single retained PPS for every slice. No partial header-only success. */
    size_t at = 0;
    while (at < sample_bytes) {
        uint32_t bytes = 0;
        for (unsigned j = 0; j < c->length_size; j++) bytes = (bytes << 8) | sample[at++];
        unsigned type = (sample[at] >> 1) & 63;
        if (type < 32) {
            struct bits b = { .p = sample + at + 2, .n = bytes - 2 };
            (void)bit(&b);
            if (type >= 16) (void)bit(&b);
            unsigned id = ue(&b);
            const struct x9_hevc_parameter_set *pps = find_set(c,34,id);
            const struct x9_hevc_parameter_set *selected = pps ? find_set(c,33,pps->reference_id) : NULL;
            if (b.error || !selected) return EINVAL;
            struct sps_info info = {0};
            struct x9_hevc_vui v;
            e = sps(hvcc+selected->offset,selected->bytes,&info,&v);
            if (e) return e;
            if (info.sps_id != selected->id || info.vps_id != selected->reference_id) return EINVAL;
            v.pps_id = id; *out = v;
            return 0;
        }
        at += bytes;
    }
    return EINVAL;
}
/* SEI payloadSize counts RBSP bytes, not EBSP bytes. One shared escape reader
 * plus a logical bit cursor bounds each child independently of its parent.
 * No RBSP heap, recursive nesting, or changes to the SPS bit reader. */
struct sei_reader { struct bits b; uint64_t used; unsigned messages; };
struct sei_context {
    const struct x9_hevc_config *config;
    const uint8_t *hvcc;
    const struct x9_hevc_hdr *before;
    struct x9_hevc_hdr *hdr;
    const struct x9_hevc_alpha *alpha_before;
    struct x9_hevc_alpha *alpha;
    unsigned *counts;
    unsigned pps_id, temporal, nal_temporal, prefix, after_vcl;
};
static uint32_t sei_get(struct sei_reader *r, unsigned n, uint64_t end)
{
    if (r->used > end || n > end-r->used) { r->b.error = 1; return 0; }
    r->used += n;
    return get(&r->b,n);
}
static uint32_t sei_ue(struct sei_reader *r, uint32_t maximum, uint64_t end)
{
    unsigned zeroes = 0;
    while (!r->b.error && !sei_get(r,1,end))
        if (++zeroes > 31) { r->b.error = 1; return 0; }
    uint32_t value = r->b.error ? 0 : ((1u << zeroes)-1) + sei_get(r,zeroes,end);
    if (value > maximum) r->b.error = 1;
    return r->b.error ? 0 : value;
}
static int sei_header(struct sei_reader *r, uint64_t end, uint32_t *type, uint32_t *bytes)
{
    if (++r->messages > 1024) return E2BIG;
    uint32_t *fields[] = {type,bytes};
    for (unsigned i = 0; i < 2; i++) {
        uint32_t value; *fields[i] = 0;
        do {
            value = sei_get(r,8,end);
            if (r->b.error || value > UINT32_MAX-*fields[i]) return EINVAL;
            *fields[i] += value;
        } while (value == 255);
    }
    return r->used > end || (uint64_t)*bytes*8 > end-r->used ? EINVAL : 0;
}
static unsigned sei_vcl_associated(uint32_t type)
{
    if ((type >= 134 && type <= 152) || (type >= 154 && type <= 159) ||
        (type >= 200 && type <= 202) || type == 205) return 1;
    switch (type) {
    case 2: case 3: case 6: case 9: case 15: case 16: case 17: case 19:
    case 22: case 23: case 45: case 47: case 56: case 128: case 131: case 132:
    case 165: return 1; /* Annex F F.14.3.1, including scalable-nested alpha */
    default: return 0;
    }
}
/* Query OpLayerIdList base membership in the ACTUAL selected VPS. Only its
 * prefix through layer sets is needed here; this is not a complete VPS/HRD/
 * extension conformance checker or a multilayer decoder capability claim. */
static int sei_vps_base_scope(const struct sei_context *s, const uint32_t selected[32],
    unsigned largest, unsigned *base)
{
    const struct x9_hevc_parameter_set *pps = find_set(s->config,34,s->pps_id);
    const struct x9_hevc_parameter_set *sp = pps ? find_set(s->config,33,pps->reference_id) : NULL;
    const struct x9_hevc_parameter_set *vp = sp ? find_set(s->config,32,sp->reference_id) : NULL;
    if (!vp) return EINVAL;
    struct bits b = { .p = s->hvcc+vp->offset+2, .n = vp->bytes-2 };
    if (get(&b,4) != vp->id) return EINVAL;
    skip(&b,2+6); /* internal/available/max layers do not change set membership */
    unsigned sub = get(&b,3), nesting = bit(&b);
    if (sub > 6 || (!sub && !nesting)) return EINVAL;
    skip(&b,16); /* reserved_0xffff: decoders shall ignore its value */
    skip(&b,96); /* general profile_tier_level(1): profile88 + level8 */
    unsigned pp[7] = {0}, lp[7] = {0};
    for (unsigned i = 0; i < sub; i++) { pp[i] = bit(&b); lp[i] = bit(&b); }
    if (sub) for (unsigned i = sub; i < 8; i++) (void)get(&b,2);
    for (unsigned i = 0; i < sub; i++) {
        if (pp[i]) skip(&b,88);
        if (lp[i]) skip(&b,8);
    }
    unsigned all = bit(&b);
    for (unsigned i = all ? 0 : sub; i <= sub && !b.error; i++) {
        (void)ue(&b); (void)ue(&b); (void)ue(&b);
    }
    unsigned max_layer = get(&b,6), sets = bounded_ue(&b,1023);
    if (b.error || largest > sets) return EINVAL; /* this SEI NAL has layer0 */
    *base = selected[0] & 1u; /* normative LayerSetLayerIdList[0]={0} */
    for (unsigned i = 1; i <= sets && !b.error; i++)
        for (unsigned j = 0; j <= max_layer && !b.error; j++) {
            unsigned included = bit(&b);
            if (!j && included && (selected[i/32] & (1u << (i%32)))) *base = 1;
        }
    return b.error ? EINVAL : 0;
}
static int alpha_equal(const struct x9_hevc_alpha *a, const struct x9_hevc_alpha *b)
{
    /* Scope provenance may differ for agreeing direct/nested repetitions. */
    return a->present == b->present && a->cancel == b->cancel && a->use_idc == b->use_idc &&
        a->bit_depth_minus8 == b->bit_depth_minus8 && a->transparent == b->transparent &&
        a->opaque == b->opaque && a->incr == b->incr && a->clip == b->clip && a->clip_type == b->clip_type;
}
static int sei_alpha(struct sei_reader *r, uint64_t end, const struct sei_context *s,
    unsigned nested, unsigned apply, unsigned source)
{
    struct x9_hevc_alpha alpha = { .present = 1, .sources = source };
    alpha.cancel = sei_get(r,1,end);
    if (!alpha.cancel) {
        alpha.use_idc = sei_get(r,3,end); /* 3..7 reserved; do not invent a convention */
        alpha.bit_depth_minus8 = sei_get(r,3,end);
        unsigned width = alpha.bit_depth_minus8+9;
        alpha.transparent = sei_get(r,width,end);
        alpha.opaque = sei_get(r,width,end);
        alpha.incr = sei_get(r,1,end);
        alpha.clip = sei_get(r,1,end);
        if (alpha.clip) alpha.clip_type = sei_get(r,1,end);
        uint32_t range = alpha.opaque >= alpha.transparent ? alpha.opaque-alpha.transparent :
            alpha.transparent-alpha.opaque;
        /* F.14.3.8: incr requires a nonzero power-of-two endpoint distance.
         * No max=(1<<bit_depth)-1 test: the wire scalars have ONE extra bit.
         * Equal endpoints and reversed endpoints remain legal raw syntax. */
        if (alpha.incr && (!range || (range & (range-1)))) return EINVAL;
    }
    if (r->b.error) return EINVAL;
    /* D.2.1/D.3.1: a partial-byte payload needs its one/zero tail. Reserved
     * extension data, including extra bytes, must be ignored by decoders.
     * The last one bit of the final payload byte is the delimiter; at most
     * seven trailing zero bits follow it. Never borrow the next SEI header
     * or NAL rbsp_trailing_bits to complete a truncated scalar or payload. */
    if (r->used < end) {
        unsigned last = 0;
        while (r->used < end && !r->b.error) {
            unsigned n = end-r->used < 8 ? (unsigned)(end-r->used) : 8;
            last = ((last << n) | sei_get(r,n,end)) & 255u;
        }
        if (!last) return EINVAL;
    }
    if (r->b.error || r->used != end) return EINVAL;
    if (!apply) return 0; /* validate non-base syntax, never promote its facts */
    if (++s->counts[4] > 8 || (!nested && s->nal_temporal != s->temporal) ||
        (s->after_vcl && !s->alpha_before->present) ||
        (s->alpha->present && !alpha_equal(s->alpha,&alpha))) return EINVAL;
    unsigned sources = s->alpha->sources | source;
    *s->alpha = alpha; s->alpha->sources = sources;
    return 0;
}
static int sei_payload(struct sei_reader *r, uint32_t type, uint32_t bytes, uint64_t end,
    const struct sei_context *s, unsigned nested, unsigned apply, unsigned source)
{
    if (nested && (type == 129 || type == 132 || type == 133)) return EINVAL;
    /* Suffix165 is reserved_sei_message(), NOT alpha_channel_info(). */
    if (s->prefix && type == 165) return sei_alpha(r,end,s,nested,apply,source);
    /* D.2.1 suffix137/144/147/148 are reserved payloads, not these messages. */
    unsigned mastering = s->prefix && type == 137, light = s->prefix && type == 144;
    unsigned ambient = s->prefix && type == 148, preferred = s->prefix && type == 147;
    if ((mastering && bytes != 24) || (light && bytes != 4) || (ambient && bytes != 8) ||
        (preferred && bytes != 1)) return EINVAL;
    if (apply && (mastering || light || ambient || preferred)) {
        if ((mastering && ++s->counts[0] > 8) || (light && ++s->counts[1] > 8) ||
            (ambient && ++s->counts[2] > 8) || (preferred && ++s->counts[3] > 8)) return EINVAL;
        if ((!nested && s->nal_temporal != s->temporal) ||
            (s->after_vcl && ((mastering && !s->before->mastering_present) ||
                             (light && !s->before->light_present) ||
                             (ambient && !s->before->ambient_present) ||
                             (preferred && !s->before->preferred_present)))) return EINVAL;
    }
    uint8_t raw[24];
    for (uint32_t i = 0; i < bytes && !r->b.error; i++) {
        uint32_t value = sei_get(r,8,end);
        if (mastering || light || ambient || preferred) raw[i] = (uint8_t)value;
    }
    if (r->b.error || r->used != end) return EINVAL;
    /* D.3.39: original big-endian units stay unchanged. Even a non-base
     * scoped payload must have conforming values, not only a valid length. */
    if (ambient) {
        struct x9_hevc_hdr check = { .ambient_present = 1 };
        memcpy(check.ambient,raw,8);
        if (x9_hevc_hdr_validate(&check)) return EINVAL;
    }
    if (!apply) return 0;
    if (mastering) {
        if (s->hdr->mastering_present && memcmp(s->hdr->mastering,raw,24)) return EINVAL;
        memcpy(s->hdr->mastering,raw,24); s->hdr->mastering_present = 1;
        s->hdr->mastering_sources |= source;
    } else if (light) {
        if (s->hdr->light_present && memcmp(s->hdr->light,raw,4)) return EINVAL;
        memcpy(s->hdr->light,raw,4); s->hdr->light_present = 1;
        s->hdr->light_sources |= source;
    } else if (ambient) {
        if (s->hdr->ambient_present && memcmp(s->hdr->ambient,raw,8)) return EINVAL;
        memcpy(s->hdr->ambient,raw,8); s->hdr->ambient_present = 1;
        s->hdr->ambient_sources |= source;
    } else if (preferred) {
        if (s->hdr->preferred_present && s->hdr->preferred_transfer != raw[0]) return EINVAL;
        s->hdr->preferred_transfer = raw[0]; s->hdr->preferred_present = 1;
        s->hdr->preferred_sources |= source;
    }
    return 0;
}
static int sei_scalable(struct sei_reader *r, uint64_t end, const struct sei_context *s,
    unsigned *timing)
{
    unsigned subset = sei_get(r,1,end), ops = sei_get(r,1,end), base = 0, full_temporal = 1;
    if (ops) {
        uint32_t selected[32] = {0}; /* 1024 normative operation indices */
        unsigned def = sei_get(r,1,end), num = sei_ue(r,1023,end), largest = 0, explicit_ops = 0;
        if (def) { base = 1; full_temporal = s->nal_temporal == 6; }
        for (unsigned i = def; i <= num && !r->b.error; i++) {
            unsigned cap = sei_get(r,3,end), index = sei_ue(r,1023,end);
            if (cap < s->nal_temporal+1) return EINVAL;
            if (cap != 7) full_temporal = 0;
            selected[index/32] |= 1u << (index%32); explicit_ops = 1;
            if (index > largest) largest = index;
        }
        if (r->b.error) return EINVAL;
        if (explicit_ops) {
            unsigned selected_base = 0;
            int e = sei_vps_base_scope(s,selected,largest,&selected_base);
            if (e) return e;
            base |= selected_base;
        }
    } else {
        unsigned all = sei_get(r,1,end);
        if (all) base = 1; /* current AU contains only layer0 */
        else {
            unsigned cap = sei_get(r,3,end), num = sei_ue(r,63,end), previous = 0;
            if (!cap) return EINVAL;
            full_temporal = cap == 7;
            for (unsigned i = 0; i <= num && !r->b.error; i++) {
                unsigned id = sei_get(r,6,end);
                if (i && id <= previous) return EINVAL;
                if (!id) base = 1;
                previous = id;
            }
        }
    }
    while ((r->used & 7) && !r->b.error) if (sei_get(r,1,end)) return EINVAL;
    if (r->b.error || r->used == end) return EINVAL; /* one or more children */
    unsigned other = 0; *timing = 0;
    while (r->used < end && !r->b.error) {
        uint32_t type, bytes;
        int e = sei_header(r,end,&type,&bytes);
        if (e) return e;
        unsigned is_timing = type == 0 || type == 1 || type == 130;
        if (is_timing) *timing = 1; else other = 1;
        if ((*timing && other) || (is_timing && !subset) ||
            ((sei_vcl_associated(type) || type == 129) && subset) ||
            (sei_vcl_associated(type) && (s->nal_temporal || !full_temporal))) return EINVAL;
        uint64_t payload_end = r->used+(uint64_t)bytes*8;
        e = sei_payload(r,type,bytes,payload_end,s,1,base,
            ops ? X9_HEVC_HDR_SCALABLE_OPERATIONS : X9_HEVC_HDR_SCALABLE_LAYERS);
        if (e) return e;
    }
    return r->b.error || r->used != end ? EINVAL : 0;
}
static int sei_hdr(const uint8_t *p, size_t n, const struct x9_hevc_config *config,
    const uint8_t *hvcc, unsigned pps_id, unsigned temporal, unsigned after_vcl,
    const struct x9_hevc_hdr *before, struct x9_hevc_hdr *hdr,
    const struct x9_hevc_alpha *alpha_before, struct x9_hevc_alpha *alpha, unsigned counts[5])
{
    struct sei_reader r = { .b = { .p = p+2, .n = n-2 } };
    const struct sei_context s = { .config = config, .hvcc = hvcc, .pps_id = pps_id,
        .before = before, .hdr = hdr, .alpha_before = alpha_before, .alpha = alpha,
        .counts = counts, .temporal = temporal,
        .nal_temporal = (p[1]&7)-1u, .prefix = ((p[0]>>1)&63) == 39, .after_vcl = after_vcl };
    uint64_t limit = (uint64_t)(n-2)*8; /* EBSP upper bound, not a payload bound */
    unsigned last = 0, direct_timing = 0, nested_timing = 0, other = 0, active = 0;
    while (!r.b.error && r.b.pos < r.b.n) {
        /* Only an 0x80 at the final HEADER boundary is rbsp_trailing_bits;
         * within a sized parent/child payload it remains ordinary data. */
        if (r.b.n-r.b.pos == 1 && r.b.p[r.b.pos] == 0x80)
            return r.messages ? trailing_bits(&r.b) : EINVAL;
        if (last) return EINVAL; /* D.2.24 nesting consumes the last SEI payload */
        uint32_t type, bytes;
        int e = sei_header(&r,limit,&type,&bytes);
        if (e) return e;
        uint64_t end = r.used+(uint64_t)bytes*8;
        if (s.prefix && type == 133) {
            unsigned timing = 0;
            e = sei_scalable(&r,end,&s,&timing); last = 1;
            if (timing) nested_timing = 1;
        } else {
            if (s.prefix && (type == 0 || type == 1 || type == 130)) direct_timing = 1;
            else other = 1;
            if (s.prefix && type == 129) active = 1;
            e = sei_payload(&r,type,bytes,end,&s,0,1,X9_HEVC_HDR_DIRECT);
        }
        if (e) return e;
        if (s.prefix && ((active && r.messages != 1) ||
            (direct_timing && (other || last)) || (nested_timing && other))) return EINVAL;
    }
    return EINVAL;
}
int x9_hevc_sample_metadata(const struct x9_hevc_config *c, const uint8_t *hvcc,
    size_t hvcc_bytes, const uint8_t *sample, size_t sample_bytes,
    struct x9_hevc_au_metadata *out)
{
    if (!out) return EINVAL;
    memset(out,0,sizeof(*out));
    if (!c || hvcc_bytes != c->config_bytes) return EINVAL;
    size_t needed = 0;
    struct x9_hevc_au_metadata metadata = {0};
    int e = au_annexb(c,hvcc,sample,sample_bytes,NULL,0,&needed,0,NULL,&metadata);
    if (e) return e;
    struct x9_hevc_hdr hdr = {0};
    struct x9_hevc_hdr before = {0};
    struct x9_hevc_alpha alpha = {0}, alpha_before = {0};
    unsigned vcl_seen = 0;
    unsigned counts[5] = {0};
    size_t at = 0;
    while (at < sample_bytes) {
        uint32_t bytes = 0;
        for (unsigned j = 0; j < c->length_size; j++) bytes = (bytes << 8) | sample[at++];
        unsigned type = (sample[at] >> 1) & 63;
        if (type < 32) vcl_seen = 1;
        if (type == 39 || type == 40) {
            e = sei_hdr(sample+at,bytes,c,hvcc,metadata.pps_id,
                metadata.temporal_id,vcl_seen,&before,&hdr,&alpha_before,&alpha,counts);
            if (e) return e;
            if (!vcl_seen) { before = hdr; alpha_before = alpha; }
        }
        at += bytes;
    }
    metadata.hdr = hdr;
    metadata.alpha = alpha;
    *out = metadata; return 0;
}
int x9_hevc_sample_hdr(const struct x9_hevc_config *c, const uint8_t *hvcc,
    size_t hvcc_bytes, const uint8_t *sample, size_t sample_bytes, struct x9_hevc_hdr *out)
{
    if (!out) return EINVAL;
    memset(out,0,sizeof(*out));
    struct x9_hevc_au_metadata metadata;
    int e = x9_hevc_sample_metadata(c,hvcc,hvcc_bytes,sample,sample_bytes,&metadata);
    /* Still-picture/HEIF callers consume only HDR and have no associated
     * alpha carrier or output-order state. Do not silently drop SEI165 or
     * infer opaque pixels/cancel success from its presence or raw use_idc. */
    if (!e && metadata.alpha.present) e = ENOTSUP;
    if (!e) *out = metadata.hdr;
    return e;
}
int x9_hevc_hdr_validate(const struct x9_hevc_hdr *hdr)
{
    if (!hdr || hdr->mastering_present > 1 || hdr->light_present > 1 || hdr->ambient_present > 1 ||
        hdr->preferred_present > 1 || hdr->preferred_transfer > 255 ||
        ((hdr->mastering_sources | hdr->light_sources | hdr->ambient_sources | hdr->preferred_sources) & ~X9_HEVC_HDR_SOURCES) ||
        (!hdr->mastering_present && hdr->mastering_sources) ||
        (!hdr->light_present && hdr->light_sources) || (!hdr->ambient_present && hdr->ambient_sources) ||
        (!hdr->preferred_present && (hdr->preferred_sources || hdr->preferred_transfer)))
        return EINVAL;
    if (hdr->ambient_present && (!(hdr->ambient[0] | hdr->ambient[1] | hdr->ambient[2] | hdr->ambient[3]) ||
        (((unsigned)hdr->ambient[4] << 8) | hdr->ambient[5]) > 50000 ||
        (((unsigned)hdr->ambient[6] << 8) | hdr->ambient[7]) > 50000)) return EINVAL;
    return 0;
}
int x9_hevc_poc_sequence_next(const struct x9_hevc_poc_sequence *current,
    const struct x9_hevc_au_metadata *picture, struct x9_hevc_poc_sequence *out,
    struct x9_hevc_poc_picture *result)
{
    if (!out || !result || (void *)out == (void *)result) return EINVAL;
    struct x9_hevc_poc_sequence previous = {0};
    struct x9_hevc_au_metadata facts = {0};
    if (current) previous = *current;
    if (picture) facts = *picture;
    memset(out,0,sizeof(*out)); memset(result,0,sizeof(*result));
    if (!current || !picture || previous.started > 1 || previous.prev_valid > 1 ||
        previous.eos_present > 1 || previous.eob_present > 1 || previous.eob_present ||
        previous.associated_irap_no_rasl_output > 1 ||
        previous.started != !!previous.clvs || previous.started != previous.prev_valid ||
        (!previous.started && (previous.eos_present || previous.prev_lsb || previous.prev_msb ||
            previous.associated_irap_no_rasl_output)) ||
        previous.prev_lsb > 65535 || previous.prev_msb < (int64_t)INT32_MIN-65535 ||
        previous.prev_msb > INT32_MAX ||
        previous.prev_msb+previous.prev_lsb < INT32_MIN ||
        previous.prev_msb+previous.prev_lsb > INT32_MAX ||
        facts.nal_type > 21 || (facts.nal_type > 9 && facts.nal_type < 16) ||
        facts.temporal_id > 6 || (facts.nal_type >= 16 && facts.temporal_id) ||
        facts.log2_max_poc_lsb < 4 || facts.log2_max_poc_lsb > 16 ||
        facts.pic_output_flag > 1 || facts.first_slice_type > 2 ||
        facts.no_output_of_prior_pics_flag > 1 || facts.eos_present > 1 || facts.eob_present > 1)
        return EINVAL;
    unsigned maximum = 1u << facts.log2_max_poc_lsb;
    if (facts.poc_lsb >= maximum ||
        ((facts.nal_type == 19 || facts.nal_type == 20) && facts.poc_lsb)) return EINVAL;
    unsigned irap = facts.nal_type >= 16;
    if ((!previous.started || previous.eos_present) && !irap) return ENOTSUP;
    /* 8.1.3: IDR/BLA, bitstream first IRAP or IRAP after EOS. A middle CRA
     * does not reset POC, even if no_output_of_prior_pics_flag is set. */
    unsigned boundary = !previous.started || previous.eos_present ||
        (irap && facts.nal_type <= 20);
    if (boundary && previous.clvs == UINT64_MAX) return E2BIG;
    int64_t msb = boundary ? 0 : previous.prev_msb;
    if (!boundary) {
        if (facts.poc_lsb < previous.prev_lsb &&
            previous.prev_lsb-facts.poc_lsb >= maximum/2) msb += maximum;
        else if (facts.poc_lsb > previous.prev_lsb &&
            facts.poc_lsb-previous.prev_lsb > maximum/2) msb -= maximum;
    }
    int64_t poc = msb + facts.poc_lsb;
    if (poc < INT32_MIN || poc > INT32_MAX) return EINVAL;
    struct x9_hevc_poc_sequence next = boundary ? (struct x9_hevc_poc_sequence){0} : previous;
    /* 8.3.1: exclude RADL6/7, RASL8/9 and the *_N types0/2/4/6/8. */
    if (!facts.temporal_id && !(facts.nal_type >= 6 && facts.nal_type <= 9) &&
        !(facts.nal_type < 16 && !(facts.nal_type & 1))) {
        next.prev_msb = msb; next.prev_lsb = facts.poc_lsb; next.prev_valid = 1;
    }
    next.clvs = previous.clvs + boundary; next.started = 1;
    next.eos_present = facts.eos_present; next.eob_present = facts.eob_present;
    /* 3.3 associates to the previous IRAP in DECODING order, not a previous
     * displayed picture or only the CLVS-start IRAP. A middle CRA replaces
     * the association with NoRaslOutputFlag0 even though CLVS/POC persist.
     * Keep this prospective until SUBMIT accepts this picture. */
    if (irap) next.associated_irap_no_rasl_output = boundary;
    unsigned rasl = facts.nal_type == 8 || facts.nal_type == 9;
    unsigned output = facts.pic_output_flag &&
        !(rasl && next.associated_irap_no_rasl_output);
    *out = next;
    *result = (struct x9_hevc_poc_picture){ .value = (int32_t)poc,
        .no_rasl_output_flag = irap && boundary, .pic_output_flag = output,
        .clvs = next.clvs };
    return 0;
}
int x9_hevc_hdr_sequence_next(const struct x9_hevc_hdr_sequence *current,
    const struct x9_hevc_au_metadata *picture, struct x9_hevc_hdr_sequence *out)
{
    if (!out) return EINVAL;
    /* Permit current == out, but still clear output on every failure. */
    struct x9_hevc_hdr_sequence previous = {0};
    if (current) previous = *current;
    memset(out,0,sizeof(*out));
    if (!current || !picture || previous.started > 1 || previous.eos_present > 1 ||
        previous.eob_present > 1 || previous.started != !!previous.clvs ||
        x9_hevc_hdr_validate(&previous.hdr) || x9_hevc_hdr_validate(&picture->hdr) ||
        picture->eos_present > 1 || picture->eob_present > 1 ||
        picture->nal_type > 21 || (picture->nal_type > 9 && picture->nal_type < 16) ||
        picture->temporal_id > 6 || (picture->nal_type >= 16 && picture->temporal_id) ||
        picture->no_output_of_prior_pics_flag > 1 || previous.eob_present) return EINVAL;
    unsigned irap = picture->nal_type >= 16;
    if ((!previous.started || previous.eos_present) && !irap) return ENOTSUP;
    /* 3.31 / 8.1, base layer, no external HandleCraAsBlaFlag. A CRA in the
     * middle of a CVS does NOT clear state, regardless of its slice no_output.
     * DRAIN is absent from this API and cannot manufacture a CVS boundary. */
    unsigned new_clvs = !previous.started || previous.eos_present ||
        (picture->nal_type >= 16 && picture->nal_type <= 20);
    if (new_clvs && previous.clvs == UINT64_MAX) return E2BIG;
    struct x9_hevc_hdr_sequence next = new_clvs ?
        (struct x9_hevc_hdr_sequence){0} : previous;
    if (picture->hdr.mastering_present) {
        if ((!new_clvs && !next.hdr.mastering_present) || (next.hdr.mastering_present &&
            memcmp(next.hdr.mastering,picture->hdr.mastering,24))) return EINVAL;
        memcpy(next.hdr.mastering,picture->hdr.mastering,24); next.hdr.mastering_present = 1;
        next.hdr.mastering_sources |= picture->hdr.mastering_sources;
    }
    if (picture->hdr.light_present) {
        if (next.hdr.light_present && memcmp(next.hdr.light,picture->hdr.light,4)) return EINVAL;
        memcpy(next.hdr.light,picture->hdr.light,4); next.hdr.light_present = 1;
        next.hdr.light_sources |= picture->hdr.light_sources;
    }
    if (picture->hdr.ambient_present) {
        /* D.3.39 shares mastering's first-IRAP/persistent/exact rules, not
         * CLLI's unrestricted late arrival. No retroactive ambient plan. */
        if ((!new_clvs && !next.hdr.ambient_present) || (next.hdr.ambient_present &&
            memcmp(next.hdr.ambient,picture->hdr.ambient,8))) return EINVAL;
        memcpy(next.hdr.ambient,picture->hdr.ambient,8); next.hdr.ambient_present = 1;
        next.hdr.ambient_sources |= picture->hdr.ambient_sources;
    }
    if (picture->hdr.preferred_present) {
        if ((!new_clvs && !next.hdr.preferred_present) || (next.hdr.preferred_present &&
            next.hdr.preferred_transfer != picture->hdr.preferred_transfer)) return EINVAL;
        next.hdr.preferred_transfer = picture->hdr.preferred_transfer; next.hdr.preferred_present = 1;
        next.hdr.preferred_sources |= picture->hdr.preferred_sources;
    }
    next.clvs = previous.clvs + new_clvs;
    next.started = 1; next.eos_present = picture->eos_present; next.eob_present = picture->eob_present;
    *out = next; return 0;
}
