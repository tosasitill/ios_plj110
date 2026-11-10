/* MTK audio_ipi wire codec, documented by OPPO audio_messenger_ipi.h and
 * audio_playback_msg_id.h. Explicit bytes avoid C bitfield/padding ABI bugs.
 * This is a codec, not ADSP boot, mailbox ownership, or a ready indication. */
#ifndef X9_AUDIO_DSP_H
#define X9_AUDIO_DSP_H
#include <stdint.h>
#define X9_DSP_HEADER 16u
#define X9_DSP_PAYLOAD_MAX 96u
#define X9_DSP_MESSAGE_MAX 112u
#define X9_DSP_DMA_INFO_BYTES 56u /* reserve16, hal16, rw_idx4, size4, wb16 */
enum { X9DSPOnly, X9DSPPayload, X9DSPDMA };
enum { X9DSPBypass, X9DSPNeedAck, X9DSPAckBack, X9DSPDirect };
enum {
    X9DSPOpen = 1, X9DSPClose = 2, X9DSPPrepare = 3, X9DSPHWParam = 4,
    X9DSPRingShare = 8, X9DSPHWFree = 9, X9DSPDLCopy = 10,
    X9DSPStart = 15, X9DSPStop = 16
};
struct x9_dsp_message {
    uint8_t scene, source, target, type, ack;
    uint16_t id;
    uint32_t param1, param2;
    uint8_t payload[X9_DSP_PAYLOAD_MAX];
};
#ifdef __cplusplus
extern "C" {
#endif
/* Return 0 or errno. DMA encoding admits ONLY the region-index form:
 * legacy opaque dma_addr in reserve[0..7], rest of reserve/hal/writeback
 * zero, 128B-aligned rw_idx, data_size=param1>1. The legacy token is NOT a
 * DMA address or a pointer that decode may dereference.
 * Decode preserves the 56B byte descriptor, never dereferences addresses.
 * Encoding/decoding grants NO physical-region or DMA-send authorization. */
int x9_dsp_encode(const struct x9_dsp_message *, void *, uint32_t *length);
int x9_dsp_decode(const void *, uint32_t length, struct x9_dsp_message *);
/* Match ACK against the one outstanding serialized request. No invented
 * transaction token: MTK matches task/message, so timeout must quarantine
 * that mailbox session before another identical request can be issued. */
int x9_dsp_ack_matches(const struct x9_dsp_message *request,
                       const struct x9_dsp_message *reply);
#ifdef __cplusplus
}
#endif
#endif
