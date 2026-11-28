#ifndef X9_STATUSBAR_GEOMETRY_H
#define X9_STATUSBAR_GEOMETRY_H
#include <math.h>

/* Units: caller's UIKit points; user requested physical/native pixels.
 * Always apply to the frame reset by Apple's original layout, never to a
 * previously shifted frame. Landscape/external/invalid screens are unchanged.
 * Total shift 50 native px: the original 20 px plus the 10 pt the user
 * asked to move down on 2026-11-28; at this panel's @3x native scale
 * 10 pt == 30 native px exactly (20 + 30 = 50 px == 16.667 pt).
 */
static inline double x9_statusbar_offset(double native_scale, int portrait)
{
    return portrait && isfinite(native_scale) && native_scale >= 1.0 &&
           native_scale <= 4.0 ? 50.0 / native_scale : 0.0;
}
#endif
