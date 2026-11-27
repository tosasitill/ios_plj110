/* Call-voice USIP EMI owner, pure half (L5; see
 * docs/call-voice-audio-chain-20261127.md).
 *
 * The Android owner of the speech EMI region is the kernel mtk-usip
 * driver (OPPO kernel_device_modules-6.12,
 * sound/soc/mediatek/common/mtk-usip.c):
 *   - usip_get_addr() takes the region from CCCI
 *     get_smem_phy_start_addr(0, SMEM_USER_RAW_USIP) -- the LK cacheable
 *     SMEM region this tree carries as id 24 -- and records addr_phy /
 *     memory_size / memory_ready. No address is ever synthesized.
 *   - EMI_TABLE splits the region into the three subranges below; the
 *     audio HAL's SpeechExtMemUSIP mmaps the whole region (usip_mmap:
 *     remap_pfn_range, noncached) and writes speech parameters at
 *     map + 0x4000 inside the AP parameter subrange.
 *   - usip_send_emi_info_to_dsp() publishes the PHONECALL + SMARTPA
 *     tail with one IPI_MSG_A2D_GET_EMI_ADDRESS (AD50) per enabled
 *     scene: scene 0 (TASK_SCENE_PHONE_CALL) always when the ADSP_ENH
 *     config bit is set, scene 19 (TASK_SCENE_PHONE_CALL_SUB) when the
 *     CORE_SEPARATE bit is also set. Payload is two u64
 *     {addr_phy + 0x30000, 0x8000 + 0x28000}; ack is BYPASS_ACK, so a
 *     submitted frame is not a DSP receipt.
 *
 * This header is the host-testable half of the X9 owner: descriptor
 * binding, a reference-counted map lifetime, the subrange table and
 * the AD50 frame construction. It performs no mapping, no IPI
 * submission and no region access itself; the kernel glue in
 * x9_modem.inc supplies ml_io_map_wcomb / ml_io_unmap and is the only
 * place a VA exists. Building an AD50 frame here does not submit it:
 * this tree has no voice IPI transport owner (x9_ipi_dma is
 * SmartPA-only), so frames stay unsubmitted until a call owner with a
 * qualified scene route exists. An address may only be published
 * while the owner is bound, mapped and pinned (qualified). */
#ifndef X9_VOICE_USIP_H
#define X9_VOICE_USIP_H

#include <stdint.h>
#include <string.h>

/* EMI_TABLE from mtk-usip.c: {type, offset, size}; the three subranges
 * exactly tile the 0x60000 region this device's LK table reports. */
#define X9_VOICE_USIP_EXTENT        0x60000u
#define X9_VOICE_USIP_PARAM_OFF     0x00000u   /* AP_USIP_PARAMETER */
#define X9_VOICE_USIP_PARAM_SIZE    0x30000u
#define X9_VOICE_USIP_CALL_OFF      0x30000u   /* ADSP_USIP_PHONECALL */
#define X9_VOICE_USIP_CALL_SIZE     0x08000u
#define X9_VOICE_USIP_SMARTPA_OFF   0x38000u   /* ADSP_USIP_SMARTPA */
#define X9_VOICE_USIP_SMARTPA_SIZE  0x28000u
/* SpeechExtMemUSIP writes speech parameters at map + 0x4000. */
#define X9_VOICE_USIP_PARAM_WRITE_OFF 0x4000u

enum x9_voice_usip_sub { X9V_USIP_PARAM = 0, X9V_USIP_CALL = 1,
                         X9V_USIP_SMARTPA = 2 };

static inline int x9_voice_usip_subrange(enum x9_voice_usip_sub which,
                                         uint32_t *off, uint32_t *size){
 switch (which) {
  case X9V_USIP_PARAM:   *off = X9_VOICE_USIP_PARAM_OFF;
                         *size = X9_VOICE_USIP_PARAM_SIZE;   return 0;
  case X9V_USIP_CALL:    *off = X9_VOICE_USIP_CALL_OFF;
                         *size = X9_VOICE_USIP_CALL_SIZE;    return 0;
  case X9V_USIP_SMARTPA: *off = X9_VOICE_USIP_SMARTPA_OFF;
                         *size = X9_VOICE_USIP_SMARTPA_SIZE; return 0;
 }
 return -1;
}

/* AD50 / IPI_MSG_A2D_GET_EMI_ADDRESS frame (audio_messenger_ipi.h
 * struct ipi_msg_t header + the 16-byte payload above). */
#define X9_VOICE_USIP_AD50_ID       0xad50u
#define X9_VOICE_USIP_AD50_FRAME    32u        /* 16 header + 16 payload */
#define X9_VOICE_USIP_SCENE_MAIN    0u         /* TASK_SCENE_PHONE_CALL */
#define X9_VOICE_USIP_SCENE_SUB     19u        /* TASK_SCENE_PHONE_CALL_SUB */

struct x9_voice_usip {
 uint64_t pa;          /* AP physical base from the LK table (crs) */
 uint32_t size;        /* LK region size */
 uint32_t md_view;     /* MD-view base from the LK table (regs) */
 int bound;            /* descriptor recorded */
 int mapped;           /* glue holds a VA for the region */
 unsigned refs;        /* outstanding pins */
};

static inline void x9_voice_usip_reset(struct x9_voice_usip *o){
 memset(o, 0, sizeof(*o));
}

/* Record the LK descriptor. The region must cover the full EMI table
 * extent; a smaller region cannot carry the Android layout and is
 * rejected rather than partially bound. Rebinding a live owner is an
 * error. Values come only from this boot's LK table, never constants. */
static inline int x9_voice_usip_bind(struct x9_voice_usip *o,
                                     uint64_t pa, uint32_t size,
                                     uint32_t md_view){
 if (!o || o->bound || o->refs || o->mapped) return -1;
 if (!pa || (pa & 0xfffu) || size < X9_VOICE_USIP_EXTENT) return -2;
 o->pa = pa; o->size = size; o->md_view = md_view; o->bound = 1;
 return 0;
}

/* Take a pin. Returns 0 when the region is already mapped (pin added),
 * 1 when the caller must perform the map and report it through
 * x9_voice_usip_map_done(), or a negative error. Only one map request
 * may be outstanding. */
static inline int x9_voice_usip_acquire(struct x9_voice_usip *o){
 if (!o || !o->bound) return -1;
 if (o->mapped) { o->refs++; return 0; }
 if (o->refs) return -2;              /* map already in flight */
 o->refs = 1;
 return 1;
}

/* Report the map attempt for the outstanding pin. Failure rolls the
 * pin back so a later acquire may retry; success marks the region
 * mapped with its first pin held. */
static inline int x9_voice_usip_map_done(struct x9_voice_usip *o, int ok){
 if (!o || !o->bound || o->mapped || o->refs != 1) return -1;
 if (ok) o->mapped = 1; else o->refs = 0;
 return 0;
}

/* Drop a pin. Returns 0 while pins remain, 1 when the last pin was
 * dropped and the caller must unmap and report through
 * x9_voice_usip_unmap_done(), or a negative error. */
static inline int x9_voice_usip_release(struct x9_voice_usip *o){
 if (!o || !o->bound || !o->mapped || !o->refs) return -1;
 if (--o->refs) return 0;
 return 1;
}

static inline int x9_voice_usip_unmap_done(struct x9_voice_usip *o){
 if (!o || !o->bound || !o->mapped || o->refs) return -1;
 o->mapped = 0;
 return 0;
}

/* An EMI address may only be published (AD50) or handed to a consumer
 * while the owner is bound, mapped and pinned. */
static inline int x9_voice_usip_qualified(const struct x9_voice_usip *o){
 return o && o->bound && o->mapped && o->refs > 0;
}

/* Build one AD50 frame (header + payload) for a scene. Requires a
 * qualified owner: publishing an address without a live pin is
 * refused. The frame is only constructed, never submitted here. */
static inline int x9_voice_usip_ad50_build(const struct x9_voice_usip *o,
                                           unsigned scene,
                                           uint8_t out[X9_VOICE_USIP_AD50_FRAME]){
 uint64_t addr, len;
 if (!out) return -1;
 if (!x9_voice_usip_qualified(o)) return -2;
 if (scene != X9_VOICE_USIP_SCENE_MAIN && scene != X9_VOICE_USIP_SCENE_SUB)
  return -3;
 addr = o->pa + X9_VOICE_USIP_CALL_OFF;
 len = (uint64_t)X9_VOICE_USIP_CALL_SIZE + X9_VOICE_USIP_SMARTPA_SIZE;
 memset(out, 0, X9_VOICE_USIP_AD50_FRAME);
 out[0] = 0x88; out[1] = 0x88;          /* IPI_MSG_MAGIC_NUMBER */
 out[2] = (uint8_t)scene;
 out[3] = 0x21;                         /* src FROM_KERNEL=1, dst TO_DSP=2 */
 out[4] = 1;                            /* AUDIO_IPI_PAYLOAD */
 out[5] = 0;                            /* AUDIO_IPI_MSG_BYPASS_ACK */
 out[6] = (uint8_t)(X9_VOICE_USIP_AD50_ID & 0xff);
 out[7] = (uint8_t)(X9_VOICE_USIP_AD50_ID >> 8);
 out[8] = 16;                           /* param1 = payload size */
 /* out[12..15] param2 = 0 */
 for (unsigned i = 0; i < 8; i++) {
  out[16 + i] = (uint8_t)(addr >> (8 * i));
  out[24 + i] = (uint8_t)(len >> (8 * i));
 }
 return 0;
}

#endif
