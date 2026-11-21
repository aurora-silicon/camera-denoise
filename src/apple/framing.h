/* SPDX-License-Identifier: MIT */
#ifndef CDN_APPLE_FRAMING_H
#define CDN_APPLE_FRAMING_H
/*
 * Automatic framing ("Center Stage") with the tunables macOS ships in
 * CinematicFraming.framework (cinematicFramingParameters.plist, J815/J813):
 * the default and smallest viewport, the subject deadband, the head's offset
 * from the top, per-axis speed and acceleration multipliers, the recentring
 * and no-subject timings. Only the numbers come from Apple; the controller
 * is ours. Coordinates are fractions of the full camera frame, x and y in
 * [0, 1]; a viewport keeps the frame's aspect ratio, its size is the
 * fraction of the frame's width (and height) it shows.
 */
#include <stdbool.h>
#include <stdint.h>

struct apple_framing_params {
	float default_fov;      /* viewportDefaultFOV */
	float min_fov;          /* viewportMinimumFOV */
	float deadband_w, deadband_h;          /* one subject */
	float group_deadband_w, group_deadband_h;
	float top_offset;       /* head top below the viewport top, of its height */
	float group_width;      /* a group fills this much of the viewport width */
	float speed_x, speed_y, speed_zoom_in, speed_zoom_out;
	float accel_x, accel_y, accel_zoom;
	float ease_x, ease_y, ease_zoom; /* approach gains, 1/s */
	float recenter_idle_s;   /* still subject off centre: recentre after */
	float recenter_s;        /* ... over this long */
	float no_subject_wait_s; /* nobody: wait, then go home over */
	float no_subject_return_s;
	float tiny_head;         /* heads smaller than this (of frame height) are ignored */
	float movement_tolerance; /* of the head size: smaller moves are "still" */
	float face_fov;          /* viewport size per unit of head height */
	float max_fov;           /* one person is never framed wider than this */
};

/* The J815/J813 values (also J616s's: the plist is shared). */
void apple_framing_params_default(struct apple_framing_params *p);

/* One frame's subject, from the segmentation: union of the people and the head. */
struct apple_subject {
	bool present;
	int people;                  /* 1, or 2 for "more than one" */
	float x0, y0, x1, y1;        /* everyone */
	float head_x, head_top, head_h;
};

struct apple_viewport {
	float cx, cy, size;
};

struct apple_framing {
	struct apple_framing_params p;
	struct apple_viewport pos, vel, target;
	struct apple_subject smooth;
	bool have_subject, started;
	float since_subject, still_for, since_move;
	float return_from_size, return_from_cx, return_from_cy, returning_for;
	float zoom;                  /* user zoom, 0 = automatic */
};

void apple_framing_init(struct apple_framing *f, const struct apple_framing_params *p,
			float zoom);
void apple_framing_reset(struct apple_framing *f);
/* Advance by dt seconds with this frame's subject; returns the viewport. */
struct apple_viewport apple_framing_step(struct apple_framing *f,
					 const struct apple_subject *s, float dt);
/* Pixel crop for a viewport: even-aligned, inside the frame, at least 64 x 64. */
void apple_framing_crop(const struct apple_viewport *v, int width, int height,
			int *x, int *y, int *w, int *h);

#endif
