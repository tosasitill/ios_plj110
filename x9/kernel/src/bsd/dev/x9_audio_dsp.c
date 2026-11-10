#include "x9_audio_dsp.h"
#if defined(KERNEL)
#include <sys/systm.h>
#include <sys/errno.h>
#else
#include <string.h>
#include <errno.h>
#endif
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t get32(const uint8_t *p)
{ return get16(p) | ((uint32_t)get16(p + 2) << 16); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }
int x9_dsp_encode(const struct x9_dsp_message *m, void *out, uint32_t *length)
{
    if (!m || !out || !length || m->source > 2 || m->target > 2 || !m->id ||
        m->ack > X9DSPDirect || m->type > X9DSPDMA) return EINVAL;
    if (m->type == X9DSPDMA) {
        if(m->param1<=1 || !(get32(m->payload)|get32(m->payload+4)) ||
           get32(m->payload+36)!=m->param1 ||
           (get32(m->payload+32)&127u))return EINVAL;
        /* HAL and writeback descriptors remain unsupported, even if the
         * caller supplies a plausible physical address. */
        for(unsigned i=0;i<X9_DSP_DMA_INFO_BYTES;i++)
            if(((i>=8 && i<32) || i>=40) && m->payload[i])return ENOTSUP;
    }
    if (m->type == X9DSPPayload && m->param1 > X9_DSP_PAYLOAD_MAX) return EMSGSIZE;
    uint32_t n = X9_DSP_HEADER + (m->type == X9DSPPayload ? m->param1 :
                                 m->type == X9DSPDMA ? X9_DSP_DMA_INFO_BYTES : 0);
    if (*length < n) { *length = n; return EMSGSIZE; }
    uint8_t *p = out;
    memset(p, 0, n); put16(p, 0x8888); p[2] = m->scene;
    p[3] = (uint8_t)(m->source | (m->target << 4)); p[4] = m->type; p[5] = m->ack;
    put16(p + 6, m->id); put32(p + 8, m->param1); put32(p + 12, m->param2);
    if (n > X9_DSP_HEADER) memcpy(p + X9_DSP_HEADER, m->payload, n - X9_DSP_HEADER);
    *length = n; return 0;
}
int x9_dsp_decode(const void *in, uint32_t n, struct x9_dsp_message *out)
{
    if (!in || !out || n < X9_DSP_HEADER || n > X9_DSP_MESSAGE_MAX) return EINVAL;
    const uint8_t *p = in;
    if (get16(p) != 0x8888 || (p[3] & 15) > 2 || (p[3] >> 4) > 2 ||
        p[4] > X9DSPDMA || p[5] > X9DSPDirect || !get16(p + 6)) return EINVAL;
    uint32_t size = p[4] == X9DSPPayload ? get32(p + 8) :
                    p[4] == X9DSPDMA ? X9_DSP_DMA_INFO_BYTES : 0;
    if (size > X9_DSP_PAYLOAD_MAX || n != X9_DSP_HEADER + size) return EMSGSIZE;
    struct x9_dsp_message m;
    memset(&m, 0, sizeof(m));
    m.scene = p[2]; m.source = p[3] & 15; m.target = p[3] >> 4;
    m.type = p[4]; m.ack = p[5]; m.id = get16(p + 6);
    m.param1 = get32(p + 8); m.param2 = get32(p + 12);
    memcpy(m.payload, p + X9_DSP_HEADER, size); *out = m; return 0;
}
int x9_dsp_ack_matches(const struct x9_dsp_message *a, const struct x9_dsp_message *b)
{
    return a && b && a->ack == X9DSPNeedAck && b->ack == X9DSPAckBack &&
        a->scene == b->scene && a->id == b->id &&
        a->source == b->target && a->target == b->source;
}
