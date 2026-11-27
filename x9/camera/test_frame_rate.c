/* Host test for isp_frame_rate() (camera_isp.c): the frame-length window a
 * rate contract admits. Build and run on the host:
 *   cc -Ix9/camera -o /tmp/test_frame_rate \
 *      x9/camera/test_frame_rate.c x9/camera/camera_isp.c -lm
 *   /tmp/test_frame_rate
 *
 * Regression covered: the main sensor's native frame (7546 lines x 4415 ns)
 * runs at 30.03 fps, so a strict 30 fps ceiling used to compute a 7552-line
 * fastest bound and the bridge discarded the first frames of every start
 * and lens switch as "too fast" (log 421-diag46: first delivered picture
 * was seq=3). The native length must be admissible under the mode's own
 * nominal 30 fps contract, while a real client cap (24 fps) must not move.
 */
#include <stdio.h>
#include <string.h>
#include "camera_isp.h"

static int failures;

static void expect(const char *name, bool ok)
{
	printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
	if (!ok) failures++;
}

static struct hakios_camera_frame frame(uint32_t sensor, uint32_t line_ns,
					  uint32_t min_lines, uint32_t max_lines)
{
	struct hakios_camera_frame f;
	memset(&f, 0, sizeof(f));
	f.sensor = sensor;
	f.line_ns = line_ns;
	f.frame_lines_min = min_lines;
	f.frame_lines_max = max_lines;
	f.margin = 4;
	return f;
}

int main(void)
{
	struct isp isp;
	struct hakios_camera_frame f;

	/* Main sensor, the default 15-30 fps contract: native 7546 admitted. */
	isp_init(&isp);
	f = frame(1, 4415, 7546, 65532);
	expect("main 15-30 accepted", isp_frame_rate(&isp, &f, 15, 30));
	expect("main fastest is native 7546", isp.min_frame_lines == 7546);
	expect("main longest is 15100", isp.max_frame_lines == 15100);

	/* Front sensor (7642 x 4361 ns = 30.005 fps): native admitted. */
	isp_init(&isp);
	f = frame(2, 4361, 7642, 65535);
	expect("front 15-30 accepted", isp_frame_rate(&isp, &f, 15, 30));
	expect("front fastest is native 7642", isp.min_frame_lines == 7642);

	/* Ultra-wide (6408 x 5200 ns = 30.007 fps): native admitted. */
	isp_init(&isp);
	f = frame(3, 5200, 6408, 65535);
	expect("uw 15-30 accepted", isp_frame_rate(&isp, &f, 15, 30));
	expect("uw fastest is native 6408", isp.min_frame_lines == 6408);

	/* Telephoto (3888 x 8538 ns = 30.13 fps) is a genuine 0.43% over-rate,
	 * outside the 0.2% quantization slop: the strict bound stands and AE
	 * frame lengthening is the correct path, so no clamp here. */
	isp_init(&isp);
	f = frame(4, 8538, 3888, 65535);
	expect("tele 15-30 accepted", isp_frame_rate(&isp, &f, 15, 30));
	expect("tele fastest stays 3908", isp.min_frame_lines == 3908);

	/* A real client cap far below native must not move: fixed 24 fps on
	 * the main sensor quantizes to 9438 lines (1e9/24/4415 = 9437.5). */
	isp_init(&isp);
	f = frame(1, 4415, 7546, 65532);
	expect("main fixed 24 accepted", isp_frame_rate(&isp, &f, 24, 24));
	expect("main 24fps fastest is 9438", isp.min_frame_lines == 9438);
	expect("main 24fps longest is 9438", isp.max_frame_lines == 9438);

	/* Out-of-contract requests still refuse. */
	isp_init(&isp);
	f = frame(1, 4415, 7546, 65532);
	expect("max 31 refused", !isp_frame_rate(&isp, &f, 15, 31));
	expect("min 3 refused", !isp_frame_rate(&isp, &f, 3, 30));

	printf("%s (%d failures)\n", failures ? "FAILED" : "ALL PASS", failures);
	return failures != 0;
}
