#ifndef X9_STATUSBAR_GEOMETRY_H
#define X9_STATUSBAR_GEOMETRY_H
#include <math.h>

/* Units: caller's UIKit points; user requested physical/native pixels.
 * Always apply to the frame reset by Apple's original layout, never to a
 * previously shifted frame. Landscape/external/invalid screens are unchanged.
 */
static inline double x9_statusbar_offset(double native_scale, int portrait)
{
    return portrait && isfinite(native_scale) && native_scale >= 1.0 &&
           native_scale <= 4.0 ? 20.0 / native_scale : 0.0;
}
#endif
