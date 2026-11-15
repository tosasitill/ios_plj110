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
    if (n < 3 || (p[0] & 0x80) || ((p[0] >> 1) & 63) != type ||
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
static int au_annexb(const struct x9_hevc_config *c, const uint8_t *hvcc,
    const uint8_t *p, size_t n, uint8_t *dst, size_t cap, size_t *written,
    int independent, struct x9_hevc_irap_picture *picture)
{
    if (!c || !hvcc || !p || !written || !n || n > X9_HEVC_MAX_INPUT || catalog_valid(c) ||
        (c->length_size != 1 && c->length_size != 2 && c->length_size != 4)) return EINVAL;
    *written = 0;
    size_t at = 0, size = 0;
    unsigned pictures = 0, slices = 0, vcl = 0, temporal = 0, nals = 0, picture_pps = 0;
    /* Only the standalone gate uses phase/flag equality. Do not silently
     * tighten the existing stateful AU API or alter the legacy IDR ioctl. */
    unsigned phase = 0, no_output = 0; /* prefix0, VCL1, suffix2, EOS3, EOB4 */
    while (at < n) {
        if (++nals > 1024 || n - at < c->length_size) return EINVAL;
        uint32_t bytes = 0;
        for (unsigned j = 0; j < c->length_size; j++) bytes = (bytes << 8) | p[at++];
        if (bytes > n - at || bytes < 3) return EINVAL;
        unsigned type = (p[at] >> 1) & 63, tid = (p[at + 1] & 7) - 1;
        if (nal(p + at, bytes, type)) return EINVAL;
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
                 (independent && prior != no_output)))) return EINVAL;
            picture_pps = pid;
            pictures += first; slices++; vcl = type; temporal = tid;
            no_output = prior; phase = 1;
        } else if (type >= 32 && type <= 34) {
            unsigned i = 0;
            while (i < c->parameter_set_count && (c->parameter_sets[i].type != type ||
                c->parameter_sets[i].bytes != bytes ||
                memcmp(p + at, hvcc + c->parameter_sets[i].offset, bytes))) i++;
            if (i == c->parameter_set_count) return ENOTSUP;
            if (slices) return EINVAL; /* next-picture sets cannot follow VCL */
        } else if (type == 35) {
            if (slices || (independent && nals != 1)) return EINVAL;
        } else if (type == 39) {
            if (independent && phase) return EINVAL;
        } else if (type == 40 || type == 38) {
            if (independent) {
                if (!phase || phase >= 3) return EINVAL;
                phase = 2;
            }
        } else if (type == 36 || type == 37) {
            if (independent) {
                unsigned next = type == 36 ? 3u : 4u;
                if (!phase || phase >= next) return EINVAL;
                phase = next;
            }
        } else return ENOTSUP;
        if (size > X9_HEVC_MAX_INPUT - 4 || bytes > X9_HEVC_MAX_INPUT - size - 4) return E2BIG;
        size += bytes + 4; at += bytes;
    }
    if (pictures != 1) return ENOTSUP;
    *written = size;
    if (picture) *picture = (struct x9_hevc_irap_picture){ .nal_type = vcl,
        .pps_id = picture_pps, .no_output_of_prior_pics_flag = no_output };
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
{ return au_annexb(c,hvcc,p,n,dst,cap,written,0,NULL); }
int x9_hevc_irap_annexb(const struct x9_hevc_config *c, const uint8_t *hvcc,
    const uint8_t *p, size_t n, uint8_t *dst, size_t cap, size_t *written,
    struct x9_hevc_irap_picture *picture)
{
    if (picture) memset(picture,0,sizeof(*picture));
    if (!written) return EINVAL;
    *written = 0;
    size_t bytes = 0;
    struct x9_hevc_irap_picture parsed = {0};
    int e = au_annexb(c,hvcc,p,n,dst,cap,&bytes,1,&parsed);
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
static int sei_hdr(const uint8_t *p, size_t n, struct x9_hevc_hdr *hdr)
{
    struct bits b = { .p = p + 2, .n = n - 2 };
    unsigned messages = 0;
    /* SEI headers and payloads are byte aligned. Only the final 0x80 at a
     * HEADER boundary is rbsp_trailing_bits; an 0x80 within a sized payload
     * remains ordinary data. The existing bit reader validates canonical
     * EBSP escapes even for unknown payloads, with no temporary RBSP heap. */
    while (!b.error && b.pos < b.n) {
        if (b.n-b.pos == 1 && b.p[b.pos] == 0x80)
            return messages ? trailing_bits(&b) : EINVAL;
        if (++messages > 1024) return E2BIG;
        uint32_t type = 0, bytes = 0, value;
        do {
            value = get(&b,8);
            if (b.error || value > UINT32_MAX-type) return EINVAL;
            type += value;
        } while (value == 255);
        do {
            value = get(&b,8);
            if (b.error || value > UINT32_MAX-bytes) return EINVAL;
            bytes += value;
        } while (value == 255);
        if (bytes > b.n-b.pos) return EINVAL;
        if (type == 133) return ENOTSUP; /* scalable_nesting scope not guessed */
        uint8_t raw[24];
        if ((type == 137 && bytes != 24) || (type == 144 && bytes != 4)) return EINVAL;
        /* This still-image resolver only interprets direct prefix/Tid0
         * HDR. Other placement/temporal scope is unsupported, not claimed
         * malformed without resolving the applicable SEI semantics. */
        if ((type == 137 || type == 144) &&
            (((p[0] >> 1) & 63) != 39 || (p[1] & 7) != 1)) return ENOTSUP;
        for (uint32_t i = 0; i < bytes && !b.error; i++) {
            value = get(&b,8);
            if (type == 137 || type == 144) raw[i] = (uint8_t)value;
        }
        if (b.error) return EINVAL;
        if (type == 137) {
            if (hdr->mastering_present && memcmp(hdr->mastering,raw,24)) return EINVAL;
            memcpy(hdr->mastering,raw,24); hdr->mastering_present = 1;
        } else if (type == 144) {
            if (hdr->light_present && memcmp(hdr->light,raw,4)) return EINVAL;
            memcpy(hdr->light,raw,4); hdr->light_present = 1;
        }
    }
    return EINVAL; /* the final rbsp_trailing_bits byte is required */
}
int x9_hevc_sample_hdr(const struct x9_hevc_config *c, const uint8_t *hvcc,
    size_t hvcc_bytes, const uint8_t *sample, size_t sample_bytes, struct x9_hevc_hdr *out)
{
    if (!out) return EINVAL;
    memset(out,0,sizeof(*out));
    if (!c || hvcc_bytes != c->config_bytes) return EINVAL;
    size_t needed = 0;
    int e = x9_hevc_au_annexb(c,hvcc,sample,sample_bytes,NULL,0,&needed);
    if (e) return e;
    struct x9_hevc_hdr hdr = {0};
    size_t at = 0;
    while (at < sample_bytes) {
        uint32_t bytes = 0;
        for (unsigned j = 0; j < c->length_size; j++) bytes = (bytes << 8) | sample[at++];
        unsigned type = (sample[at] >> 1) & 63;
        if (type == 39 || type == 40) {
            e = sei_hdr(sample+at,bytes,&hdr);
            if (e) return e;
        }
        at += bytes;
    }
    *out = hdr; return 0;
}
