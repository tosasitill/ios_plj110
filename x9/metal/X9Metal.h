#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
NS_ASSUME_NONNULL_BEGIN
/* Explicit opt-in device factory for the built-in native add_u32 pipeline.
 * No global swizzle, system default registration, source/AIR compiler or CPU
 * compute fallback. Unknown functions/libraries/encodings fail explicitly. */
FOUNDATION_EXPORT id<MTLDevice> _Nullable X9MetalCreateDevice(NSError **error);
NS_ASSUME_NONNULL_END
