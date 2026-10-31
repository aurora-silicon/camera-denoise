// SPDX-License-Identifier: MIT
#include <string.h>

#include "trigger.h"

void cdn_trigger_init(struct cdn_trigger *t, const struct cdn_trigger_cfg *cfg,
		      enum cdn_mode mode)
{
	t->cfg = *cfg;
	t->mode = mode;
	t->gain_on = false;
	t->noise_on = false;
	t->active = false;
	t->strength = 0;
}

static bool fps_ok(const struct cdn_trigger *t, float fps)
{
	/* a little slack: a nominal 30 fps stream measures 30.0x */
	return t->cfg.fps_threshold <= 0 || fps <= 0 || fps <= t->cfg.fps_threshold + 0.5f;
}

float cdn_trigger_step(struct cdn_trigger *t, uint32_t gain, float noise, float fps)
{
	float target;

	switch (t->mode) {
	case CDN_MODE_ON:
		t->active = true;
		break;
	case CDN_MODE_OFF:
		t->active = false;
		break;
	case CDN_MODE_AUTO:
		if (gain && t->cfg.enter_gain) {
			if (!t->gain_on && gain >= t->cfg.enter_gain)
				t->gain_on = true;
			else if (t->gain_on && gain < t->cfg.exit_gain)
				t->gain_on = false;
		}
		if (noise >= 0 && t->cfg.enter_noise > 0) {
			if (!t->noise_on && noise >= t->cfg.enter_noise)
				t->noise_on = true;
			else if (t->noise_on && noise < t->cfg.exit_noise)
				t->noise_on = false;
		}
		t->active = fps_ok(t, fps) && (t->gain_on || t->noise_on);
		break;
	}

	target = t->active ? 1.0f : 0.0f;
	if (t->strength < target) {
		t->strength = t->cfg.ramp_up_frames > 0 ?
			t->strength + 1.0f / t->cfg.ramp_up_frames : 1.0f;
		if (t->strength > target)
			t->strength = target;
	} else if (t->strength > target) {
		t->strength = t->cfg.ramp_down_frames > 0 ?
			t->strength - 1.0f / t->cfg.ramp_down_frames : 0.0f;
		if (t->strength < target + 1e-4f)
			t->strength = target;
	}
	return t->strength;
}

const char *cdn_mode_name(enum cdn_mode m)
{
	return m == CDN_MODE_ON ? "on" : m == CDN_MODE_OFF ? "off" : "auto";
}

int cdn_mode_parse(const char *s, enum cdn_mode *m)
{
	if (!strcmp(s, "auto"))
		*m = CDN_MODE_AUTO;
	else if (!strcmp(s, "on"))
		*m = CDN_MODE_ON;
	else if (!strcmp(s, "off"))
		*m = CDN_MODE_OFF;
	else
		return -1;
	return 0;
}
