/* SPDX-License-Identifier: MIT */
#ifndef CDN_APPLE_PORTRAIT_H
#define CDN_APPLE_PORTRAIT_H
/*
 * The image half of the effects: the person and skin segmentation network
 * (R, G, B planes at 256 x 192 scaled by 1/255 in, person and skin
 * probabilities out) feeds
 *  - Portrait: the background blurred, the person kept sharp;
 *  - Studio Light: a key light on the face and the background dimmed;
 *  - the subject that Center Stage frames.
 * The masks are refined to the frame with an edge-aware (guided) filter on
 * the luma at a quarter of the frame size. Everything here is plain C on
 * NV12, so it is tested on the host with masks the tests make up.
 */
#include <stdint.h>

#include <stdbool.h>

#include "../backend.h"

#define APPLE_SEG_W 256
#define APPLE_SEG_H 192

/* One frame's subject, from the segmentation: union of the people and the head. */
struct apple_subject {
	bool present;
	int people;                  /* 1, or 2 for "more than one" */
	float x0, y0, x1, y1;        /* everyone */
	float head_x, head_top, head_h;
};

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

/*
 * Center Stage's detections from the masks, one per person (column runs of the person mask apart by
 * more than 8 % of the width): the body = the person's extent, which matches a human detector's box
 * (a macOS face and body detector: median IoU 0.89 on 73 Linux frames); the face = the largest skin blob in the top 45 % of
 * the person, as a box 1.2 median skin rows wide, square in frame pixels, its top 0.16 of its height
 * below the blob's top (fitted to that detector's face boxes on 96 Linux frames: median IoU 0.80, size error
 * 5.5 %), only when the blob looks like a face alone (it fills >= 40 % of the box, <= 30 % of it lies
 * outside, at most 2 boxes tall): raised arms, a bare torso or a turned head give no face, and
 * Center Stage follows the body, as macOS does without a face. Rects normalised to the frame.
 */
#define APPLE_MAX_PEOPLE 4
struct apple_person {
	float bx, by, bw, bh;
	int face;
	float fx, fy, fw, fh;
};
int apple_masks_people(const struct apple_masks *m, float aspect, struct apple_person *out, int max);

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

/*
 * The same effects in two steps, for a composite elsewhere (the GPU): the
 * quarter-size maps from the frame as it came in, then the full-size
 * composite. Blur first, light on the blurred picture, as above. All maps
 * qw x qh floats; lut = 64 x 256 bytes (light level x luma).
 */
#include <stdbool.h>
void apple_portrait_blur_prepare(struct apple_portrait *p, const struct cdn_frame *f, float strength);
/*
 * refine and blur_prepare split at their only full-size reads: the frame's
 * quarter-size sums (qw x qh floats each), from apple_portrait_quarter() on
 * the CPU or apple_gpu_quarter() on the GPU (the same numbers).
 */
struct apple_quarter {
	const float *gsum, *ysum, *usum, *vsum;
};
void apple_portrait_quarter(struct apple_portrait *p, const struct cdn_frame *f,
			    struct apple_quarter *q);
void apple_portrait_refine_q(struct apple_portrait *p, const struct apple_masks *m,
			     const struct apple_quarter *q);
void apple_portrait_blur_prepare_q(struct apple_portrait *p, const struct apple_quarter *q,
				   float strength);
void apple_portrait_blur_apply(struct apple_portrait *p, struct cdn_frame *f);
bool apple_portrait_light_prepare(struct apple_portrait *p, const struct apple_subject *s,
				  float strength);
void apple_portrait_light_apply(struct apple_portrait *p, struct cdn_frame *f);
struct apple_fx_maps {
	int qw, qh;
	const float *alpha, *by, *bu, *bv, *light, *bg;
	const uint8_t *lut;
	float g;
};
void apple_portrait_maps(const struct apple_portrait *p, struct apple_fx_maps *m);

#endif
