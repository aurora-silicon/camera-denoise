/* SPDX-License-Identifier: MIT */
/*
 * Filter level for the video encoder's motion-compensated temporal filter
 * (MCTF), following what macOS asks of it for the built-in camera: a higher
 * level for the first frames of a stream, then a level that drops as the
 * light falls (traced on the J700: 16 for the first frames, 12 in room
 * light, 9 in a dark room).
 *
 * The light is judged from the sensor gain, with hysteresis so the level
 * does not flip between two neighbouring frames.
 */
#ifndef CDN_MCTF_LEVEL_H
#define CDN_MCTF_LEVEL_H

#include <stdint.h>

#define CDN_MCTF_LEVEL_START 16
#define CDN_MCTF_LEVEL_ROOM 12
#define CDN_MCTF_LEVEL_DARK 9
#define CDN_MCTF_START_FRAMES 6

struct cdn_mctf_level {
	uint32_t frames;
	int dark;
	/* total sensor gain in 1/256 steps: enter dark at or above, leave below */
	uint32_t dark_enter_gain, dark_exit_gain;
};

static inline void cdn_mctf_level_init(struct cdn_mctf_level *l, uint32_t enter, uint32_t exit)
{
	l->frames = 0;
	l->dark = 0;
	l->dark_enter_gain = enter;
	l->dark_exit_gain = exit < enter ? exit : enter;
}

/* gain 0 means unknown: keep the current light state */
static inline int cdn_mctf_level_step(struct cdn_mctf_level *l, uint32_t gain)
{
	if (gain) {
		if (!l->dark && gain >= l->dark_enter_gain)
			l->dark = 1;
		else if (l->dark && gain < l->dark_exit_gain)
			l->dark = 0;
	}
	if (l->frames < CDN_MCTF_START_FRAMES) {
		l->frames++;
		return CDN_MCTF_LEVEL_START;
	}
	return l->dark ? CDN_MCTF_LEVEL_DARK : CDN_MCTF_LEVEL_ROOM;
}

static inline void cdn_mctf_level_reset(struct cdn_mctf_level *l)
{
	l->frames = 0;
}

#endif
