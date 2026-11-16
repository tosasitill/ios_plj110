/* SPDX-License-Identifier: MIT
 * Read the independent status minor, never acquire or close the exclusive
 * control/RAW vnode to check whether an already active vendor is available.
 */
#ifndef X9_CAMERA_STATUS_H
#define X9_CAMERA_STATUS_H
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "../kernel/src/bsd/dev/x9_camera.h"
static uint32_t x9_camera_backend_mask(void)
{
    int fd = open(X9_CAMERA_STATUS_DEVICE, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    struct x9_camera_status status = {0};
    struct x9_camera_lenses lenses = {0};
    int query = ioctl(fd, X9_CAMERA_GET_STATUS, &status);
    if(!query)query=ioctl(fd,X9_CAMERA_GET_LENSES,&lenses);
    int closed = close(fd);
    uint32_t mask=status.supported_mask;
    if(query || closed || status.version!=1 || status.mode!=2 ||
       !(status.flags&X9_CAMERA_READY) || (status.flags&(X9_CAMERA_FAULT|X9_CAMERA_RETAINED)) ||
       status.cold_retained || lenses.version!=1 || lenses.count!=X9_CAMERA_SENSOR_COUNT ||
       lenses.reserved || lenses.supported_mask!=mask ||
       ((mask&X9_CAMERA_DUAL_MASK)!=X9_CAMERA_DUAL_MASK || (mask&~X9_CAMERA_ALL_MASK)) ||
       status.chip_id[0]!=0x9186 || status.chip_id[1]!=0x0615)return 0;
    const uint32_t expected[X9_CAMERA_SENSOR_COUNT]={0x9186,0x0615,0x38e5,0x8202};
    for(unsigned i=0;i<X9_CAMERA_SENSOR_COUNT;i++) {
        if(lenses.lens[i].sensor!=i+1 || lenses.lens[i].reserved)return 0;
        if((mask&(1u<<(i+1))) &&
           (lenses.lens[i].chip_id!=expected[i] ||
            lenses.lens[i].flags!=(X9_CAMERA_PROBED|X9_CAMERA_READY)))return 0;
        if(!(mask&(1u<<(i+1))) && (lenses.lens[i].flags&X9_CAMERA_READY))return 0;
    }
    return mask;
}
static bool x9_camera_backend_ready(void)
{ return x9_camera_backend_mask()!=0; }

/* Independent status vnode only. Conservatively refuse a new physical lens
 * BEFORE stopping the recording's live stream when pinned histories are full. */
static inline int x9_camera_switch_remaining(uint32_t *remaining)
{
    int fd = open(X9_CAMERA_STATUS_DEVICE, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    struct x9_camera_switch_capacity r = {0};
    int query = ioctl(fd, X9_CAMERA_GET_SWITCH_CAPACITY, &r);
    int closed = close(fd);
    if (query || closed || r.version != 1 || r.flags != X9_CAMERA_SWITCH_SERIAL || r.limit != 8 ||
        r.retired > r.limit || r.remaining != (r.retired + 1 < r.limit ? r.limit - r.retired - 1 : 0) ||
        r.reserved[0] || r.reserved[1] || r.reserved[2]) return -1;
    *remaining = r.remaining;
    return 0;
}
#endif
