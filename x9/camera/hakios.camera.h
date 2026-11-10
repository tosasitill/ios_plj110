/* SPDX-License-Identifier: MIT
 * HakiOS 63fc2b4 userland import, X9 adaptation 2026. See LICENSE.hakios.
 */
/* HakiOS MIT RAW10/command layout, X9 semantics:
 * open: exclusive owner, no automatic power-up. ioctl GET_STATUS works when
 *       platform is absent. write SENSOR selects/starts the physical sensor;
 *       read: header-only peek or exact header+stride*height, bounded 1s wait.
 *       last close stops/drains/restores camera-owned resources.
 * EXPOSURE gain is linear Q10, translated to Sony reciprocal encoding;
 * KEEP/FOCUS are unsupported. No synthetic or host-uploaded frames.
 */
#ifndef _SYS_HAKIOS_CAMERA_H_
#define _SYS_HAKIOS_CAMERA_H_

#include <stdint.h>

#define HAKIOS_CAMERA_DEVICE           "/dev/x9-camera"
#define HAKIOS_CAMERA_MAGIC            "RWFRAME2"

struct hakios_camera_frame {
	char            magic[8];       /* HAKIOS_CAMERA_MAGIC */
	uint32_t        header_bytes;   /* this structure's size: the frame's bytes follow */
	uint32_t        width, height;  /* pixels */
	uint32_t        stride;         /* bytes from one line to the next */
	uint32_t        format;         /* 1: MIPI RAW10, four pixels in five bytes */
	uint32_t        bayer;          /* the order of a 2x2 cell: 0 RGGB, 1 GRBG, 2 GBRG, 3 BGGR */
	uint32_t        black;          /* the black level, in 10-bit steps */
	uint64_t        sequence;       /* the frame's number since the sensor started, from 1 */
	uint64_t        time;           /* when the frame was done, in mach_absolute_time()'s units */
	uint32_t        exposure_lines; /* the exposure it was taken with, in lines ... */
	uint32_t        line_ns;        /* ... of this many nanoseconds */
	uint32_t        gain;           /* linear analog gain in Q10 ... */
	uint32_t        gain_unity;     /* ... which is this for 1x */
	uint32_t        gain_max;       /* maximum linear Q10 gain */
	uint32_t        frame_lines;    /* the frame's length in lines (frames a second: 1e9 / (frame_lines * line_ns)) */
	uint32_t        frame_lines_min; /* the mode's own length: the shortest frame */
	uint32_t        frame_lines_max; /* the longest frame it may be given */
	uint32_t        margin;         /* exposure_lines is at most frame_lines - margin */
	uint32_t        sensor;         /* which: 0 the macro, 1 the wide, 2 the front, 3 the ultra-wide camera */
	uint32_t        focus;          /* the lens's position as last set, 0 (the furthest) to 1023: HAKIOS_CAMERA_NO_FOCUS where it
	                                 * has no motor (or it does not answer) */
	/* X9 v2 uses three former reserved words, keeping the 120B wire layout.
	 * The explicit magic rejects DONE-only v1; no stale-header fallback.
	 * Split words avoid an unaligned uint64_t at offset100. SOF is observed
	 * by the sole IRQ owner for this frame, NOT the physical exposure start. */
	uint32_t        sof_time_lo, sof_time_hi, timing_flags;
	uint32_t        reserved[2];
};

#define HAKIOS_CAMERA_TIMING_SOF 1u
static inline uint64_t hakios_camera_sof_time(const struct hakios_camera_frame *f)
{
	return (uint64_t)f->sof_time_hi << 32 | f->sof_time_lo;
}

#define HAKIOS_CAMERA_NO_FOCUS 0xffffffffu

enum {
	HAKIOS_CAMERA_EXPOSURE = 1,    /* a: lines, b: gain, c: frame lines (0: the shortest that holds the exposure) */
	HAKIOS_CAMERA_KEEP = 2,        /* a picture for the host: a width, b height, c stride, d format (1 the frame's RAW10, 2 NV12,
	                                 * 3 BGRA); bytes follow */
	HAKIOS_CAMERA_SENSOR = 3,      /* another sensor: a which (as in the frames' header), b its mode (~0: the usual one);
	                                 * the frames stop, everything goes down, and the next read starts the other one */
	HAKIOS_CAMERA_FOCUS = 4,       /* the lens to a: 0 (the furthest) to 1023, from the next frame or so */
};

struct hakios_camera_command {
	uint32_t        command;
	uint32_t        bytes;          /* of what follows this structure */
	uint32_t        a, b, c, d;
};

#endif /* _SYS_HAKIOS_CAMERA_H_ */
