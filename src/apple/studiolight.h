/* SPDX-License-Identifier: MIT */
#ifndef CDN_APPLE_STUDIOLIGHT_H
#define CDN_APPLE_STUDIOLIGHT_H

#include <stdint.h>

/*
 * Studio Light, in plain C:
 * the reference for the GPU port, checked pass by pass against the passes' own inputs and outputs captured
 * on macOS (tests/test_studiolight.c). One frame (no history):
 *
 *  1. diffuse: the depth network's disparity (256x144) unprojected through the camera's field of view,
 *     the surface normal from 4 taps, its z (facing the camera) = a Lambert term for a light at the camera;
 *  2. light mask (320x180, 2 channels) from the face rects: a face-light term and the subject (a capsule
 *     from the face down the body);
 *  3. light estimate: the exposure target L (0.58 with no face; with faces the face exposure statistics);
 *  4. the gain map (320x180): background dimmed by a radial vignette tint, the subject lifted by an exposure
 *     curve L x / (L x + (1 - L)(1 - x)) driven by the diffuse term, as out / in per channel;
 *  5. 3x3 Gaussian, then a 3x3 Gaussian of bilinear taps up to 640x360;
 *  6. full size: the frame (BT.709 video range) to linear, times the gain (eased to 1 on highlights),
 *     through a 32^3 colour cube straight to YCbCr; chroma from each 2x2 block's top-left pixel.
 *
 * The colour cube is built from macOS's 32^3 colour-correction cube, which is Apple's data (not shipped):
 * sl_cube_build() takes it, or NULL for none (identity).
 */

/* A YCbCr matrix: kr, kb (0.2126, 0.0722 = BT.709; 0.299, 0.114 = BT.601), full or video range. */
struct sl_yuv {
	float kr, kb;
	int full_range;
};

/* Per-run constants (the values the reference renderer passes, as traced) */
struct sl_params {
	float bg_centre[3], bg_edge[3];   /* vignette tint: centre, corners (0.9135 0.9135 0.966 / 0.315 0.315 0.3465) */
	float vignette_pow;               /* 0.5 */
	float near, far;                  /* subject when disparity >= near * focus, background <= far * focus (1.05, 0.9) */
	float lum_mix;                    /* luminance-only vs per-channel exposure curve (0.5) */
	float subject_mix[3];             /* how much of the relit subject per channel (1, 0.9, 0.75) */
	float bg_gamma[3];                /* background pow (1.03093, 1.03093, 1) */
	float hl_lo[2], hl_hi[2];         /* highlight guard: luma (lo, hi) at the centre and the edge */
	float fov[2];                     /* tan(half FOV) x, y (0.577293, 0.324727) */
	float max_disparity;              /* 10 */
	float distance_modifier;          /* 1 */
	/* the frame's matrix as Portrait passes it (half3x4 rows: R'G'B'1 -> YCbCr / 255 codes, and back) */
	float rgb2yuv[3][4], yuv2rgb[3][4];
};
/* BT.709 video range (what Portrait uses for an untagged or a BT.709 buffer) */
void sl_params_default(struct sl_params *p);
void sl_params_matrix(struct sl_params *p, const struct sl_yuv *m);

/* The light estimate: x = exposure target L; y, z: highlight protection (0 with no face). */
struct sl_light {
	float l, y, z, w;
};
/* No face: 0.58. With faces: the face exposure coefficients 6, 7, 8 and 13 (c13 = target median; faceexp.h). */
void sl_light_default(struct sl_light *o);
void sl_light_from_fexp(struct sl_light *o, float c6, float c7, float c8, float c13);

/* Face rects (top-left origin, normalised, like the frame) for the light mask; up to 4. */
struct sl_face {
	float x, y, w, h;
};

/* Pass 1: disparity (dw x dh, the network's) -> diffuse (dw x dh, 8-bit values as macOS stores them). */
void sl_diffuse(const float *disp, int dw, int dh, const struct sl_params *p, float *diffuse);

/*
 * Pass 2: the light mask (mw x mh x 2, 8-bit values); frame_aspect = width / height. Each face's radius is
 * 0.375 x its diagonal / sqrt 2 in height units (|(w * aspect, h)|; checked on a square and a wide rect).
 */
void sl_light_mask(const struct sl_face *faces, int nfaces, float frame_aspect, int mw, int mh, float *mask);

/*
 * Pass 4: the gain map (gw x gh x 4). guide: the frame at gdw x gdh as linear RGBA (macOS: 512x288 BGRA8
 * read as sRGB); disp: dw x dh disparity x 1.94097 (fp16 values); focus_offset: Portrait's focus offset
 * (-1.94097 * focus disparity; -1.5 with no face).
 */
void sl_gain(const float *guide, int gdw, int gdh, const float *diffuse, const float *disp, int dw, int dh,
	     const float *mask, const struct sl_params *p, const struct sl_light *l, float focus_offset,
	     float *gain, int gw, int gh);

/* Pass 5: 3x3 Gaussian (in place size), then up to uw x uh. */
void sl_gain_blur(const float *in, int w, int h, float *out);
void sl_gain_upsample(const float *in, int w, int h, float *out, int uw, int uh);

/* The 32^3 cube from linear RGB (normalised by min/max) to YCbCr (BT.709 video range codes / 255). */
struct sl_cube {
	int n;                            /* 32 */
	float *yuv;                       /* n^3 * 3, x fastest */
	float min[3], max[3];             /* the linear range the cube spans */
};
/*
 * correction: n^3 RGBA8 (macOS's colour-correction cube: fixed data, the same for any matrix; on this
 * frame it moves luma by 0.2 LSB on average), NULL for identity. Returns 0, -1 on no memory.
 */
int sl_cube_build(struct sl_cube *c, int n, const uint8_t *correction, const struct sl_params *p);
void sl_cube_free(struct sl_cube *c);

/*
 * Pass 6: the frame (NV12) relit by the gain (gw x gh) into oy/ouv. macOS reads the camera's YCbCr texture
 * through the GPU's own conversion (the buffer's matrix, clamped to 0..1, chroma of the pixel's 2x2 block);
 * m is that matrix (for an untagged buffer the GPU uses BT.601 while Portrait's own matrices are BT.709:
 * the captures; a tagged buffer has one matrix for both). The output is the cube's YCbCr.
 */
void sl_relight_nv12(const uint8_t *y, const uint8_t *uv, int w, int h, int stride, const struct sl_yuv *m,
		     const float *gain, int gw, int gh, const struct sl_params *p, const struct sl_cube *c,
		     uint8_t *oy, uint8_t *ouv);

/*
 * The whole first frame: disp = the network's disparity (dw x dh, macOS 256x144), guide = the frame at 512x288
 * (linear RGBA, see sdof_guide_nv12), the faces, the light estimate and Portrait's focus offset, the cube.
 * Returns 0, -1 when out of memory.
 */
int sl_render_nv12(const uint8_t *y, const uint8_t *uv, int w, int h, int stride, const struct sl_yuv *m,
		   const struct sl_params *p, const float *guide, int gdw, int gdh, const float *disp, int dw, int dh,
		   const struct sl_face *faces, int nfaces, const struct sl_light *l, float focus_offset,
		   const struct sl_cube *c, uint8_t *oy, uint8_t *ouv);

#endif
