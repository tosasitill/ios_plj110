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
static bool x9_camera_backend_ready(void)
{
    int fd = open(X9_CAMERA_STATUS_DEVICE, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    struct x9_camera_status status = {0};
    int query = ioctl(fd, X9_CAMERA_GET_STATUS, &status);
    int closed = close(fd);
    return !query && !closed && status.version == 1 && status.mode == 2 &&
        (status.flags & X9_CAMERA_READY) && !(status.flags & (X9_CAMERA_FAULT | X9_CAMERA_RETAINED)) &&
        status.supported_mask == ((1u << X9_CAMERA_BACK) | (1u << X9_CAMERA_FRONT)) &&
        status.chip_id[0] == 0x9186 && status.chip_id[1] == 0x0615 && !status.cold_retained;
}
#endif
