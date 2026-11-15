#ifndef X9_HEVC_VT_DECODER_H
#define X9_HEVC_VT_DECODER_H
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
/* VTRegisterVideoDecoderWithInfo appends .<codec> to CMClassImplementationID
 * (20H392 0x194cc8138..0x194cc8210). Explicit DecoderID avoids selecting some
 * unrelated hardware codec in the manual client. No automatic registration. */
#define X9_HEVC_IMPLEMENTATION "org.ios-sweet.x9.vcp"
#define X9_HEVC_DECODER_ID X9_HEVC_IMPLEMENTATION ".hvc1"
__attribute__((visibility("default"))) OSStatus X9HEVCRegisterDecoder(void);
/* One independent IDR sample through this exact DecoderID + RequireHardware.
 * No software fallback. Checks real capabilities at registration and session
 * UsingHardware before/after decode, exactly one callback and linear NV12/P010
 * visible geometry. Caller owns the returned pixel; no transforms applied. */
__attribute__((visibility("default"))) OSStatus X9HEVCCopyHardwarePicture(
    CMSampleBufferRef sample, CVPixelBufferRef *pixel);
#endif
