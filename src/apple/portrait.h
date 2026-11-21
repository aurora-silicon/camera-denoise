/* SPDX-License-Identifier: MIT */
#ifndef CDN_APPLE_PORTRAIT_H
#define CDN_APPLE_PORTRAIT_H
/*
 * The image half of the Apple effects: macOS's person segmentation
 * (ImagingNetworks/personsemantics-preview-lowfps, the network Portrait.framework's
 * PTEffectPersonSegmentation runs: RGB 256 x 192 scaled by 1/255 in, person and
 * skin probabilities out) feeds
 *  - Portrait: the background blurred, the person kept sharp;
 *  - Studio Light: a key light on the face and the background dimmed;
 *  - the subject that Center Stage frames.
 * The masks are refined to the frame with an edge-aware (guided) filter on
 * the luma at a quarter of the frame size. Everything here is plain C on
 * NV12, so it is tested on the host with masks the tests make up.
 */
#include <stdint.h>

#include "../backend.h"
#include "framing.h"

#define APPLE_SEG_W 256
#define APPLE_SEG_H 192

struct apple_masks {
	float person[APPLE_SEG_H * APPLE_SEG_W];
	float skin[APPLE_SEG_H * APPLE_SEG_W];
	int valid;
};

/* NV12 -> planar RGB in [0, 1] at w x h (stretched), for the network. */
void apple_nv12_to_rgb(const struct cdn_frame *in, int matrix, int full_range,
		       float *r, float *g, float *b, int w, int h);

/* Temporal smoothing of fresh network output into the kept masks. */
void apple_masks_update(struct apple_masks *m, const float *person, const float *skin, float a);

/* The people and the head, from the masks. */
/* aspect: the frame's width / height (0 = unknown) */
void apple_masks_subject(const struct apple_masks *m, struct apple_subject *s, float aspect);

struct apple_portrait;
struct apple_portrait *apple_portrait_new(int width, int height);
void apple_portrait_free(struct apple_portrait *p);
/* Refine the masks to this frame (call once per frame before the effects). */
void apple_portrait_refine(struct apple_portrait *p, const struct apple_masks *m,
			   const struct cdn_frame *f);
/* Background blur (strength 0..1), in place. */
void apple_portrait_blur(struct apple_portrait *p, struct cdn_frame *f, float strength);
/* Studio Light (strength 0..1), in place; s gives the head for the key light. */
void apple_portrait_light(struct apple_portrait *p, struct cdn_frame *f,
			  const struct apple_subject *s, float strength);
/* The refined person alpha at quarter size (for tests). */
const float *apple_portrait_alpha(const struct apple_portrait *p, int *w, int *h);

#endif
