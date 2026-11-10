/* Native camera STOP, one attempt only. Neither ioctl0 nor close0 is a grant. */
#ifndef X9_CAMERA_STOP_HELPER_H
#define X9_CAMERA_STOP_HELPER_H
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/ioctl.h>
#include "../kernel/src/bsd/dev/x9_camera.h"
_Static_assert(sizeof(struct x9_camera_stop)==64,"camera stop receipt ABI");

static inline int x9_camera_stop_once(int fd, struct x9_camera_stop *receipt)
{
    const uint32_t states=X9_CAMERA_READY|X9_CAMERA_STREAMING|X9_CAMERA_FAULT|
        X9_CAMERA_PROBED|X9_CAMERA_RETAINED;
    const uint32_t owned=X9_CAM_STOP_POWER|X9_CAM_STOP_CAPTURE|X9_CAM_STOP_STREAM;
    struct x9_camera_status status={0};
    memset(receipt,0,sizeof(*receipt));
    if(ioctl(fd,X9_CAMERA_GET_STATUS,&status)!=0)return errno;
    if(status.version!=1 || status.mode!=2 ||
        (status.sensor!=X9_CAMERA_BACK && status.sensor!=X9_CAMERA_FRONT) ||
        (status.flags&~states) || status.reserved[0] || status.reserved[1])return EPROTO;
    struct x9_camera_stop r={.version=1,.sensor=status.sensor,
        .generation_before=status.generation};
    /* No retry, including ESTALE/EINTR/EALREADY/copyout failure. A request
     * may have changed hardware before its reply failed; close still once. */
    if(ioctl(fd,X9_CAMERA_STOP,&r)!=0)return errno;
    *receipt=r; /* keep raw evidence even when strict validation rejects it */
    if(r.version!=1 || r.sensor!=status.sensor ||
        r.generation_before!=status.generation || r.state_before!=status.flags ||
        ((r.state_before|r.state_after)&~states) || r.reserved[0] || r.reserved[1] ||
        ((r.owned_before|r.owned_after)&~owned) || (r.owned_after&~r.owned_before) ||
        r.readers_after>r.readers_before || r.last_errno>INT_MAX ||
        ((r.state_before&X9_CAMERA_STREAMING) && !(r.owned_before&X9_CAM_STOP_STREAM)))return EPROTO;
    if(r.last_errno) {
        if(r.flags!=(X9_CAM_STOP_RETAINED|X9_CAM_STOP_LATCHED) ||
            (r.state_after&(X9_CAMERA_FAULT|X9_CAMERA_RETAINED))!=
                (X9_CAMERA_FAULT|X9_CAMERA_RETAINED) ||
            (r.generation_after!=r.generation_before &&
             (r.generation_before==UINT64_MAX || r.generation_after!=r.generation_before+1)))return EPROTO;
        return (int)r.last_errno;
    }
    if(r.flags!=(r.owned_before?X9_CAM_STOP_RETIRED:X9_CAM_STOP_IDLE) ||
        r.generation_before==UINT64_MAX || r.generation_after!=r.generation_before+1 ||
        r.owned_after || r.readers_after ||
        (r.state_after&(X9_CAMERA_STREAMING|X9_CAMERA_RETAINED)) ||
        r.state_after!=(r.state_before&~X9_CAMERA_STREAMING))return EPROTO;
    return 0; /* IDLE means no attempted scope; it does not prove DMA ran. */
}
#endif
