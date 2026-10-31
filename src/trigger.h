/* SPDX-License-Identifier: MIT */
#ifndef CDN_TRIGGER_H
#define CDN_TRIGGER_H

#include <stdbool.h>
#include <stdint.h>

enum cdn_mode {
	CDN_MODE_AUTO,	/* follow the sensor gain (or noise) with hysteresis */
	CDN_MODE_ON,	/* always denoise */
	CDN_MODE_OFF,	/* never denoise */
};

struct cdn_trigger_cfg {
	/* total sensor gain, 1/256 steps: enter at >= enter, leave below exit */
	uint32_t enter_gain;
	uint32_t exit_gain;
	/* the same on the estimated luma noise, when the gain is unknown */
	float enter_noise;
	float exit_noise;
	/* denoise only while the frame rate is at most this (0: any) */
	float fps_threshold;
	/* frames to fade the filter in and out */
	int ramp_up_frames;
	int ramp_down_frames;
};

struct cdn_trigger {
	struct cdn_trigger_cfg cfg;
	enum cdn_mode mode;
	bool gain_on, noise_on;
	bool active;
	float strength;
};

void cdn_trigger_init(struct cdn_trigger *t, const struct cdn_trigger_cfg *cfg,
		      enum cdn_mode mode);

/*
 * Advance one frame. gain is 0 when unknown, noise < 0 when unknown, fps
 * <= 0 when unknown. In auto mode the filter is on while the gain or the
 * measured noise is on (each with its own hysteresis; a threshold of 0
 * disables that test) and the frame rate is within the threshold.
 * Returns the filter strength for this frame, 0..1.
 */
float cdn_trigger_step(struct cdn_trigger *t, uint32_t gain, float noise, float fps);

const char *cdn_mode_name(enum cdn_mode m);
int cdn_mode_parse(const char *s, enum cdn_mode *m);

#endif
