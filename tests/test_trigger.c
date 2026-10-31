// SPDX-License-Identifier: MIT
#include <math.h>
#include <stdio.h>

#include "../src/trigger.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void)
{
	/* the Neo's numbers */
	struct cdn_trigger_cfg cfg = { .enter_gain = 2304, .exit_gain = 1792, .enter_noise = 3,
				       .exit_noise = 2, .fps_threshold = 30, .ramp_down_frames = 30 };
	struct cdn_trigger t;
	float s;

	cdn_trigger_init(&t, &cfg, CDN_MODE_AUTO);
	CHECK(cdn_trigger_step(&t, 600, -1, 30) == 0);
	CHECK(cdn_trigger_step(&t, 2303, -1, 30) == 0);
	/* enter at >= 2304, no ramp up by default */
	CHECK(cdn_trigger_step(&t, 2304, -1, 30) == 1);
	/* hysteresis: stays on down to the exit level */
	CHECK(cdn_trigger_step(&t, 1792, -1, 30) == 1);
	CHECK(cdn_trigger_step(&t, 2000, -1, 30) == 1);
	/* leave below 1792, fading over 30 frames */
	s = cdn_trigger_step(&t, 1791, -1, 30);
	CHECK(fabsf(s - (1 - 1 / 30.0f)) < 1e-4f);
	for (int i = 0; i < 28; i++)
		s = cdn_trigger_step(&t, 1000, -1, 30);
	CHECK(s > 0);
	CHECK(cdn_trigger_step(&t, 1000, -1, 30) == 0);
	/* back above the exit level but under enter: stays off */
	CHECK(cdn_trigger_step(&t, 2200, -1, 30) == 0);
	/* above the frame rate threshold: off */
	CHECK(cdn_trigger_step(&t, 4000, -1, 60) == 0);
	CHECK(cdn_trigger_step(&t, 4000, -1, 30) == 1);
	/* no gain: fall back to noise */
	cdn_trigger_init(&t, &cfg, CDN_MODE_AUTO);
	CHECK(cdn_trigger_step(&t, 0, 2.5f, 30) == 0);
	CHECK(cdn_trigger_step(&t, 0, 3.0f, 30) == 1);
	CHECK(cdn_trigger_step(&t, 0, 2.1f, 30) == 1);
	CHECK(cdn_trigger_step(&t, 0, 1.9f, 30) < 1);
	/* either signal turns it on: gain low but noisy (an untuned ISP) */
	cdn_trigger_init(&t, &cfg, CDN_MODE_AUTO);
	CHECK(cdn_trigger_step(&t, 600, 2.0f, 30) == 0);
	CHECK(cdn_trigger_step(&t, 600, 3.5f, 30) == 1);
	CHECK(cdn_trigger_step(&t, 2400, 1.0f, 30) == 1);
	CHECK(cdn_trigger_step(&t, 1000, 1.0f, 30) < 1);
	/* a zero threshold disables that test */
	cfg.enter_noise = 0;
	cdn_trigger_init(&t, &cfg, CDN_MODE_AUTO);
	CHECK(cdn_trigger_step(&t, 600, 50.0f, 30) == 0);
	cfg.enter_noise = 3;
	/* user override */
	cdn_trigger_init(&t, &cfg, CDN_MODE_ON);
	CHECK(cdn_trigger_step(&t, 300, -1, 30) == 1);
	cdn_trigger_init(&t, &cfg, CDN_MODE_OFF);
	CHECK(cdn_trigger_step(&t, 9000, -1, 30) == 0);
	/* ramp up when asked */
	cfg.ramp_up_frames = 4;
	cdn_trigger_init(&t, &cfg, CDN_MODE_AUTO);
	CHECK(fabsf(cdn_trigger_step(&t, 3000, -1, 30) - 0.25f) < 1e-4f);
	printf(fails ? "FAIL\n" : "PASS\n");
	return fails != 0;
}
