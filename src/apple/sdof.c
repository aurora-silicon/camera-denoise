// SPDX-License-Identifier: MIT
/* Portrait's ray pass in C (see sdof.h). The operation order matters for the match with the reference. */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sdof.h"

/* The shader's half (IEEE binary16), for its rounding. */
typedef _Float16 h16;

/* Portrait's function constants for the pass. */
#define RAYS 19
#define RADIUS_LOCAL 0.25f
#define RADIUS_LOCAL_FRACTION 0.25f
#define DIAMETER_COVERAGE_LIMIT 2.0f
#define FOCUS_BLEND_A (-0.5f)
#define FOCUS_BLEND_B (-2.5f)
#define PYRAMID_SAMPLING_FRACTION 0.6666667f
#define SKIP_FULL_SIZE_LAYER 1
#define HIGHLIGHT_BOOST 0.02f
/* The disparity scale and the default focus (no face: offset -1.5). */
#define DISPARITY_SCALE 1.94097f

/* The jitter table: which aperture point perturbs ray i at a pixel (as the reference uses it). */
static const uint8_t ray_jitter[128] = {
	17, 17, 16, 10, 11, 2, 6, 18, 13, 12, 0, 5, 3, 1, 17, 11, 16, 1, 10, 16, 11, 9, 16, 1, 3, 0, 10, 9,
	5, 0, 15, 0, 13, 3, 18, 11, 17, 9, 12, 13, 10, 8, 4, 14, 3, 14, 2, 15, 15, 10, 3, 8, 1, 12, 2, 13,
	13, 0, 6, 2, 14, 17, 2, 15, 17, 12, 8, 4, 16, 1, 7, 13, 6, 14, 12, 14, 5, 10, 7, 0, 18, 1, 9, 4,
	8, 18, 7, 18, 17, 5, 8, 4, 1, 7, 14, 7, 12, 10, 3, 11, 9, 18, 2, 8, 5, 0, 9, 12, 6, 7, 6, 6, 4,
	14, 9, 5, 4, 2, 8, 6, 5, 16, 15, 13, 11, 4, 15, 16,
};

/*
 * The aperture: centre, a ring of 6 at radius 0.5 (from 0 deg, every 60 deg, clockwise in y-down), 12 at 1;
 * half-precision values, as the reference stores them.
 */
static void aperture_points(float pts[RAYS][2])
{
	static const float ring6[6][2] = {
		{ 0.5f, 0.0f }, { 0.25f, -0.43310547f }, { -0.25f, -0.43310547f },
		{ -0.5f, 0.0f }, { -0.25f, 0.43310547f }, { 0.25f, 0.43310547f },
	};
	static const float ring12[12][2] = {
		{ 0.5f, 0.8662109f }, { 0.0f, 1.0f }, { -0.5f, 0.8662109f }, { -0.8662109f, 0.5f },
		{ -1.0f, 0.0f }, { -0.8662109f, -0.5f }, { -0.5f, -0.8662109f }, { 0.0f, -1.0f },
		{ 0.5f, -0.8662109f }, { 0.8662109f, -0.5f }, { 1.0f, 0.0f }, { 0.8662109f, 0.5f },
	};
	pts[0][0] = pts[0][1] = 0;
	memcpy(pts[1], ring6, sizeof(ring6));
	memcpy(pts[7], ring12, sizeof(ring12));
}

/* 1 / f0 from the aperture: linear to 0.6 (fit to 0.1 and 0.5: 110.062, 150.456), then measured. */
static float inverse_f0(float a)
{
	static const float knee[][2] = {
		{ 0.6f, 160.554f }, { 0.7f, 178.85f }, { 0.8f, 220.59f }, { 0.9f, 307.31f }, { 0.95f, 377.89f },
		{ 1.0f, 473.37f },
	};
	if (a <= 0.6f)
		return 99.963f + 100.985f * (a < 0 ? 0 : a);
	for (unsigned i = 1; i < sizeof(knee) / sizeof(knee[0]); i++)
		if (a <= knee[i][0]) {
			float t = (a - knee[i - 1][0]) / (knee[i][0] - knee[i - 1][0]);
			return knee[i - 1][1] + t * (knee[i][1] - knee[i - 1][1]);
		}
	return knee[5][1];
}

void sdof_params_for(struct sdof_params *p, float aperture, int frame_w)
{
	p->f0 = 1.0f / inverse_f0(aperture);
	p->f1x = p->f0 * 0.8333333f;
	p->f1y = p->f0 * 2.1333333f;
	p->f2x = -0.375309f;
	p->f2y = -0.211111f;
	p->frame_w = (float)frame_w;
}

float sdof_focus_offset(float focus_disparity)
{
	return -DISPARITY_SCALE * focus_disparity;
}

float sdof_srgb_to_linear(float v)
{
	return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
}

float sdof_linear_to_srgb(float v)
{
	if (v <= 0)
		return 0;
	if (v >= 1)
		return 1;
	return v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
}

/* mirrored_repeat on a texel index. */
static int mirror(int i, int n)
{
	int period = 2 * n;

	i %= period;
	if (i < 0)
		i += period;
	return i < n ? i : period - 1 - i;
}

/* Bilinear on normalized coordinates, mirrored_repeat addressing (Metal's linear filter). */
static void bilinear(const float *img, int w, int h, int ch, float u, float v, float *out)
{
	float x = u * w - 0.5f, y = v * h - 0.5f;
	int x0 = (int)floorf(x), y0 = (int)floorf(y);
	float fx = x - x0, fy = y - y0;
	int xa = mirror(x0, w), xb = mirror(x0 + 1, w), ya = mirror(y0, h), yb = mirror(y0 + 1, h);

	for (int c = 0; c < ch; c++) {
		float a = img[(ya * w + xa) * ch + c], b = img[(ya * w + xb) * ch + c];
		float d = img[(yb * w + xa) * ch + c], e = img[(yb * w + xb) * ch + c];
		out[c] = (a + (b - a) * fx) + ((d + (e - d) * fx) - (a + (b - a) * fx)) * fy;
	}
}

/* Trilinear on the pyramid at a level of detail (linear mip filter, clamped to the levels there are). */
static void sample_lod(const struct sdof_level *l, int n, float u, float v, float lod, float out[4])
{
	float a[4], b[4];
	int i;

	if (lod <= 0) {
		bilinear(l[0].rgba, l[0].w, l[0].h, 4, u, v, out);
		return;
	}
	if (lod >= n - 1) {
		bilinear(l[n - 1].rgba, l[n - 1].w, l[n - 1].h, 4, u, v, out);
		return;
	}
	i = (int)floorf(lod);
	bilinear(l[i].rgba, l[i].w, l[i].h, 4, u, v, a);
	bilinear(l[i + 1].rgba, l[i + 1].w, l[i + 1].h, 4, u, v, b);
	for (int c = 0; c < 4; c++)
		out[c] = a[c] + (b[c] - a[c]) * (lod - i);
}

static float saturate(float v)
{
	return v < 0 ? 0 : v > 1 ? 1 : v;
}

/* Metal's fast_fmod: x - y * trunc(x / y). */
static float fmod_fast(float x, float y)
{
	return x - y * truncf(x / y);
}

static float fract(float v)
{
	return v - floorf(v);
}

/* The per-pixel rotation of the aperture, a hash of the position. */
static float hash_angle(float x, float y)
{
	float a = fmod_fast(y * 75.833f - x * x * 37.135f, 0.014f);
	float b = fmod_fast(x * 12.63f - y * y * 57.3f, 0.01369f);
	float c = fmod_fast((y * 39.7f + y * y * 21.7f) * x, 0.0879f);
	union { uint32_t u; float f; } k = { 0x455F8BEDu };

	return fract((a + b + c) * k.f) * 6.2831855f;
}

void sdof_raytrace(const struct sdof_level *levels, int nlevels, const float *disp, int dw, int dh,
		   const struct sdof_params *p, float *out, int ow, int oh)
{
	float pts[RAYS][2];
	const float c34 = 1.0f / (PYRAMID_SAMPLING_FRACTION * RADIUS_LOCAL * sqrtf(2.0f) + 1.0f);
	const float c39 = PYRAMID_SAMPLING_FRACTION * 2.0f / sqrtf((float)RAYS);
	const float c45 = p->frame_w * p->f0 * c39;
	const float boost = HIGHLIGHT_BOOST / c39;
	const float jitter = RADIUS_LOCAL * RADIUS_LOCAL_FRACTION;
	const int step_mod = 101 / RAYS;

	aperture_points(pts);
	for (int gy = 0; gy < oh; gy++)
		for (int gx = 0; gx < ow; gx++) {
			float u = (gx + 0.5f) / ow, v = (gy + 0.5f) / oh;
			float vx = (u - 0.5f) * 2.0f * p->f2x, vy = (v - 0.5f) * 2.0f * p->f2y;
			float r = sqrtf(vx * 0.5f * vx * 0.5f + vy * 0.5f * vy * 0.5f);
			float b0[2] = { 1, 0 }, b1[2] = { 0, 1 };
			float *o4 = out + ((size_t)gy * ow + gx) * 4;

			if (!(r < 1e-5f)) {
				float ang = -atan2f(vy, vx), s = sinf(ang), c = cosf(ang), q = sqrtf(1 - r * r);

				b0[0] = c * (1 - r);
				b0[1] = -s * (1 - r);
				b1[0] = s * q;
				b1[1] = c * q;
			}
			float th = hash_angle(u, v), sn = sinf(th), cs = cosf(th);
			float R0[2] = { cs * b0[0] - sn * b1[0], cs * b0[1] - sn * b1[1] };
			float R1[2] = { sn * b0[0] + cs * b1[0], sn * b0[1] + cs * b1[1] };
			float B0[2] = { R0[0] * p->f1x, R0[1] * p->f1y }, B1[2] = { R1[0] * p->f1x, R1[1] * p->f1y };
			int pix = gx * oh + gy, k = (gx + gy) % step_mod, n = 0, done = 0;
			float sum[3] = { 0, 0, 0 }, wsum[3] = { 0, 0, 0 }, rsum = 0, dprev = 0;

			/* Half precision where the shader computes in half: offsets, samples, radii, the sums. */
			h16 hs[3] = { 0, 0, 0 }, hw[3] = { 0, 0, 0 }, hr = 0, hprev = 0, hboost = (h16)boost;

			(void)sum; (void)wsum; (void)rsum; (void)dprev;
			for (int i = 0; i < RAYS && !done; i++) {
				int j = ray_jitter[(pix + i * k) % 128];
				float px = pts[i][0] + jitter * pts[j][0], py = pts[i][1] + jitter * pts[j][1];
				h16 ox = (h16)(px * B0[0] + py * B1[0]), oy = (h16)(px * B0[1] + py * B1[1]);
				float d;

				bilinear(disp, dw, dh, 1, u + (float)(h16)(ox * hprev), v + (float)(h16)(oy * hprev), &d);
				h16 hd = (h16)d;

				hprev = hd;
				float su = (float)(h16)(hd * ox) * c34 + u, sv = (float)(h16)(hd * oy) * c34 + v;

				if (su < 0 || sv < 0 || su >= 1 || sv >= 1 || hd == (h16)65504.0f)
					continue;
				h16 rad = (h16)fabsf(c45 * (float)hd);
				float col[4];
				float lod = (DIAMETER_COVERAGE_LIMIT < (float)rad ? (float)(h16)log2f((float)rad) :
					     log2f(DIAMETER_COVERAGE_LIMIT)) - SKIP_FULL_SIZE_LAYER;

				sample_lod(levels, nlevels, su, sv, lod, col);
				h16 hc[3] = { (h16)col[0], (h16)col[1], (h16)col[2] };

				if (i == 0 && (h16)saturate((FOCUS_BLEND_B + (float)(h16)((float)rad / c39)) * FOCUS_BLEND_A) ==
					      (h16)1.0f) {
					o4[0] = (float)hc[0];
					o4[1] = (float)hc[1];
					o4[2] = (float)hc[2];
					o4[3] = 1;
					done = 1;
					break;
				}
				h16 rb = (h16)(rad * hboost);

				for (int c = 0; c < 3; c++) {
					h16 w = (h16)(rb * hc[c] + (h16)1.0f);

					hs[c] = (h16)(w * hc[c] + hs[c]);
					hw[c] = (h16)(w + hw[c]);
				}
				hr = (h16)(rad + hr);
				n++;
			}
			if (done)
				continue;
			for (int c = 0; c < 3; c++)
				o4[c] = (float)hw[c] > 0 ? (float)(h16)(hs[c] / hw[c]) : 0;
			o4[3] = n ? (float)(h16)saturate((FOCUS_BLEND_B + (float)(h16)((float)hr / (n * c39))) * FOCUS_BLEND_A) : 0;
		}
}

/*
 * Guided filter (coefficients, their average, applied): radius 1 on the 3x3 box, no square-root guide, one
 * output through the post-modifier. Taps outside the image are left out of the sums and the count.
 */
static const signed char box3[9][2] = {
	{ -1, -1 }, { -1, 0 }, { -1, 1 }, { 0, -1 }, { 0, 0 }, { 0, 1 }, { 1, -1 }, { 1, 0 }, { 1, 1 },
};

void sdof_guided_coefs(const float *guide, const float *p, int w, int h, float eps, float *coef)
{
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			float mi[3] = { 0 }, mp = 0, mip[3] = { 0 }, e00 = 0, e01 = 0, e02 = 0, e11 = 0, e12 = 0, e22 = 0;
			int n = 0;

			for (int k = 0; k < 9; k++) {
				int sx = x + box3[k][0], sy = y + box3[k][1];

				if (sx < 0 || sx >= w || sy < 0 || sy >= h)
					continue;
				const float *g = guide + ((size_t)sy * w + sx) * 4;
				float v = p[(size_t)sy * w + sx];

				for (int c = 0; c < 3; c++) {
					mi[c] += g[c];
					mip[c] += g[c] * v;
				}
				mp += v;
				e00 += g[0] * g[0];
				e01 += g[0] * g[1];
				e02 += g[0] * g[2];
				e11 += g[1] * g[1];
				e12 += g[1] * g[2];
				e22 += g[2] * g[2];
				n++;
			}
			float fn = (float)n, m0 = mi[0] / fn, m1 = mi[1] / fn, m2 = mi[2] / fn, pm = mp / fn;
			float c0 = mip[0] / fn - m0 * pm, c1 = mip[1] / fn - m1 * pm, c2 = mip[2] / fn - m2 * pm;

			e00 /= fn; e01 /= fn; e02 /= fn; e11 /= fn; e12 /= fn; e22 /= fn;
			/* (cov(I) + eps) a = cov(I, p), by the LDL^T solve the shader does */
			float s01 = e01 - m0 * m1, s02 = e02 - m0 * m2;
			float d0 = (e00 - m0 * m0) + eps;
			float l10 = s01 / d0;
			float d1 = (e11 + eps) - (l10 * s01 + m1 * m1);
			float t12 = e12 - (m1 * m2 + l10 * s02);
			float l20 = s02 / d0, l21 = t12 / d1;
			float d2 = (e22 + eps) - ((l20 * s02 + m2 * m2) + l21 * t12);
			float y1 = c1 - l10 * c0, y2 = c2 - (l20 * c0 + l21 * y1);
			float a2 = y2 / d2, a1 = (y1 - a2 * t12) / d1, a0 = (c0 - (a1 * s01 + a2 * s02)) / d0;
			float *o = coef + ((size_t)y * w + x) * 4;

			o[0] = (float)(h16)a0;
			o[1] = (float)(h16)a1;
			o[2] = (float)(h16)a2;
			o[3] = (float)(h16)(pm - (a0 * m0 + a1 * m1 + a2 * m2));
		}
}

void sdof_guided_average(const float *coef, int w, int h, float *out)
{
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			h16 s[4] = { 0 };
			int n = 0;

			for (int k = 0; k < 9; k++) {
				int sx = x + box3[k][0], sy = y + box3[k][1];

				if (sx < 0 || sx >= w || sy < 0 || sy >= h)
					continue;
				for (int c = 0; c < 4; c++)
					s[c] = (h16)(s[c] + (h16)coef[((size_t)sy * w + sx) * 4 + c]);
				n++;
			}
			for (int c = 0; c < 4; c++)
				out[((size_t)y * w + x) * 4 + c] = (float)(h16)(s[c] / (h16)n);
		}
}

void sdof_guided_apply(const float *coef, int cw, int ch, const float *guide, int w, int h,
		       float offset, float lo, float hi, float *out)
{
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			float ab[4];
			const float *g = guide + ((size_t)y * w + x) * 4;

			bilinear(coef, cw, ch, 4, (x + 0.5f) / w, (y + 0.5f) / h, ab);
			h16 q = (h16)((h16)((h16)((h16)g[0] * (h16)ab[0]) + (h16)((h16)g[1] * (h16)ab[1])) +
				      (h16)((h16)g[2] * (h16)ab[2]));
			float v = offset + (float)(h16)(q + (h16)ab[3]);

			out[(size_t)y * w + x] = (float)(h16)(v < lo ? lo : v > hi ? hi : v);
		}
}

/* A linear value as an RGBA8 sRGB texel stores it, read back. */
static float srgb8(float v)
{
	return sdof_srgb_to_linear(lrintf(sdof_linear_to_srgb(v) * 255.0f) / 255.0f);
}

/* BT.709's inverse OETF (kColorTransferFunctionToLinear 1). */
static float bt709_to_linear(float v)
{
	return v >= 0.081f ? powf((v + 0.099f) / 1.099f, 1.0f / 0.45f) : v / 4.5f;
}

void sdof_level0_nv12(const uint8_t *y, const uint8_t *uv, int w, int h, int stride,
		      const struct sdof_yuv *m, float *out)
{
	float kg = 1 - m->kr - m->kb, ys = m->full_range ? 1 : 255.0f / 219, cs = m->full_range ? 1 : 255.0f / 224;
	float yo = m->full_range ? 0 : 16;
	float rv = 2 * (1 - m->kr), gu = -2 * (1 - m->kb) * m->kb / kg, gv = -2 * (1 - m->kr) * m->kr / kg;
	float bu = 2 * (1 - m->kb);
	int ow = w / 2, oh = h / 2;

	for (int j = 0; j < oh; j++)
		for (int i = 0; i < ow; i++) {
			const uint8_t *l = y + (size_t)(2 * j) * stride + 2 * i, *c = uv + (size_t)j * stride + 2 * i;
			float yy = ((l[0] + l[1] + l[stride] + l[stride + 1]) / 4.0f - yo) * ys / 255;
			float cb = (c[0] - 128) * cs / 255, cr = (c[1] - 128) * cs / 255;
			float rgb[3] = { yy + rv * cr, yy + gu * cb + gv * cr, yy + bu * cb };
			float *o = out + ((size_t)j * ow + i) * 4;

			for (int k = 0; k < 3; k++)
				o[k] = srgb8(bt709_to_linear(rgb[k] < 0 ? 0 : rgb[k] > 1 ? 1 : rgb[k]));
			o[3] = 1;
		}
}

void sdof_downscale_gauss3(const float *src, int sw, int sh, float *dst, int dw, int dh)
{
	static const float wt[3] = { (float)(h16)0.0751343f, (float)(h16)0.1238403f, (float)(h16)0.2042236f };

	for (int j = 0; j < dh; j++)
		for (int i = 0; i < dw; i++) {
			float u = (i + 0.5f) / dw, v = (j + 0.5f) / dh;
			h16 acc[3] = { 0 };

			for (int a = 0; a < 3; a++)
				for (int b = 0; b < 3; b++) {
					float t[4];
					h16 k = (h16)wt[(a == 1) + (b == 1)];

					bilinear(src, sw, sh, 4, u + (a - 1) / (float)sw, v + (b - 1) / (float)sh, t);
					for (int c = 0; c < 3; c++)
						acc[c] = (h16)(acc[c] + (h16)((h16)t[c] * k));
				}
			float *o = dst + ((size_t)j * dw + i) * 4;

			for (int c = 0; c < 3; c++)
				o[c] = srgb8((float)acc[c]);
			o[3] = 1;
		}
}

/* BT.709's OETF (kColorTransferFunction 2), in half like the shader. */
static float linear_to_bt709(float v)
{
	h16 x = (h16)v;

	return x >= (h16)0.0180054f ? (float)(h16)((h16)((h16)powf((float)x, (float)(h16)0.4499512f) * (h16)1.0986328f) - (h16)0.098999f)
				  : (float)(h16)(x * (h16)4.5f);
}

static float tap_repeat(const float *lut, int n, float u, float v)
{
	float x = u * n - 0.5f, y = v * n - 0.5f;
	int x0 = (int)floorf(x), y0 = (int)floorf(y);
	float fx = x - x0, fy = y - y0;
	int xa = ((x0 % n) + n) % n, xb = (xa + 1) % n, ya = ((y0 % n) + n) % n, yb = (ya + 1) % n;
	float a = lut[ya * n + xa], b = lut[ya * n + xb], c = lut[yb * n + xa], d = lut[yb * n + xb];

	return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
}

void sdof_composite_nv12(const float *ray, int rw, int rh, const uint8_t *y, const uint8_t *uv, int w, int h,
			 int stride, const struct sdof_yuv *m, const struct sdof_grain *g, uint8_t *oy, uint8_t *ouv)
{
	float kg = 1 - m->kr - m->kb, ys = m->full_range ? 1 : 255.0f / 219, cs = m->full_range ? 1 : 255.0f / 224;
	float yo = m->full_range ? 0 : 16;
	float rv = 2 * (1 - m->kr), gu = -2 * (1 - m->kb) * m->kb / kg, gv = -2 * (1 - m->kr) * m->kr / kg;
	float bu = 2 * (1 - m->kb);
	float *cb = calloc((size_t)(w / 2) * (h / 2) * 2, sizeof(float));

	for (int j = 0; j < h; j++)
		for (int i = 0; i < w; i++) {
			float r[4], s[3], o[3];
			const uint8_t *c = uv + (size_t)(j / 2) * stride + (i / 2) * 2;
			float yy = (y[(size_t)j * stride + i] - yo) * ys / 255;
			float u = (c[0] - 128) * cs / 255, v = (c[1] - 128) * cs / 255;

			bilinear(ray, rw, rh, 4, (i + 0.5f) / w, (j + 0.5f) / h, r);
			h16 al = (h16)r[3];

			s[0] = yy + rv * v;
			s[1] = yy + gu * u + gv * v;
			s[2] = yy + bu * u;
			for (int k = 0; k < 3; k++)
				s[k] = s[k] < 0 ? 0 : s[k] > 1 ? 1 : s[k];
			if (al == (h16)1) {
				memcpy(o, s, sizeof(o));
			} else {
				float n = 0;

				if (g && g->lut) {
					float sz = g->lut_size * g->grain;
					float n1 = (float)(h16)tap_repeat(g->lut, g->lut_size, (i + g->ox + 0.5f) / sz,
									(j + g->oy + 0.5f) / sz);
					float n2 = (float)(h16)tap_repeat(g->lut, g->lut_size, (-0.5f - i) / sz,
									(-0.5f - j) / sz);

					n = (float)(h16)((float)(h16)((n1 + n2) * 0.5f) * (1 - (float)al));
				}
				for (int k = 0; k < 3; k++) {
					h16 b = (h16)r[k];
					h16 lin = al > (h16)0 ? (h16)bt709_to_linear(s[k]) : (h16)0;
					h16 sd = (h16)sqrtf((float)(h16)((h16)(g ? g->a : 0) +
						 (h16)fmaxf(0, (float)(h16)((h16)(g ? g->b : 0) * b))));
					h16 half_b = (h16)(b * (h16)0.5f);
					h16 amp = half_b < (h16)0 ? (h16)0 : half_b > sd ? sd : half_b;
					h16 bl = (h16)((h16)n * amp + b);
					h16 mx = (h16)(bl + (lin - bl) * al);

					o[k] = linear_to_bt709((float)mx);
				}
			}
			float Y = m->kr * o[0] + kg * o[1] + m->kb * o[2];
			float *cc = cb + ((size_t)(j / 2) * (w / 2) + i / 2) * 2;

			oy[(size_t)j * stride + i] = (uint8_t)fminf(255, fmaxf(0, lrintf(Y * 255 / ys + yo)));
			cc[0] += (o[2] - Y) / bu;
			cc[1] += (o[0] - Y) / rv;
		}
	for (int j = 0; j < h / 2; j++)
		for (int i = 0; i < w / 2; i++)
			for (int k = 0; k < 2; k++) {
				float v = cb[((size_t)j * (w / 2) + i) * 2 + k] / 4;

				ouv[(size_t)j * stride + i * 2 + k] = (uint8_t)fminf(255, fmaxf(0, lrintf(v * 255 / cs + 128)));
			}
	free(cb);
}

/* Area weights of n0 source samples for each of n1 outputs: idx[i] = first, cnt[i], wts[i * span ...]. */
static int area_weights(int n0, int n1, int **first, int **cnt, float **wts, int *span)
{
	double sc = (double)n0 / n1;

	*span = (int)ceil(sc) + 1;
	*first = malloc(sizeof(int) * n1);
	*cnt = malloc(sizeof(int) * n1);
	*wts = calloc((size_t)n1 * *span, sizeof(float));
	if (!*first || !*cnt || !*wts)
		return -1;
	for (int i = 0; i < n1; i++) {
		double a = i * sc, b = (i + 1) * sc;
		int k0 = (int)floor(a), k1 = (int)ceil(b);

		if (k1 > n0)
			k1 = n0;
		(*first)[i] = k0;
		(*cnt)[i] = k1 - k0;
		for (int k = k0; k < k1; k++)
			(*wts)[(size_t)i * *span + (k - k0)] = (float)(((b < k + 1 ? b : k + 1) - (a > k ? a : k)) / sc);
	}
	return 0;
}

/* The frame's R'G'B' area-resampled to ow x oh, as a BGRA8 guide read as sRGB: linear RGBA out. */
int sdof_guide_nv12(const uint8_t *y, const uint8_t *uv, int w, int h, int stride, const struct sdof_yuv *m,
		    int ow, int oh, float *out)
{
	float kg = 1 - m->kr - m->kb, ys = m->full_range ? 1 : 255.0f / 219, cs = m->full_range ? 1 : 255.0f / 224;
	float yo = m->full_range ? 0 : 16;
	float rv = 2 * (1 - m->kr), gu = -2 * (1 - m->kb) * m->kb / kg, gv = -2 * (1 - m->kr) * m->kr / kg;
	float bu = 2 * (1 - m->kb);
	int *fx, *nx, *fy, *ny, sx, sy, r = -1;
	float *wx, *wy, *rows = malloc((size_t)ow * h * 3 * sizeof(float));

	if (!rows || area_weights(w, ow, &fx, &nx, &wx, &sx) || area_weights(h, oh, &fy, &ny, &wy, &sy))
		goto out;
	for (int j = 0; j < h; j++)
		for (int i = 0; i < ow; i++) {
			float acc[3] = { 0 };

			for (int k = 0; k < nx[i]; k++) {
				int x = fx[i] + k;
				const uint8_t *c = uv + (size_t)(j / 2) * stride + (x / 2) * 2;
				float yy = (y[(size_t)j * stride + x] - yo) * ys / 255;
				float u = (c[0] - 128) * cs / 255, v = (c[1] - 128) * cs / 255;
				float p[3] = { yy + rv * v, yy + gu * u + gv * v, yy + bu * u }, wt = wx[(size_t)i * sx + k];

				for (int q = 0; q < 3; q++) {
					float e = p[q] < 0 ? 0 : p[q] > 1 ? 1 : p[q];

					acc[q] += wt * (m->tagged ? powf(e, 1.961f) : e);
				}
			}
			memcpy(rows + ((size_t)j * ow + i) * 3, acc, sizeof(acc));
		}
	for (int j = 0; j < oh; j++)
		for (int i = 0; i < ow; i++) {
			float acc[3] = { 0 };

			for (int k = 0; k < ny[j]; k++)
				for (int q = 0; q < 3; q++)
					acc[q] += wy[(size_t)j * sy + k] * rows[((size_t)(fy[j] + k) * ow + i) * 3 + q];
			for (int q = 0; q < 3; q++)
				out[((size_t)j * ow + i) * 4 + q] = sdof_srgb_to_linear(
					lrintf((m->tagged ? sdof_linear_to_srgb(acc[q]) : acc[q]) * 255) / 255.0f);
			out[((size_t)j * ow + i) * 4 + 3] = 1;
		}
	free(fx); free(nx); free(wx); free(fy); free(ny); free(wy);
	r = 0;
out:
	free(rows);
	return r;
}

int sdof_render_nv12(const uint8_t *y, const uint8_t *uv, int w, int h, int stride, const float *disp, int dw,
		     int dh, float aperture, float focus_offset, const struct sdof_yuv *m,
		     const struct sdof_grain *g, uint8_t *oy, uint8_t *ouv)
{
	int pw = w / 2, ph = h / 2, uw = dw * 2, uh = dh * 2, r = -1;
	size_t lsz = 0;
	struct sdof_level lv[4];
	struct sdof_params p;
	float *pyr, *gl = malloc((size_t)dw * dh * 4 * sizeof(float)), *gh = malloc((size_t)uw * uh * 4 * sizeof(float));
	float *d = malloc((size_t)dw * dh * sizeof(float)), *cf = malloc((size_t)dw * dh * 4 * sizeof(float));
	float *av = malloc((size_t)dw * dh * 4 * sizeof(float)), *fd = malloc((size_t)uw * uh * sizeof(float));
	float *ray = malloc((size_t)pw * ph * 4 * sizeof(float));

	for (int l = 0; l < 4; l++)
		lsz += (size_t)(pw >> l) * (ph >> l) * 4;
	pyr = malloc(lsz * sizeof(float));
	if (!pyr || !gl || !gh || !d || !cf || !av || !fd || !ray)
		goto out;
	if (sdof_guide_nv12(y, uv, w, h, stride, m, dw, dh, gl) || sdof_guide_nv12(y, uv, w, h, stride, m, uw, uh, gh))
		goto out;
	for (int i = 0; i < dw * dh; i++)
		d[i] = (float)(h16)(disp[i] * DISPARITY_SCALE);
	sdof_guided_coefs(gl, d, dw, dh, 0.05f, cf);
	sdof_guided_average(cf, dw, dh, av);
	sdof_guided_apply(av, dw, dh, gh, uw, uh, focus_offset, -3.4028235e38f, 0, fd);
	float *lp = pyr;

	for (int l = 0; l < 4; l++) {
		lv[l] = (struct sdof_level) { pw >> l, ph >> l, lp };
		if (l == 0)
			sdof_level0_nv12(y, uv, w, h, stride, m, lp);
		else
			sdof_downscale_gauss3(lv[l - 1].rgba, lv[l - 1].w, lv[l - 1].h, lp, lv[l].w, lv[l].h);
		lp += (size_t)lv[l].w * lv[l].h * 4;
	}
	sdof_params_for(&p, aperture, w);
	sdof_raytrace(lv, 4, fd, uw, uh, &p, ray, pw, ph);
	/* the pass stores RGBA8 sRGB */
	for (int i = 0; i < pw * ph * 4; i++)
		ray[i] = i % 4 == 3 ? lrintf(ray[i] * 255) / 255.0f : srgb8(ray[i]);
	sdof_composite_nv12(ray, pw, ph, y, uv, w, h, stride, m, g, oy, ouv);
	r = 0;
out:
	free(pyr); free(gl); free(gh); free(d); free(cf); free(av); free(fd); free(ray);
	return r;
}

void sdof_grain_table(float *lut, int n, uint32_t seed)
{
	uint64_t x = 0x9E3779B97F4A7C15ull ^ seed;
	double sum = 0, sq = 0;
	int cnt = n * n;

	for (int i = 0; i < cnt; i += 2) {
		double u1, u2;

		do {
			x ^= x << 13; x ^= x >> 7; x ^= x << 17;
			u1 = ((x >> 11) + 0.5) / 9007199254740992.0;
			x ^= x << 13; x ^= x >> 7; x ^= x << 17;
			u2 = (x >> 11) / 9007199254740992.0;
		} while (u1 <= 0);
		double r = sqrt(-2 * log(u1));

		lut[i] = (float)(r * cos(6.283185307179586 * u2));
		if (i + 1 < cnt)
			lut[i + 1] = (float)(r * sin(6.283185307179586 * u2));
	}
	for (int i = 0; i < cnt; i++) {
		sum += lut[i];
		sq += (double)lut[i] * lut[i];
	}
	double mean = sum / cnt, sd = sqrt(sq / cnt - mean * mean);

	for (int i = 0; i < cnt; i++)
		lut[i] = (float)(h16)((lut[i] - mean) / sd);
}

/* Metal's bicubic filter (sampler filter::bicubic): Catmull-Rom, clamp_to_edge, on one channel. */
static float catmull_rom(float t)
{
	t = fabsf(t);
	return t < 1 ? (1.5f * t - 2.5f) * t * t + 1 : t < 2 ? ((-0.5f * t + 2.5f) * t - 4) * t + 2 : 0;
}

static float bicubic(const float *img, int w, int h, float u, float v)
{
	float x = u * w - 0.5f, y = v * h - 0.5f, r = 0;
	int x0 = (int)floorf(x), y0 = (int)floorf(y);
	float fx = x - x0, fy = y - y0;

	for (int b = -1; b < 3; b++) {
		int yy = y0 + b < 0 ? 0 : y0 + b >= h ? h - 1 : y0 + b;
		float wy = catmull_rom(fy - b), row = 0;

		for (int a = -1; a < 3; a++) {
			int xx = x0 + a < 0 ? 0 : x0 + a >= w ? w - 1 : x0 + a;

			row += img[(size_t)yy * w + xx] * catmull_rom(fx - a);
		}
		r += row * wy;
	}
	return r;
}

void sdof_temporal_ema(const float *flow, int fw, int fh, const float *prev, const float *disp, int w, int h,
		       float *out)
{
	for (int j = 0; j < h; j++)
		for (int i = 0; i < w; i++) {
			float u = (i + 0.5f) / w, v = (j + 0.5f) / h, f[2], nw = disp[(size_t)j * w + i] * DISPARITY_SCALE;
			float r = nw;

			bilinear(flow, fw, fh, 2, u, v, f);
			float pu = u + f[0] / fw, pv = v + f[1] / fh;

			if (pu >= 0 && pv >= 0 && pu <= 1 && pv <= 1) {
				float motion = sqrtf(f[0] * f[0] + f[1] * f[1]) * 720 / fh;
				float t = saturate((motion - 1) / (4 - 1)), k = 0.23f + (1 - 0.23f) * t;
				float p = bicubic(prev, w, h, pu, pv);

				r = p + (nw - p) * k;
			}
			out[(size_t)j * w + i] = (float)(h16)r;
		}
}
