#ifndef X9_HEVC_VT_DECODER_H
#define X9_HEVC_VT_DECODER_H
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
/* VTRegisterVideoDecoderWithInfo appends .<codec> to CMClassImplementationID
 * (20H392 0x194cc8138..0x194cc8210). Explicit DecoderID avoids selecting some
 * unrelated hardware codec in the manual client. A synchronous constructor
 * attempts local ordinary registration only with real native capabilities;
 * this explicit function can retry if that initial qualification failed. */
#define X9_HEVC_IMPLEMENTATION "org.ios-sweet.x9.vcp"
#define X9_HEVC_DECODER_ID X9_HEVC_IMPLEMENTATION ".hvc1"
__attribute__((visibility("default"))) OSStatus X9HEVCRegisterDecoder(void);
/* One standalone BLA/IDR/CRA sample, TemporalId=0, through this exact DecoderID
 * + RequireHardware; Main-compatible Main Still Picture remains IDR-only.
 * Creates a fresh persistent native codec with the full parameter-set catalog,
 * submits once and drains (no leading/inter pictures or legacy IDR ioctl).
 * No software fallback. Checks real capabilities at registration and session
 * UsingHardware before/after decode, exactly one callback and linear NV12/P010
 * visible geometry. Caller owns the returned pixel; no transforms applied. */
__attribute__((visibility("default"))) OSStatus X9HEVCCopyHardwarePicture(
    CMSampleBufferRef sample, CVPixelBufferRef *pixel);
#endif
