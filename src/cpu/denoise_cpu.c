// SPDX-License-Identifier: MIT
/*
 * CPU denoiser for NV12 camera frames.
 *
 * 1. Temporal: a motion-adaptive recursive filter. Each luma pixel keeps a
 *    12-bit history (8.4 fixed point). Three tests of frame - history
 *    against the estimated noise measure motion: the 3x3 sum of its
 *    magnitude, and the 5x5 and 9x9 sums of its sign, which see faint
 *    changes over larger areas. Static pixels blend in 1/8 of the new
 *    frame (about 15 frames of averaging), moving pixels take the new
 *    frame as is, so motion does not smear.
 * 2. Spatial: an edge-clipped 3x3 mean on luma, whose clip level follows
 *    the temporal weight: where motion stopped the temporal filter, the
 *    spatial filter does the work; on static areas it hardly acts.
 * 3. Chroma: the same temporal filter driven by the luma motion of its 2x2
 *    block, then two edge-clipped passes (distance 1 and 2), since low-light
 *    chroma noise is blotchy and low-frequency.
 *
 * The noise level is estimated every frame (median temporal difference,
 * bounded by a spatial estimate) so the filter follows the sensor gain.
 *
 * All arithmetic is integer and defined so that the NEON and scalar paths
 * give bit-identical results (tests/test_cpu.c checks this).
 */
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../backend.h"
#include "denoise_cpu.h"

#if defined(__aarch64__) && !defined(CDN_NO_NEON)
#include <arm_neon.h>
#define CDN_NEON 1
#else
#define CDN_NEON 0
#endif

#define KMAX 32767
#define AGE_MAX 7	/* 1/(AGE_MAX + 1) = 1/8: about 15 frames of averaging */

/*
 * Weight for a history of a frames: 1/(a + 1), so a history that motion just
 * reset becomes a running mean (1/2, 1/3, ...) instead of one noisy sample
 * fading out at 1/8 a frame, which would freeze bands of noise along the
 * path of anything that moves.
 */
static const uint16_t age_weight[16] = {
	KMAX, KMAX / 2, KMAX / 3, KMAX / 4, KMAX / 5, KMAX / 6, KMAX / 7, KMAX / 8,
	KMAX / 8, KMAX / 8, KMAX / 8, KMAX / 8, KMAX / 8, KMAX / 8, KMAX / 8, KMAX / 8,
};
#define MAX_THREADS 16

struct band {
	int y0, y1; /* luma rows [y0, y1), even */
};

struct cdn_cpu;
struct worker_arg {
	struct cdn_cpu *s;
	int idx;
};

struct cdn_cpu {
	int w, h;
	bool valid;

	/* history, 8.4 fixed point */
	int16_t *hy, *huv;
	/* scratch: |diff|, signed diff, its horizontal 5-sums, temporal
	 * weight (Q15), temporal outputs */
	uint8_t *dy;
	int8_t *ds;
	int16_t *hs, *hs9;
	uint16_t *ky;
	uint8_t *ty, *tuv, *tuv2;
	/* frames averaged into each luma pixel's history since motion reset it */
	uint8_t *age;

	/* per-frame parameters */
	int sig_y16, sig_c16;   /* median |frame - history|, x16 */
	int sig9_16;            /* median |9x9 sum of frame - history|, x16 */
	float sigma_y;          /* reported */
	int kmin;               /* Q15 */
	/* weight = kmin + (min(m - lo, range) * scale) >> shift */
	uint16_t lo, range, scale;		/* 3x3 sum of |d| */
	uint16_t alo, arange, ascale;		/* |5x5 sum of d| */
	uint16_t blo, brange, bscale;		/* |9x9 sum of d| */
	int shift, ashift, bshift;
	int16_t ts_y, ts_c;     /* spatial clip levels */
	int strength8;          /* 0..256 */

	const struct cdn_frame *in;
	struct cdn_frame *out;

	/* worker pool */
	int nthreads;
	pthread_t thr[MAX_THREADS];
	struct band band[MAX_THREADS];
	struct worker_arg wargs[MAX_THREADS];
	pthread_barrier_t start, phase, done;
	atomic_bool quit;
	bool use_neon;
};

static inline uint8_t sat_u8(int v)
{
	return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v;
}

/* vqrdmulhq_s16 for operands that cannot overflow */
static inline int16_t qrdmulh(int16_t a, int16_t b)
{
	return (int16_t)((2 * (int32_t)a * b + (1 << 15)) >> 16);
}

static inline int16_t clamp16(int v, int lo, int hi)
{
	return (int16_t)(v < lo ? lo : v > hi ? hi : v);
}

/* ---------------------------------------------------------------------- */
/* noise estimation (scalar, subsampled)                                   */

/* interpolated median of a 256-bin histogram, x16 */
static int hist_median16(const uint16_t *bins, uint32_t n)
{
	uint32_t acc = 0, half = n / 2;

	for (int i = 0; i < 256; i++) {
		acc += bins[i];
		if (acc > half) {
			uint32_t below = acc - bins[i];
			return i * 16 + (int)(((half - below) * 16) / bins[i]);
		}
	}
	return 255 * 16;
}

static int cmp_int(const void *a, const void *b)
{
	return *(const int *)a - *(const int *)b;
}

#define NB_X 16
#define NB_Y 9

/*
 * Noise scale: median of |frame - history|, x16. Camera ISP noise is heavy
 * tailed and spatially correlated, so every threshold is a multiple of this
 * robust scale, calibrated on static low-light J700 captures (std of the
 * difference = 1.75 x its median there).
 *
 * Moving areas inflate it, and an inflated scale turns motion into
 * "noise" and leaves ghosts. So take the median per block, over a 16x9
 * grid, and use the median over the blocks that are neither crushed black
 * nor clipped, so a large moving subject does not set the scale. (On the
 * J616s and J700 the 30th percentile ghosted no less and filtered less.)
 */
static int temporal_scale16(const uint8_t *src, int stride, const int16_t *hist,
			    int hstride, int w, int h, int step)
{
	int meds[NB_X * NB_Y], nm = 0;
	int bw = w / NB_X, bh = h / NB_Y;

	for (int by = 0; by < NB_Y; by++)
		for (int bx = 0; bx < NB_X; bx++) {
			uint16_t bins[256] = { 0 };
			uint32_t n = 0, sum = 0;

			for (int y = by * bh + 1; y < (by + 1) * bh - 1; y += 4)
				for (int x = bx * bw + 1; x < (bx + 1) * bw - 1; x += step) {
					int v = src[(size_t)y * stride + x];
					int p = sat_u8((hist[(size_t)y * hstride + x] + 8) >> 4);
					bins[abs(v - p)]++;
					sum += (uint32_t)v;
					n++;
				}
			if (!n)
				continue;
			uint32_t mean = sum / n;
			/* step 1 samples luma; chroma callers sample both
			 * components, whose mean sits near 128 */
			if (mean < 20 || mean > 245)
				continue;
			meds[nm++] = hist_median16(bins, n);
		}
	if (nm < 4)
		return 0;
	qsort(meds, (size_t)nm, sizeof(int), cmp_int);
	return meds[nm / 2];
}

static inline int8_t sat_s8(int v)
{
	return (int8_t)(v < -128 ? -128 : v > 127 ? 127 : v);
}

/*
 * Scale of the wide motion statistic: median of |9x9 sum of (frame -
 * history)|, x16, over a sparse grid, by block as above. Its relation to the
 * per-pixel scale depends on how far the sensor noise is correlated, which
 * differs between cameras and gains, so it is measured rather than derived.
 */
static int wide_scale16(const uint8_t *src, int stride, const int16_t *hist, int w, int h)
{
	int meds[NB_X * NB_Y], nm = 0;
	int bw = w / NB_X, bh = h / NB_Y;
	/* about 5x5 samples per block (120x120 at 1080p, 20x20 at 320x180) */
	int step = ((bw < bh ? bw : bh) - 8) / 5;

	if (step < 1)
		return 0;
	for (int by = 0; by < NB_Y; by++)
		for (int bx = 0; bx < NB_X; bx++) {
			int v[64], n = 0;
			uint32_t sum = 0, cnt = 0;

			for (int y = by * bh + 4; y < (by + 1) * bh - 4 && n < 64; y += step)
				for (int x = bx * bw + 4; x < (bx + 1) * bw - 4 && n < 64; x += step) {
					int acc = 0;
					for (int dy = -4; dy <= 4; dy++) {
						const uint8_t *sr = src + (size_t)(y + dy) * stride;
						const int16_t *hr = hist + (size_t)(y + dy) * w;
						for (int dx = -4; dx <= 4; dx++)
							acc += sat_s8((int)sr[x + dx] - sat_u8((hr[x + dx] + 8) >> 4));
					}
					v[n++] = abs(acc);
					sum += src[(size_t)y * stride + x];
					cnt++;
				}
			if (n < 8)
				continue;
			uint32_t mean = sum / cnt;
			if (mean < 20 || mean > 245)
				continue;
			qsort(v, (size_t)n, sizeof(int), cmp_int);
			meds[nm++] = (n & 1 ? v[n / 2] * 16 : (v[n / 2 - 1] + v[n / 2]) * 8);
		}
	if (nm < 4)
		return 0;
	qsort(meds, (size_t)nm, sizeof(int), cmp_int);
	return meds[nm / 2];
}

/* the largest shift (<= 16) whose scale still fits 16 bits */
static uint16_t weight_scale(int kmin, int range, int *shift)
{
	int sh = 16;
	uint64_t v;

	while ((v = ((uint64_t)(KMAX - kmin) << sh) / (uint64_t)range) > 65535)
		sh--;
	*shift = sh;
	return (uint16_t)v;
}

static void set_params(struct cdn_cpu *s)
{
	int med = s->sig_y16 < 8 ? 8 : s->sig_y16;	/* median |d|, x16 */
	int medc = s->sig_c16 < 8 ? 8 : s->sig_c16;

	s->kmin = KMAX / 8;
	/*
	 * Two motion detectors, each mapped to a weight between kmin (full
	 * averaging) and 1 (take the new frame). On static frames the 3x3
	 * sum of |d| stays under 24.5 medians for 95% of pixels (32.6 for
	 * 99%), and |5x5 sum of d| under 35.5 (57.6): noise mostly cancels
	 * in the signed sum, so it sees a dark object sliding over a dark
	 * background that |d| misses.
	 * Ramp from about the 90th to the 99th percentile of noise. A false
	 * trigger only restarts that pixel's running mean (see age_weight);
	 * a missed one leaves a ghost, which is far worse.
	 *   3x3 sum |d|:   full averaging below 20 medians, none from 32
	 *   |5x5 sum d|:   full averaging below 26 medians, none from 56
	 */
	int lo = (med * 320) >> 8, hi = (med * 512) >> 8;
	if (hi <= lo)
		hi = lo + 1;
	s->lo = (uint16_t)lo;
	s->range = (uint16_t)(hi - lo);
	s->scale = weight_scale(s->kmin, s->range, &s->shift);
	int alo = (med * 416) >> 8, ahi = (med * 896) >> 8;
	if (ahi <= alo)
		ahi = alo + 1;
	s->alo = (uint16_t)alo;
	s->arange = (uint16_t)(ahi - alo);
	s->ascale = weight_scale(s->kmin, s->arange, &s->ashift);
	/*
	 * |9x9 sum of d|: averages about three times as many samples as the
	 * 5x5 sum, so it sees a change of a few levels over an area. That is
	 * what a low-contrast object leaves behind: a faint copy of itself in
	 * the history, which the 3x3 and 5x5 tests miss and which then fades
	 * at 1/8 a frame, a trail. Its thresholds are multiples of its own
	 * measured scale (see wide_scale16), not of the per-pixel one, since
	 * how the two relate depends on how far the noise is correlated.
	 * Static noise crosses the low end now and then; that only restarts
	 * the pixel's running mean, and tools/cdn-motion.c measures the cost
	 * (static std up about 5%, trails down 2 to 3 times).
	 *   |9x9 sum d|:   full averaging below 3 of its medians (2 std),
	 *                  none from 6 (4 std)
	 */
	/* 9 medians is the value for uncorrelated noise, the least it can be */
	int med9 = s->sig9_16 < med * 9 ? med * 9 : s->sig9_16;
	int blo = (med9 * 48) >> 8, bhi = (med9 * 96) >> 8;
	if (bhi <= blo)
		bhi = blo + 1;
	s->blo = (uint16_t)blo;
	s->brange = (uint16_t)(bhi - blo);
	s->bscale = weight_scale(s->kmin, s->brange, &s->bshift);
	/* spatial clip: about 2 sigma on luma, 3 on chroma (sigma = 1.75 med) */
	s->ts_y = clamp16((med * 7 + 16) >> 5, 1, 64);
	s->ts_c = clamp16((medc * 21 + 32) >> 6, 1, 64);
}

/* ---------------------------------------------------------------------- */
/* pass 1: |Y - history|                                                   */

static void pass_diff(struct cdn_cpu *s, int y0, int y1)
{
	const struct cdn_frame *in = s->in;
	int w = s->w;

	for (int y = y0; y < y1; y++) {
		const uint8_t *src = in->y + (size_t)y * in->stride_y;
		const int16_t *h = s->hy + (size_t)y * w;
		uint8_t *d = s->dy + (size_t)y * w;
		int8_t *ds = s->ds + (size_t)y * w;
		int16_t *hs = s->hs + (size_t)y * w;
		int16_t *h9 = s->hs9 + (size_t)y * w;
		int x = 0;
#if CDN_NEON
		if (s->use_neon)
			for (; x + 16 <= w; x += 16) {
				uint8x16_t c = vld1q_u8(src + x);
				int16x8_t h0 = vld1q_s16(h + x), h1 = vld1q_s16(h + x + 8);
				uint8x16_t p = vcombine_u8(vqrshrun_n_s16(h0, 4),
							   vqrshrun_n_s16(h1, 4));
				vst1q_u8(d + x, vabdq_u8(c, p));
				int16x8_t d0 = vreinterpretq_s16_u16(vsubl_u8(vget_low_u8(c), vget_low_u8(p)));
				int16x8_t d1 = vreinterpretq_s16_u16(vsubl_u8(vget_high_u8(c), vget_high_u8(p)));
				vst1q_s8(ds + x, vcombine_s8(vqmovn_s16(d0), vqmovn_s16(d1)));
			}
#endif
		for (; x < w; x++) {
			int p = sat_u8((h[x] + 8) >> 4);
			d[x] = (uint8_t)abs((int)src[x] - p);
			ds[x] = sat_s8((int)src[x] - p);
		}

		/* horizontal 5-sums, edges replicated */
#define HS_SCALAR(X)                                                               \
	do {                                                                       \
		int acc = 0;                                                       \
		for (int dx = -2; dx <= 2; dx++) {                                 \
			int xx = (X) + dx;                                         \
			xx = xx < 0 ? 0 : xx > w - 1 ? w - 1 : xx;                 \
			acc += ds[xx];                                             \
		}                                                                  \
		hs[X] = (int16_t)acc;                                              \
	} while (0)
		HS_SCALAR(0);
		HS_SCALAR(1);
		x = 2;
#if CDN_NEON
		if (s->use_neon)
			for (; x + 8 <= w - 2; x += 8) {
				int16x8_t a = vmovl_s8(vld1_s8(ds + x - 2));
				a = vaddw_s8(a, vld1_s8(ds + x - 1));
				a = vaddw_s8(a, vld1_s8(ds + x));
				a = vaddw_s8(a, vld1_s8(ds + x + 1));
				a = vaddw_s8(a, vld1_s8(ds + x + 2));
				vst1q_s16(hs + x, a);
			}
#endif
		for (; x < w; x++)
			HS_SCALAR(x);
#undef HS_SCALAR

		/* horizontal 9-sums: two 5-sums overlapping at x */
#define H9_SCALAR(X)                                                               \
	do {                                                                       \
		int acc = 0;                                                       \
		for (int dx = -4; dx <= 4; dx++) {                                 \
			int xx = (X) + dx;                                         \
			xx = xx < 0 ? 0 : xx > w - 1 ? w - 1 : xx;                 \
			acc += ds[xx];                                             \
		}                                                                  \
		h9[X] = (int16_t)acc;                                              \
	} while (0)
		for (x = 0; x < 4; x++)
			H9_SCALAR(x);
#if CDN_NEON
		if (s->use_neon)
			for (; x + 8 <= w - 4; x += 8) {
				int16x8_t a = vaddq_s16(vld1q_s16(hs + x - 2), vld1q_s16(hs + x + 2));
				vst1q_s16(h9 + x, vsubq_s16(a, vmovl_s8(vld1_s8(ds + x))));
			}
#endif
		for (; x < w; x++)
			H9_SCALAR(x);
#undef H9_SCALAR
	}
}

/* ---------------------------------------------------------------------- */
/* pass 2: motion weight + luma temporal                                   */

static inline uint16_t ramp(int m, int lo, int range, int scale, int shift)
{
	int d = m - lo;
	if (d < 0)
		d = 0;
	if (d > range)
		d = range;
	return (uint16_t)(((uint32_t)d * (uint32_t)scale) >> shift);
}

static inline uint16_t weight_scalar(const struct cdn_cpu *s, int m, int a, int b)
{
	uint16_t wb = ramp(m, s->lo, s->range, s->scale, s->shift);
	uint16_t wa = ramp(a < 0 ? -a : a, s->alo, s->arange, s->ascale, s->ashift);
	uint16_t w9 = ramp(b < 0 ? -b : b, s->blo, s->brange, s->bscale, s->bshift);
	uint16_t wm = wa > wb ? wa : wb;
	return (uint16_t)(s->kmin + (w9 > wm ? w9 : wm));
}

static void pass_temporal_y(struct cdn_cpu *s, int y0, int y1)
{
	const struct cdn_frame *in = s->in;
	int w = s->w, h = s->h;

	for (int y = y0; y < y1; y++) {
		const uint8_t *src = in->y + (size_t)y * in->stride_y;
		int ya = y > 0 ? y - 1 : 0, yb = y < h - 1 ? y + 1 : h - 1;
		const uint8_t *da = s->dy + (size_t)ya * w, *db = s->dy + (size_t)y * w,
			      *dc = s->dy + (size_t)yb * w;
		const int16_t *hr[5], *h9[9];
		for (int j = 0; j < 5; j++) {
			int yy = y + j - 2;
			yy = yy < 0 ? 0 : yy > h - 1 ? h - 1 : yy;
			hr[j] = s->hs + (size_t)yy * w;
		}
		for (int j = 0; j < 9; j++) {
			int yy = y + j - 4;
			yy = yy < 0 ? 0 : yy > h - 1 ? h - 1 : yy;
			h9[j] = s->hs9 + (size_t)yy * w;
		}
		int16_t *hist = s->hy + (size_t)y * w;
		uint16_t *k = s->ky + (size_t)y * w;
		uint8_t *t = s->ty + (size_t)y * w;
		uint8_t *age = s->age + (size_t)y * w;
		int x = 0;

		/* edge columns and the scalar tail */
#define TEMPORAL_Y_SCALAR(X)                                                      \
	do {                                                                      \
		int xl = (X) > 0 ? (X) - 1 : 0, xr = (X) < w - 1 ? (X) + 1 : w - 1; \
		int m = da[xl] + da[X] + da[xr] + db[xl] + db[X] + db[xr] +      \
			dc[xl] + dc[X] + dc[xr];                                  \
		int av = hr[0][X] + hr[1][X] + hr[2][X] + hr[3][X] + hr[4][X];   \
		int bv = 0;                                                       \
		for (int j = 0; j < 9; j++)                                       \
			bv += h9[j][X];                                           \
		uint16_t km = weight_scalar(s, m, av, bv);                        \
		uint16_t ka = age_weight[age[X]];                                 \
		uint16_t kk = km > ka ? km : ka;                                  \
		age[X] = km > KMAX / 2 ? 1 : age[X] + 1 > AGE_MAX ? AGE_MAX : age[X] + 1; \
		int16_t delta = (int16_t)((src[X] << 4) - hist[X]);               \
		int16_t nh = (int16_t)(hist[X] + qrdmulh(delta, (int16_t)kk));    \
		hist[X] = nh;                                                     \
		k[X] = kk;                                                        \
		t[X] = sat_u8((nh + 8) >> 4);                                     \
	} while (0)

		TEMPORAL_Y_SCALAR(0);
		x = 1;
#if CDN_NEON
		if (s->use_neon) {
			uint16x8_t lo = vdupq_n_u16(s->lo), range = vdupq_n_u16(s->range);
			uint16x4_t scale = vdup_n_u16(s->scale);
			uint16x8_t alo = vdupq_n_u16(s->alo), arange = vdupq_n_u16(s->arange);
			uint16x4_t ascale = vdup_n_u16(s->ascale);
			uint16x8_t kmin = vdupq_n_u16((uint16_t)s->kmin);
			int32x4_t nsh = vdupq_n_s32(-s->shift), nash = vdupq_n_s32(-s->ashift);
			uint16x8_t blo = vdupq_n_u16(s->blo), brange = vdupq_n_u16(s->brange);
			uint16x4_t bscale = vdup_n_u16(s->bscale);
			int32x4_t nbsh = vdupq_n_s32(-s->bshift);
			uint8_t tlo[16], thi[16];
			for (int i = 0; i < 16; i++) {
				tlo[i] = (uint8_t)age_weight[i];
				thi[i] = (uint8_t)(age_weight[i] >> 8);
			}
			uint8x16_t rlo = vld1q_u8(tlo), rhi = vld1q_u8(thi);
			uint16x8_t half = vdupq_n_u16(KMAX / 2);
			uint8x8_t one = vdup_n_u8(1), amax = vdup_n_u8(AGE_MAX);
			for (; x + 8 <= w - 1; x += 8) {
				/* 3x3 sum in u16 */
				uint16x8_t m = vdupq_n_u16(0);
				for (int dx = -1; dx <= 1; dx++) {
					m = vaddw_u8(m, vld1_u8(da + x + dx));
					m = vaddw_u8(m, vld1_u8(db + x + dx));
					m = vaddw_u8(m, vld1_u8(dc + x + dx));
				}
				uint16x8_t d = vminq_u16(vqsubq_u16(m, lo), range);
				uint32x4_t p0 = vmull_u16(vget_low_u16(d), scale);
				uint32x4_t p1 = vmull_u16(vget_high_u16(d), scale);
				uint16x8_t wb = vcombine_u16(vmovn_u32(vshlq_u32(p0, nsh)),
							     vmovn_u32(vshlq_u32(p1, nsh)));
				int16x8_t av = vaddq_s16(vaddq_s16(vld1q_s16(hr[0] + x), vld1q_s16(hr[1] + x)),
							 vaddq_s16(vld1q_s16(hr[2] + x), vld1q_s16(hr[3] + x)));
				av = vaddq_s16(av, vld1q_s16(hr[4] + x));
				uint16x8_t aa = vreinterpretq_u16_s16(vabsq_s16(av));
				uint16x8_t e = vminq_u16(vqsubq_u16(aa, alo), arange);
				uint32x4_t q0 = vmull_u16(vget_low_u16(e), ascale);
				uint32x4_t q1 = vmull_u16(vget_high_u16(e), ascale);
				uint16x8_t wa = vcombine_u16(vmovn_u32(vshlq_u32(q0, nash)),
							     vmovn_u32(vshlq_u32(q1, nash)));
				int16x8_t bv = vld1q_s16(h9[0] + x);
				for (int j = 1; j < 9; j++)
					bv = vaddq_s16(bv, vld1q_s16(h9[j] + x));
				uint16x8_t ba = vreinterpretq_u16_s16(vabsq_s16(bv));
				uint16x8_t f = vminq_u16(vqsubq_u16(ba, blo), brange);
				uint32x4_t r0 = vmull_u16(vget_low_u16(f), bscale);
				uint32x4_t r1 = vmull_u16(vget_high_u16(f), bscale);
				uint16x8_t w9 = vcombine_u16(vmovn_u32(vshlq_u32(r0, nbsh)),
							     vmovn_u32(vshlq_u32(r1, nbsh)));
				uint16x8_t km = vaddq_u16(kmin, vmaxq_u16(w9, vmaxq_u16(wa, wb)));
				uint8x8_t ag = vld1_u8(age + x);
				uint16x8_t ka = vorrq_u16(vmovl_u8(vqtbl1_u8(rlo, ag)),
							  vshlq_n_u16(vmovl_u8(vqtbl1_u8(rhi, ag)), 8));
				uint8x8_t moving = vmovn_u16(vcgtq_u16(km, half));
				vst1_u8(age + x, vbsl_u8(moving, one, vmin_u8(vadd_u8(ag, one), amax)));
				uint16x8_t kk = vmaxq_u16(km, ka);
				int16x8_t hv = vld1q_s16(hist + x);
				int16x8_t cur = vreinterpretq_s16_u16(vshll_n_u8(vld1_u8(src + x), 4));
				int16x8_t delta = vsubq_s16(cur, hv);
				int16x8_t nh = vaddq_s16(hv, vqrdmulhq_s16(delta, vreinterpretq_s16_u16(kk)));
				vst1q_s16(hist + x, nh);
				vst1q_u16(k + x, kk);
				vst1_u8(t + x, vqrshrun_n_s16(nh, 4));
			}
		}
#endif
		for (; x < w; x++)
			TEMPORAL_Y_SCALAR(x);
#undef TEMPORAL_Y_SCALAR
	}
}

/* ---------------------------------------------------------------------- */
/* pass 3: luma spatial (edge-clipped 3x3 mean) + blend                    */

static void pass_spatial_y(struct cdn_cpu *s, int y0, int y1)
{
	const struct cdn_frame *in = s->in;
	struct cdn_frame *out = s->out;
	int w = s->w, h = s->h, str = s->strength8;
	int16_t tsf = s->ts_y;          /* full clip level */
	int16_t tfloor = (int16_t)((tsf + 3) >> 2);

	for (int y = y0; y < y1; y++) {
		const uint8_t *src = in->y + (size_t)y * in->stride_y;
		uint8_t *dst = out->y + (size_t)y * out->stride_y;
		const uint8_t *t = s->ty + (size_t)y * w;
		const uint16_t *k = s->ky + (size_t)y * w;

		if (y == 0 || y == h - 1) {
			for (int x = 0; x < w; x++)
				dst[x] = sat_u8(src[x] + (((t[x] - src[x]) * str + 128) >> 8));
			continue;
		}
		const uint8_t *ta = t - w, *tc = t + w;
		int x = 0;

#define SPATIAL_Y_SCALAR(X)                                                          \
	do {                                                                         \
		int c = t[X], v;                                                     \
		if ((X) == 0 || (X) == w - 1) {                                      \
			v = c;                                                       \
		} else {                                                             \
			int16_t tt = qrdmulh(tsf, (int16_t)k[X]);                    \
			if (tt < tfloor)                                             \
				tt = tfloor;                                         \
			int sum = clamp16(ta[(X) - 1] - c, -tt, tt) +                \
				  clamp16(ta[X] - c, -tt, tt) +                      \
				  clamp16(ta[(X) + 1] - c, -tt, tt) +                \
				  clamp16(t[(X) - 1] - c, -tt, tt) +                 \
				  clamp16(t[(X) + 1] - c, -tt, tt) +                 \
				  clamp16(tc[(X) - 1] - c, -tt, tt) +                \
				  clamp16(tc[X] - c, -tt, tt) +                      \
				  clamp16(tc[(X) + 1] - c, -tt, tt);                 \
			v = sat_u8(c + qrdmulh((int16_t)sum, 3641));                 \
		}                                                                    \
		dst[X] = sat_u8(src[X] + (((v - src[X]) * str + 128) >> 8));        \
	} while (0)

		SPATIAL_Y_SCALAR(0);
		x = 1;
#if CDN_NEON
		if (s->use_neon) {
			int16x8_t vts = vdupq_n_s16(tsf), vfl = vdupq_n_s16(tfloor);
			int16x8_t nine = vdupq_n_s16(3641);
			int16x8_t vstr = vdupq_n_s16((int16_t)str);
			for (; x + 8 <= w - 1; x += 8) {
				int16x8_t c = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(t + x)));
				int16x8_t tt = vmaxq_s16(vqrdmulhq_s16(vts, vreinterpretq_s16_u16(vld1q_u16(k + x))), vfl);
				int16x8_t ntt = vnegq_s16(tt);
				int16x8_t sum = vdupq_n_s16(0);
#define ACC(P)                                                                         \
	sum = vaddq_s16(sum, vmaxq_s16(vminq_s16(vsubq_s16(                             \
		vreinterpretq_s16_u16(vmovl_u8(vld1_u8(P))), c), tt), ntt))
				ACC(ta + x - 1); ACC(ta + x); ACC(ta + x + 1);
				ACC(t + x - 1); ACC(t + x + 1);
				ACC(tc + x - 1); ACC(tc + x); ACC(tc + x + 1);
#undef ACC
				int16x8_t v = vreinterpretq_s16_u16(vmovl_u8(
					vqmovun_s16(vaddq_s16(c, vqrdmulhq_s16(sum, nine)))));
				int16x8_t sv = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(src + x)));
				/* src + ((v - src) * str + 128) >> 8 */
				int16x8_t dd = vsubq_s16(v, sv);
				int32x4_t a0 = vmull_s16(vget_low_s16(dd), vget_low_s16(vstr));
				int32x4_t a1 = vmull_s16(vget_high_s16(dd), vget_high_s16(vstr));
				int16x8_t adj = vcombine_s16(vrshrn_n_s32(a0, 8), vrshrn_n_s32(a1, 8));
				vst1_u8(dst + x, vqmovun_s16(vaddq_s16(sv, adj)));
			}
		}
#endif
		for (; x < w; x++)
			SPATIAL_Y_SCALAR(x);
#undef SPATIAL_Y_SCALAR
	}
}

/* ---------------------------------------------------------------------- */
/* pass 4: chroma temporal, weight = max over the 2x2 luma block            */

static void pass_temporal_uv(struct cdn_cpu *s, int y0, int y1)
{
	const struct cdn_frame *in = s->in;
	int w = s->w;

	for (int cy = y0 / 2; cy < y1 / 2; cy++) {
		const uint8_t *src = in->uv + (size_t)cy * in->stride_uv;
		const uint16_t *k0 = s->ky + (size_t)(2 * cy) * w, *k1 = k0 + w;
		int16_t *hist = s->huv + (size_t)cy * w;
		uint8_t *t = s->tuv + (size_t)cy * w;
		int x = 0;
#if CDN_NEON
		if (s->use_neon)
			for (; x + 8 <= w; x += 8) {
				uint16x8_t a = vmaxq_u16(vld1q_u16(k0 + x), vld1q_u16(k1 + x));
				/* pairwise max -> one weight per CbCr pair, used for both */
				uint16x4_t pm = vpmax_u16(vget_low_u16(a), vget_high_u16(a));
				uint16x4x2_t z = vzip_u16(pm, pm);
				int16x8_t kk = vreinterpretq_s16_u16(vcombine_u16(z.val[0], z.val[1]));
				int16x8_t hv = vld1q_s16(hist + x);
				int16x8_t cur = vreinterpretq_s16_u16(vshll_n_u8(vld1_u8(src + x), 4));
				int16x8_t nh = vaddq_s16(hv, vqrdmulhq_s16(vsubq_s16(cur, hv), kk));
				vst1q_s16(hist + x, nh);
				vst1_u8(t + x, vqrshrun_n_s16(nh, 4));
			}
#endif
		for (; x < w; x++) {
			int p = x & ~1;
			uint16_t a = k0[p] > k1[p] ? k0[p] : k1[p];
			uint16_t b = k0[p + 1] > k1[p + 1] ? k0[p + 1] : k1[p + 1];
			int16_t kk = (int16_t)(a > b ? a : b);
			int16_t delta = (int16_t)((src[x] << 4) - hist[x]);
			int16_t nh = (int16_t)(hist[x] + qrdmulh(delta, kk));
			hist[x] = nh;
			t[x] = sat_u8((nh + 8) >> 4);
		}
	}
}

/* ---------------------------------------------------------------------- */
/* pass 5/6: chroma spatial, edge-clipped, at sample distance dist          */

static void pass_spatial_uv(struct cdn_cpu *s, int y0, int y1, int dist,
			    const uint8_t *srcp, uint8_t *dstp, bool final)
{
	const struct cdn_frame *in = s->in;
	struct cdn_frame *out = s->out;
	int w = s->w, ch = s->h / 2, str = s->strength8;
	int bo = 2 * dist; /* byte offset of the same component */
	int16_t tt = s->ts_c;

	for (int cy = y0 / 2; cy < y1 / 2; cy++) {
		const uint8_t *t = srcp + (size_t)cy * w;
		uint8_t *dst = final ? out->uv + (size_t)cy * out->stride_uv : dstp + (size_t)cy * w;
		const uint8_t *orig = in->uv + (size_t)cy * in->stride_uv;
		bool edge_row = cy < dist || cy >= ch - dist;
		const uint8_t *ta = edge_row ? t : t - (size_t)dist * w;
		const uint8_t *tc = edge_row ? t : t + (size_t)dist * w;
		int x = 0;

#define SPATIAL_UV_SCALAR(X)                                                         \
	do {                                                                         \
		int c = t[X], v;                                                     \
		if (edge_row || (X) < bo || (X) >= w - bo) {                         \
			v = c;                                                       \
		} else {                                                             \
			int sum = clamp16(ta[(X) - bo] - c, -tt, tt) +               \
				  clamp16(ta[X] - c, -tt, tt) +                      \
				  clamp16(ta[(X) + bo] - c, -tt, tt) +               \
				  clamp16(t[(X) - bo] - c, -tt, tt) +                \
				  clamp16(t[(X) + bo] - c, -tt, tt) +                \
				  clamp16(tc[(X) - bo] - c, -tt, tt) +               \
				  clamp16(tc[X] - c, -tt, tt) +                      \
				  clamp16(tc[(X) + bo] - c, -tt, tt);                \
			v = sat_u8(c + qrdmulh((int16_t)sum, 3641));                 \
		}                                                                    \
		if (final)                                                           \
			dst[X] = sat_u8(orig[X] + (((v - orig[X]) * str + 128) >> 8));\
		else                                                                 \
			dst[X] = (uint8_t)v;                                         \
	} while (0)

		for (; x < bo && x < w; x++)
			SPATIAL_UV_SCALAR(x);
#if CDN_NEON
		if (s->use_neon && !edge_row) {
			int16x8_t vt = vdupq_n_s16(tt), nvt = vdupq_n_s16((int16_t)-tt);
			int16x8_t nine = vdupq_n_s16(3641), vstr = vdupq_n_s16((int16_t)str);
			for (; x + 8 <= w - bo; x += 8) {
				int16x8_t c = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(t + x)));
				int16x8_t sum = vdupq_n_s16(0);
#define ACC(P)                                                                         \
	sum = vaddq_s16(sum, vmaxq_s16(vminq_s16(vsubq_s16(                             \
		vreinterpretq_s16_u16(vmovl_u8(vld1_u8(P))), c), vt), nvt))
				ACC(ta + x - bo); ACC(ta + x); ACC(ta + x + bo);
				ACC(t + x - bo); ACC(t + x + bo);
				ACC(tc + x - bo); ACC(tc + x); ACC(tc + x + bo);
#undef ACC
				uint8x8_t v8 = vqmovun_s16(vaddq_s16(c, vqrdmulhq_s16(sum, nine)));
				if (final) {
					int16x8_t v = vreinterpretq_s16_u16(vmovl_u8(v8));
					int16x8_t sv = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(orig + x)));
					int16x8_t dd = vsubq_s16(v, sv);
					int32x4_t a0 = vmull_s16(vget_low_s16(dd), vget_low_s16(vstr));
					int32x4_t a1 = vmull_s16(vget_high_s16(dd), vget_high_s16(vstr));
					int16x8_t adj = vcombine_s16(vrshrn_n_s32(a0, 8), vrshrn_n_s32(a1, 8));
					vst1_u8(dst + x, vqmovun_s16(vaddq_s16(sv, adj)));
				} else {
					vst1_u8(dst + x, v8);
				}
			}
		}
#endif
		for (; x < w; x++)
			SPATIAL_UV_SCALAR(x);
#undef SPATIAL_UV_SCALAR
	}
}

/* ---------------------------------------------------------------------- */
/* threading                                                               */

static void run_phase(struct cdn_cpu *s, int phase, int y0, int y1)
{
	switch (phase) {
	case 0: pass_diff(s, y0, y1); break;
	case 1: pass_temporal_y(s, y0, y1); break;
	case 2:
		pass_spatial_y(s, y0, y1);
		pass_temporal_uv(s, y0, y1);
		break;
	case 3: pass_spatial_uv(s, y0, y1, 1, s->tuv, s->tuv2, false); break;
	case 4: pass_spatial_uv(s, y0, y1, 2, s->tuv2, NULL, true); break;
	}
}

#define NPHASES 5

static void *worker(void *p)
{
	struct worker_arg *a = p;
	struct cdn_cpu *s = a->s;

	for (;;) {
		pthread_barrier_wait(&s->start);
		if (atomic_load(&s->quit))
			return NULL;
		for (int ph = 0; ph < NPHASES; ph++) {
			run_phase(s, ph, s->band[a->idx].y0, s->band[a->idx].y1);
			pthread_barrier_wait(&s->phase);
		}
		pthread_barrier_wait(&s->done);
	}
}

static void run_all(struct cdn_cpu *s)
{
	if (s->nthreads <= 1) {
		for (int ph = 0; ph < NPHASES; ph++)
			run_phase(s, ph, 0, s->h);
		return;
	}
	pthread_barrier_wait(&s->start);
	for (int ph = 0; ph < NPHASES; ph++) {
		run_phase(s, ph, s->band[0].y0, s->band[0].y1);
		pthread_barrier_wait(&s->phase);
	}
	pthread_barrier_wait(&s->done);
}

/* ---------------------------------------------------------------------- */
/* public                                                                  */

static void *cpu_create(int width, int height, int threads)
{
	struct cdn_cpu *s;

	if (width < 16 || height < 16 || (width & 1) || (height & 1))
		return NULL;
	s = calloc(1, sizeof(*s));
	if (!s)
		return NULL;
	s->w = width;
	s->h = height;
	size_t n = (size_t)width * height;
	s->hy = malloc(n * 2);
	s->huv = malloc(n);
	s->dy = malloc(n);
	s->ds = malloc(n);
	s->hs = malloc(n * 2);
	s->hs9 = malloc(n * 2);
	s->ky = malloc(n * 2);
	s->age = malloc(n);
	s->ty = malloc(n);
	s->tuv = malloc(n / 2);
	s->tuv2 = malloc(n / 2);
	if (!s->hy || !s->huv || !s->dy || !s->ds || !s->hs || !s->hs9 || !s->ky || !s->age || !s->ty || !s->tuv || !s->tuv2) {
		cdn_cpu_destroy(s);
		return NULL;
	}
	s->use_neon = CDN_NEON;
	s->sig_y16 = 2 * 16;
	s->sig_c16 = 1 * 16;
	s->sig9_16 = 0;
	s->strength8 = 256;

	/*
	 * One thread by default: it costs the least CPU time. A 1080p frame
	 * takes about 3 ms on one J700 or J616s core; four threads on the J616s
	 * cut that to 1.2 ms for about 15% more CPU time.
	 */
	if (threads <= 0)
		threads = 1;
	if (threads > MAX_THREADS)
		threads = MAX_THREADS;
	/* bands of even rows */
	int rows = height / threads;
	rows &= ~1;
	if (rows < 8) {
		threads = 1;
		rows = height;
	}
	s->nthreads = threads;
	for (int i = 0; i < threads; i++) {
		s->band[i].y0 = i * rows;
		s->band[i].y1 = i == threads - 1 ? height : (i + 1) * rows;
	}
	if (threads > 1) {
		pthread_barrier_init(&s->start, NULL, threads);
		pthread_barrier_init(&s->phase, NULL, threads);
		pthread_barrier_init(&s->done, NULL, threads);
		for (int i = 1; i < threads; i++) {
			s->wargs[i].s = s;
			s->wargs[i].idx = i;
			pthread_create(&s->thr[i], NULL, worker, &s->wargs[i]);
		}
	}
	return s;
}

void cdn_cpu_destroy(void *p)
{
	struct cdn_cpu *s = p;

	if (!s)
		return;
	if (s->nthreads > 1) {
		atomic_store(&s->quit, true);
		pthread_barrier_wait(&s->start);
		for (int i = 1; i < s->nthreads; i++)
			pthread_join(s->thr[i], NULL);
		pthread_barrier_destroy(&s->start);
		pthread_barrier_destroy(&s->phase);
		pthread_barrier_destroy(&s->done);
	}
	free(s->hy);
	free(s->huv);
	free(s->dy);
	free(s->ds);
	free(s->hs);
	free(s->hs9);
	free(s->ky);
	free(s->age);
	free(s->ty);
	free(s->tuv);
	free(s->tuv2);
	free(s);
}

static void cpu_reset(void *p)
{
	((struct cdn_cpu *)p)->valid = false;
}

static void seed_history(struct cdn_cpu *s, const struct cdn_frame *in)
{
	for (int y = 0; y < s->h; y++)
		for (int x = 0; x < s->w; x++)
			s->hy[(size_t)y * s->w + x] = (int16_t)(in->y[(size_t)y * in->stride_y + x] << 4);
	for (int y = 0; y < s->h / 2; y++)
		for (int x = 0; x < s->w; x++)
			s->huv[(size_t)y * s->w + x] = (int16_t)(in->uv[(size_t)y * in->stride_uv + x] << 4);
	memset(s->age, 1, (size_t)s->w * s->h);
	s->valid = true;
}

static void update_noise(struct cdn_cpu *s, const struct cdn_frame *in)
{
	int my = temporal_scale16(in->y, in->stride_y, s->hy, s->w, s->w, s->h, 2);
	int mc = temporal_scale16(in->uv, in->stride_uv, s->huv, s->w, s->w, s->h / 2, 3);
	int m9 = wide_scale16(in->y, in->stride_y, s->hy, s->w, s->h);

	/* smooth: the estimate is noisy frame to frame */
	s->sig_y16 += (my - s->sig_y16) / 4;
	s->sig_c16 += (mc - s->sig_c16) / 4;
	s->sig9_16 += (m9 - s->sig9_16) / 4;
	s->sigma_y = s->sig_y16 * 1.75f / 16.0f;
}

int cdn_cpu_process_ex(void *p, const struct cdn_frame *in, struct cdn_frame *out,
		       float strength, bool neon)
{
	struct cdn_cpu *s = p;

	if (in->width != s->w || in->height != s->h || out->width != s->w ||
	    out->height != s->h)
		return -EINVAL;
	s->use_neon = neon && CDN_NEON;
	if (!s->valid) {
		seed_history(s, in);
		/* the first frame has no history: pass it through */
		cdn_frame_copy(in, out);
		return 0;
	}
	if (strength < 0)
		strength = 0;
	if (strength > 1)
		strength = 1;
	s->strength8 = (int)(strength * 256.0f + 0.5f);
	update_noise(s, in);
	set_params(s);
	s->in = in;
	s->out = out;
	run_all(s);
	return 0;
}

static int cpu_process(void *p, const struct cdn_frame *in, struct cdn_frame *out,
		       const struct cdn_frame_info *info, float strength)
{
	(void)info;
	return cdn_cpu_process_ex(p, in, out, strength, true);
}

static float cpu_noise_sigma(void *p)
{
	return ((struct cdn_cpu *)p)->sigma_y;
}

void *cdn_cpu_create(int width, int height, int threads)
{
	return cpu_create(width, height, threads);
}

static const struct cdn_backend_ops cpu_ops = {
	.abi = CDN_BACKEND_ABI,
	.name = "cpu",
	.create = cpu_create,
	.process = cpu_process,
	.reset = cpu_reset,
	.noise_sigma = cpu_noise_sigma,
	.destroy = cdn_cpu_destroy,
};

const struct cdn_backend_ops *cdn_cpu_backend(void)
{
	return &cpu_ops;
}
