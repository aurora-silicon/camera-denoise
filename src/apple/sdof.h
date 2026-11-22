/* SPDX-License-Identifier: MIT */
#ifndef CDN_APPLE_SDOF_H
#define CDN_APPLE_SDOF_H

#include <stdint.h>
/*
 * Portrait's background blur, in plain C: the reference the GPU port is checked against, itself checked
 * against the same passes rendered on macOS (their inputs and outputs, captured: tests/test_sdof.c).
 *
 * Inputs: the frame at half size as a 4-level mip pyramid of linear RGBA (kept sRGB-encoded, sampled
 * linear), and the "focus disparity" map: min(0, 1.94097 * disparity - 1.94097 * focus),
 * the depth network's disparity relative to the focus, 0 for everything at or in front of it. Output:
 * linear RGB, the blurred picture, and alpha = how much of the sharp frame to keep.
 *
 * 19 rays per pixel over a hexagonal aperture (centre, 6 at radius 0.5, 12 at 1), each jittered by a
 * second point from a fixed table, the aperture squeezed off-axis (cat's eye) and rotated by a per-pixel
 * hash; each ray reaches as far as the disparity the previous ray found, and samples the pyramid at the
 * level its blur radius needs; highlights weigh more (1 + 0.02 / 0.306 * radius * colour).
 */
struct sdof_params {
	float f0;               /* blur per unit of focus disparity (from the aperture) */
	float f1x, f1y;         /* the aperture's extent in output uv per unit */
	float f2x, f2y;         /* the cat's-eye squeeze across the frame */
	float frame_w;          /* the full frame's width (1280 for 720p) */
};

/* The parameters for an aperture (0..1; default 0.5) on a 16:9 frame. */
void sdof_params_for(struct sdof_params *p, float aperture, int frame_w);

/* The focus disparity map from the network's disparity (1.94097 * d, guided-upsampled, + offset). */
float sdof_focus_offset(float focus_disparity);

struct sdof_level {
	int w, h;
	const float *rgba;      /* linear, w * h * 4 */
};

/*
 * One pass over the ow x oh output (the half-size frame), into out (ow * oh * 4, linear RGB +
 * alpha). levels: the pyramid, level 0 = ow x oh. disp: dw x dh floats.
 */
void sdof_raytrace(const struct sdof_level *levels, int nlevels, const float *disp, int dw, int dh,
		   const struct sdof_params *p, float *out, int ow, int oh);

/*
 * The colour pyramid the ray pass samples (level 0 from the frame, then a 3x3 Gaussian downscale x3):
 * level 0 is the frame at half size, one bilinear tap at each output pixel's centre (the 2x2 luma
 * average, the chroma texel) through the YCbCr -> R'G'B' conversion and the BT.709 inverse OETF to linear;
 * each next level is a 3x3 Gaussian (sigma ~1, weights 0.0751 / 0.1238 / 0.2042) of bilinear taps one
 * source texel apart around the output centre. macOS stores every level RGBA8 sRGB; these round to that too
 * (out: linear RGBA, alpha 1).
 *
 * Matrix: BT.601 video range is what macOS uses for an untagged 4:2:0 buffer (the captures'); a real frame
 * passes its own (the coefficients kr, kb and full/video range).
 */
struct sdof_yuv {
	float kr, kb;           /* 0.299, 0.114 = BT.601; 0.2126, 0.0722 = BT.709 */
	int full_range;
	/*
	 * The buffer is colour-tagged (BT.709 transfer): macOS's scaler then colour-manages the guides,
	 * R' linearised by ColorSync's BT.709 gamma 1.961 and stored sRGB. Untagged, the R' bytes go in as is.
	 */
	int tagged;
};
void sdof_level0_nv12(const uint8_t *y, const uint8_t *uv, int w, int h, int stride,
		      const struct sdof_yuv *m, float *out);
void sdof_downscale_gauss3(const float *src, int sw, int sh, float *dst, int dw, int dh);

/*
 * The depth's guided upsampling (a guided filter: radius 1, the 3x3 box,
 * colour guide, eps 0.05): the network's 256x144 disparity, guided by the frame at that size, becomes
 * 512x288 guided by the frame at 512x288, then the post-modifier clamp(x + offset, lo, hi) makes it the
 * focus disparity (offset = sdof_focus_offset(focus), lo = -FLT_MAX, hi = 0).
 *
 * Guides are linear RGBA (4 floats per pixel; macOS samples BGRA8 sRGB). coef: w * h * 4 floats, the
 * per-pixel linear model (a.rgb, b), stored as fp16 values like macOS's RGBA16F texture.
 */
void sdof_guided_coefs(const float *guide, const float *p, int w, int h, float eps, float *coef);
void sdof_guided_average(const float *coef, int w, int h, float *out);
void sdof_guided_apply(const float *coef, int cw, int ch, const float *guide, int w, int h,
		       float offset, float lo, float hi, float *out);

/*
 * The composite (Portrait's interpolateRGBWeightSourceRGBADest*): the ray pass's result, bilinear-upsampled to
 * the frame, mixed with the sharp frame by its alpha in linear light (BT.709 inverse OETF, back by the
 * OETF), with film grain on the blurred part: (1 - alpha) * n * clamp(B / 2, 0, sqrt(a + b * B)), n the
 * mean of two taps of a Gaussian-noise table tiled every lut_size * grain pixels (macOS: 256 * 3; a, b from
 * the camera's noise model, 8.19e-11 and 3.05e-6 with none). Pixels with alpha 1 keep the frame as it is.
 * Output: 4:2:0, chroma the mean of each 2x2 block.
 */
struct sdof_grain {
	const float *lut;       /* lut_size^2 values, ~N(0, 1); NULL: no grain */
	int lut_size;
	float grain;            /* pixels per table entry (3) */
	float a, b;             /* noise sd = sqrt(a + b * linear value) */
	int ox, oy;             /* the table's offset this frame */
};
/* A grain table like macOS's (white N(0, 1), fp16 values; ours from a fixed seed, not Apple's). */
void sdof_grain_table(float *lut, int n, uint32_t seed);
void sdof_composite_nv12(const float *ray, int rw, int rh, const uint8_t *y, const uint8_t *uv, int w, int h,
			 int stride, const struct sdof_yuv *m, const struct sdof_grain *g, uint8_t *oy, uint8_t *ouv);

/*
 * The depth's temporal filter (Portrait's temporalFilterEMA_LKT, kIIRUpdateCoefficients (0.23, 1),
 * kMotionThresholdMinMax (1, 4)): the history (prev, w x h) moved along the optical flow (flow: fw x fh x 2,
 * in flow-grid pixels, macOS 128x72) and blended with the new disparity x 1.94097: out = mix(prev, new, k),
 * k = 0.23 still, up to 1 at 4 pixels of motion (in 720p pixels); history that moves in from outside is new.
 * The history is sampled bicubic (Metal's filter::bicubic = Catmull-Rom), the flow bilinear.
 */
void sdof_temporal_ema(const float *flow, int fw, int fh, const float *prev, const float *disp, int w, int h,
		       float *out);

/*
 * The whole frame as Portrait renders one with no history (its first frame): the network's disparity
 * (dw x dh, macOS 256x144) x 1.94097, guided-upsampled to 2dw x 2dh with the frame's guides (R'G'B' area-
 * resampled; macOS's own scaler differs by < 1 LSB on average) and offset by the focus, the ray pass on
 * the half-size pyramid, the composite into oy/ouv (may be y/uv). Returns 0, or -1 when out of memory.
 */
int sdof_render_nv12(const uint8_t *y, const uint8_t *uv, int w, int h, int stride, const float *disp, int dw,
		     int dh, float aperture, float focus_offset, const struct sdof_yuv *m,
		     const struct sdof_grain *g, uint8_t *oy, uint8_t *ouv);

/*
 * The frame's R'G'B' area-resampled to ow x oh, as the BGRA8 guide macOS reads as sRGB (Portrait's and Studio
 * Light's 256x144 / 512x288 guides; macOS's own scaler differs by < 1 LSB on average): linear RGBA out.
 */
int sdof_guide_nv12(const uint8_t *y, const uint8_t *uv, int w, int h, int stride, const struct sdof_yuv *m,
		    int ow, int oh, float *out);

/* sRGB <-> linear, as the sRGB texture formats convert (8-bit storage). */
float sdof_srgb_to_linear(float v);
float sdof_linear_to_srgb(float v);

#endif
