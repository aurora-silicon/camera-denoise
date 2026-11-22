/* SPDX-License-Identifier: MIT */
#ifndef CDN_APPLE_FACEEXP_H
#define CDN_APPLE_FACEEXP_H

#include <stdint.h>

/*
 * Studio Light's face exposure statistics, in plain C. They give the light estimate's inputs (coefficients
 * 6, 7, 8 and 13; see sl_light_from_fexp()).
 *
 *  - global: 64x64 nearest samples of the half-size luma, a 64-bin histogram, median and 99th percentile;
 *  - per face: a 64x64 grid over the face rect: nearest luma, bilinear skin and person masks (the
 *    segmentation's planes); histograms of the skin (skin >= the mask threshold) and of the whole face
 *    (person > it); skin 5 / 50 / 95 % and face 1 / 95 %; the skin mean;
 *  - coefficients: each face's skin mean against a target median (the tuning's six per-skin-tone targets
 *    weighted by the face's skin-tone probabilities), gated by skin contrast and brightness, into a curve
 *    boost; the faces averaged, or per face weighted by tone similarity. With no per-face instance mask
 *    (the case here: every face has instance 4 and confidence 0) the faces are always averaged (checked with
 *    targets 0.18 apart); the per-face path is not reached and is unverified.
 *
 * The tuning is read at run time (fexp_params_read: "face-exposure.conf" from the user's install).
 * Checked against the same passes rendered on macOS (five captures: one face, panned, two faces of
 * different brightness): the luma, every statistic, coefficient and the light estimate, exact. That needs
 * the GPU's own precision: bilinear weights in 8-bit fractions, and half arithmetic run in float with only
 * the final conversions rounded.
 */
struct fexp_params {
	float mask_threshold;           /* skin / person mask threshold */
	float target_median[6];         /* per skin tone I..VI */
	float max_boost[6];             /* per skin tone I..VI */
	float min_face_size;            /* of the larger face side */
	float max_curve_boost, min_curve_boost;
	float max_target_ratio_darkening, max_target_ratio_limit;
	float bias_factor;              /* per EV of exposure bias */
	float tone_similarity_sigma;
	float face_exp_dif_threshold;   /* targets this close: one boost for all faces */
	int relight_only_person_mask;
};
#include <stddef.h>
/* "key = value" tuning (mask-threshold, target-median.1..6, max-boost.1..6, ...): 0, or -1 with the reason. */
int fexp_params_read(struct fexp_params *p, const char *path, char *err, size_t errlen);

struct fexp_global {
	int valid;
	float median, p99;
	int hist[64];
};

struct fexp_face {
	float x, y, w, h;               /* top-left origin, normalised */
	float tone[6];                  /* skin-tone class probabilities I..VI (from a face classifier) */
};

struct fexp_face_stats {
	int valid, instance, count;
	float instance_conf, size;
	float skin_lo, skin_median, skin_hi, face_lo, face_hi, mean;
	float target, target_adj, boost_max;    /* written by fexp_coefficients() */
	float tone[6];
	int hist[64];
};

struct fexp_coeffs {
	unsigned flags, flags2;
	float p99, boost[5], gain[5], c5, c6, c7, c8;
	int low_skin;
	float c10;
	int single;
	float seen[4], c13;
};

/*
 * The half-size luma the statistics read (1280x720 R8 -> 640x360 R8): each output is a bilinear tap at the
 * centre of a 2x2 block, returned as fp16 and stored as 8-bit. out: (w / 2) x (h / 2) values 0..1.
 */
void fexp_luma_half(const uint8_t *y, int w, int h, int stride, float *out);

/* luma: lw x lh values 0..1 (the frame's luma at half size, 8-bit; fexp_luma_half()). */
void fexp_global(const float *luma, int lw, int lh, struct fexp_global *g);
/* skin, person: mw x mh (256x192, the segmentation's planes). */
void fexp_face_stats(const float *luma, int lw, int lh, const float *skin, const float *person, int mw, int mh,
		    const struct fexp_face *f, const struct fexp_params *p, struct fexp_face_stats *s);
/* exp_bias: the camera's exposure bias (EV, 0); face_exp_ratio (0). */
void fexp_coefficients(const struct fexp_params *p, const struct fexp_global *g, struct fexp_face_stats *s, int nfaces,
		      float exp_bias, float face_exp_ratio, struct fexp_coeffs *c);

#endif
