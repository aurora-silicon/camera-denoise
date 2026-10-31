/* SPDX-License-Identifier: MIT */
#ifndef CDN_GAIN_H
#define CDN_GAIN_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Per-frame sensor gain from the camera's V4L2 device: a read-only, volatile
 * V4L2_CID_GAIN in 1/256 steps (apple-isp reports the firmware's total gain
 * this way) and V4L2_CID_EXPOSURE_ABSOLUTE (100 us steps).
 */
struct cdn_gain {
	int fd;
	bool have_gain, have_exposure;
};

/* Open path for controls only (never streams). false if no gain control. */
bool cdn_gain_open(struct cdn_gain *g, const char *path);
/* 0 when unavailable */
uint32_t cdn_gain_read(struct cdn_gain *g, uint32_t *exposure_us);
void cdn_gain_close(struct cdn_gain *g);

/*
 * Luma noise from frame to frame, in 8-bit code values (a std-like scale):
 * the median |difference| from the previous frame on a sparse grid, per
 * block, then the median over blocks, so texture does not count (unlike a
 * spatial estimate) and a moving subject does not dominate. Cheap enough
 * to run on every frame, also while the filter is off.
 */
struct cdn_noise_tracker {
	uint8_t *prev;
	int w, h;	/* sampled grid */
	bool valid;
};

/* < 0 on the first frame or after a size change */
float cdn_noise_track(struct cdn_noise_tracker *t, const uint8_t *y, int stride,
		      int width, int height);
void cdn_noise_tracker_reset(struct cdn_noise_tracker *t);
void cdn_noise_tracker_free(struct cdn_noise_tracker *t);

#endif
