/* SPDX-License-Identifier: MIT */
#ifndef CDN_PICTURE_H
#define CDN_PICTURE_H
/*
 * The picture stage: the last pass over each NV12 output frame, after the
 * denoiser (and on frames it passes through). Lens-shading compensation
 * for the dark corners the camera's own processing leaves, then the usual
 * picture controls. Every setting at its default leaves the frame as it is
 * and the pass is skipped.
 */
#include <stdbool.h>
#include <stdint.h>

#include "backend.h"

struct cdn_picture_cfg {
	float vignette;   /* 0..1: corner brightening, 1 = twice the light in the corners */
	float brightness; /* -1..1: luma offset, 1 = +64 levels */
	float contrast;   /* 0.5..2: around mid grey */
	float saturation; /* 0..2 */
	float warmth;     /* -1..1: blue <-> orange */
	float tint;       /* -1..1: green <-> magenta */
	float sharpen;    /* 0..1: unsharp mask on luma, 1 = strong */
};

void cdn_picture_defaults(struct cdn_picture_cfg *p);
bool cdn_picture_neutral(const struct cdn_picture_cfg *p);

struct cdn_picture;
struct cdn_picture *cdn_picture_create(void);
void cdn_picture_destroy(struct cdn_picture *s);
/* In place. 0, or -ENOMEM (the frame is then left as it is). */
int cdn_picture_apply(struct cdn_picture *s, const struct cdn_picture_cfg *p, struct cdn_frame *f);

#endif
