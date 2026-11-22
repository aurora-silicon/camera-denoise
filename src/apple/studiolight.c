// SPDX-License-Identifier: MIT
/* Studio Light's relighting passes, plain C; see studiolight.h. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "studiolight.h"

typedef _Float16 h16;

/* The 3x3 Gaussian (half weights 0.0751, 0.1238, 0.2042) */
static const float gauss3[3] = { 0.07513428f, 0.12384033f, 0.20422363f };

void sl_params_default(struct sl_params *p)
{
	*p = (struct sl_params) {
		.bg_centre = { 0.9135f, 0.9135f, 0.966f }, .bg_edge = { 0.315f, 0.315f, 0.3465f },
		.vignette_pow = 0.5f, .near = 1.05f, .far = 0.9f, .lum_mix = 0.5f,
		.subject_mix = { 1, 0.9f, 0.75f }, .bg_gamma = { 1.03093f, 1.03093f, 1 },
		.hl_lo = { 0.5f, 2.0f }, .hl_hi = { 0.5f, 1.5f },
		.fov = { 0.577293f, 0.324727f }, .max_disparity = 10, .distance_modifier = 1,
	};
	sl_params_matrix(p, &(struct sl_yuv) { 0.2126f, 0.0722f, 0 });
}

/* Portrait's half3x4 matrices for a YCbCr matrix (rounded to half as it passes them). */
void sl_params_matrix(struct sl_params *p, const struct sl_yuv *m)
{
	float kg = 1 - m->kr - m->kb, ys = m->full_range ? 1 : 219.0f / 255, cs = m->full_range ? 1 : 224.0f / 255;
	float yo = m->full_range ? 0 : 16.0f / 255, co = 128.0f / 255;
	float f[3][4] = {
		{ m->kr * ys, kg * ys, m->kb * ys, yo },
		{ -m->kr / (2 * (1 - m->kb)) * cs, -kg / (2 * (1 - m->kb)) * cs, 0.5f * cs, co },
		{ 0.5f * cs, -kg / (2 * (1 - m->kr)) * cs, -m->kb / (2 * (1 - m->kr)) * cs, co },
	};
	float rv = 2 * (1 - m->kr) / cs, gu = -2 * (1 - m->kb) * m->kb / kg / cs, gv = -2 * (1 - m->kr) * m->kr / kg / cs;
	float bu = 2 * (1 - m->kb) / cs, yy = 1 / ys;
	float g[3][4] = {
		{ yy, 0, rv, -yy * yo - rv * co },
		{ yy, gu, gv, -yy * yo - (gu + gv) * co },
		{ yy, bu, 0, -yy * yo - bu * co },
	};

	for (int r = 0; r < 3; r++)
		for (int k = 0; k < 4; k++) {
			p->rgb2yuv[r][k] = (float)(h16)f[r][k];
			p->yuv2rgb[r][k] = (float)(h16)g[r][k];
		}
}

/* The light estimate: L = 0.5 + 0.8 (clamp(c13, 0.6, 0.8) - 0.5). */
void sl_light_from_fexp(struct sl_light *o, float c6, float c7, float c8, float c13)
{
	float c = c13 < 0.6f ? 0.6f : c13 > 0.8f ? 0.8f : c13;

	*o = (struct sl_light) { (c - 0.5f) * 0.8f + 0.5f, c6, c7, c8 };
}

void sl_light_default(struct sl_light *o)
{
	/* no face: c13 = 0.5 and the clamp's floor, the rest 0 */
	sl_light_from_fexp(o, 0, 0, 0, 0.5f);
}

static float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static float sat(float v)
{
	return clampf(v, 0, 1);
}

static float q8(float v)
{
	return lrintf(sat(v) * 255) / 255.0f;
}

static float hf(float v)
{
	return (float)(h16)v;
}

/*
 * A filtered fp16 texel as the GPU's texture unit returns it: rounded to fp16 half away from zero (not to
 * nearest-even; measured on the M5: the diffuse pass is exact only this way).
 */
static float h16_tex(float v)
{
	int e;
	float m = frexpf(fabsf(v), &e), ulp;

	if (v == 0 || !isfinite(v))
		return v;
	(void)m;
	ulp = e - 1 < -14 ? ldexpf(1, -24) : ldexpf(1, e - 1 - 10);
	float r = floorf(fabsf(v) / ulp + 0.5f) * ulp;

	/* and fp16 denormals flushed to 0 (a dark guide pixel's gain is 1 in the reference: its colour read as 0) */
	return r < ldexpf(1, -14) ? copysignf(0, v) : copysignf(r, v);
}

/* Bilinear, normalised coordinates, clamp_to_edge, ch channels; weights in 8-bit fractions like the texture unit. */
static void bilinear(const float *img, int w, int h, int ch, float u, float v, float *out)
{
	float x = u * w - 0.5f, y = v * h - 0.5f;
	int x0 = (int)floorf(x), y0 = (int)floorf(y);
	float fx = rintf((x - x0) * 256.0f) / 256.0f, fy = rintf((y - y0) * 256.0f) / 256.0f;
	int xa = x0 < 0 ? 0 : x0 >= w ? w - 1 : x0, xb = x0 + 1 < 0 ? 0 : x0 + 1 >= w ? w - 1 : x0 + 1;
	int ya = y0 < 0 ? 0 : y0 >= h ? h - 1 : y0, yb = y0 + 1 < 0 ? 0 : y0 + 1 >= h ? h - 1 : y0 + 1;

	for (int c = 0; c < ch; c++) {
		float a = img[((size_t)ya * w + xa) * ch + c], b = img[((size_t)ya * w + xb) * ch + c];
		float d = img[((size_t)yb * w + xa) * ch + c], e = img[((size_t)yb * w + xb) * ch + c];

		out[c] = (a + (b - a) * fx) + ((d + (e - d) * fx) - (a + (b - a) * fx)) * fy;
	}
}

static float bt709_to_linear(float v)
{
	return v >= 0.081f ? powf((v + 0.099f) / 1.099f, 1.0f / 0.45f) : v / 4.5f;
}

static float linear_to_bt709(float v)
{
	return v >= 0.0180054f ? 1.0986328f * powf(v, 0.4499512f) - 0.098999f : v * 4.5f;
}

/* ---- 1. diffuse ---- */

void sl_diffuse(const float *disp, int dw, int dh, const struct sl_params *p, float *diffuse)
{
	static const float off[4][2] = { { -0.5f, 0.5f }, { 0.5f, -0.5f }, { -0.5f, -0.5f }, { 0.5f, 0.5f } };

	for (int j = 0; j < dh; j++)
		for (int i = 0; i < dw; i++) {
			float pt[4][3];

			for (int k = 0; k < 4; k++) {
				float u = (i + 0.5f) / dw + off[k][0] / dw, v = (j + 0.5f) / dh + off[k][1] / dh, d;

				bilinear(disp, dw, dh, 1, u, v, &d);
				d = clampf(h16_tex(d), 0.01f, p->max_disparity);
				pt[k][0] = (u * 2 - 1) * p->fov[0] / d;
				pt[k][1] = (v * 2 - 1) * p->fov[1] / d;
				pt[k][2] = 1 / d;
			}
			float e1[3], e2[3];

			for (int c = 0; c < 3; c++) {
				e1[c] = pt[1][c] - pt[0][c];
				e2[c] = pt[3][c] - pt[2][c];
			}
			float n[3] = { e1[1] * e2[2] - e2[1] * e1[2], e2[0] * e1[2] - e2[2] * e1[0],
				       e2[1] * e1[0] - e2[0] * e1[1] };
			float nz = n[2] / sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
			/* stored R16Unorm, then copied to R8Unorm */
			float v16 = lrintf(sat(nz) * 65535) / 65535.0f;

			diffuse[(size_t)j * dw + i] = q8(v16);
		}
}

/* ---- 2. light mask ---- */

void sl_light_mask(const struct sl_face *faces, int nfaces, float frame_aspect, int mw, int mh, float *mask)
{
	/* the face record's constants */
	const float body_pow = 1.5f, slope = 3 * 0.5f, width = 0.9f, lift = 0.25f;

	if (nfaces > 4)
		nfaces = 4;
	for (int j = 0; j < mh; j++)
		for (int i = 0; i < mw; i++) {
			float u = hf((i + 0.5f) / mw), v = hf((j + 0.5f) / mh), face = 0, body = 0;

			for (int f = 0; f < nfaces; f++) {
				float fw = faces[f].w * frame_aspect, fh = faces[f].h;
				float r = 0.375f * sqrtf(fw * fw + fh * fh) / sqrtf(2), len = r * 2.5f, conf = 1;
				float cx = faces[f].x + faces[f].w / 2, cy = faces[f].y + faces[f].h / 2;
				float dx = (u - cx) * frame_aspect, dy = v - cy;
				float qy = dy + r * lift, k = 1 + sat(slope * qy), wd = width / k;
				float px = fabsf(dx), dotb = px * wd + qy * 2, bb = wd * wd + 4, t = sat(dotb / bb);
				float ex = px - t * wd, ey = qy - t * 2, side = 2 * px - wd * qy;
				float sd = (side > 0 ? 1 : side < 0 ? -1 : 0) * sqrtf(ex * ex + ey * ey) - r;
				float b = sat(1 - sat(sd / (k * len))) * conf;

				/* the face-light term needs the landmark radii; Portrait leaves them 0 (NaN -> 0) */
				body = b > body ? b : body;
			}
			mask[((size_t)j * mw + i) * 2 + 0] = q8(face);
			mask[((size_t)j * mw + i) * 2 + 1] = q8(powf(body, body_pow));
		}
}

/* ---- 4. gain ---- */

/* kDisparityFilterType 3, step 2 output texels: Gaussian of the 3x3 taps no nearer than the centre. */
static float disparity_filtered(const float *disp, int dw, int dh, float u, float v, float su, float sv)
{
	float c, acc = 0, wsum = 0;

	bilinear(disp, dw, dh, 1, u, v, &c);
	for (int a = 0; a < 3; a++)
		for (int b = 0; b < 3; b++) {
			float t, w = gauss3[(a == 1) + (b == 1)];

			bilinear(disp, dw, dh, 1, u + (a - 1) * su, v + (b - 1) * sv, &t);
			t = hf(t);
			if (t <= hf(c)) {
				acc += w * t;
				wsum += w;
			}
		}
	return hf(acc / wsum);
}

/* Exposure curve: L x / (L x + (1 - L)(1 - x)), as 1 - 1 / (1 + L x / ((1 - L)(1 - x))). */
static float expo(float x, float l)
{
	float r = 1 - 1 / (1 + l * x / ((1 - l) * (1 - x)));

	return r > 0 ? r : 0;
}

void sl_gain(const float *guide, int gdw, int gdh, const float *diffuse, const float *disp, int dw, int dh,
	     const float *mask, const struct sl_params *p, const struct sl_light *l, float focus_offset,
	     float *gain, int gw, int gh)
{
	float fo = -focus_offset, nearv = hf(p->near * fo), farv = hf(p->far * fo);

	for (int j = 0; j < gh; j++)
		for (int i = 0; i < gw; i++) {
			float u = (i + 0.5f) / gw, v = (j + 0.5f) / gh, c[4], d, m[2];

			bilinear(guide, gdw, gdh, 4, u, v, c);
			bilinear(mask, gw, gh, 2, u, v, m);
			bilinear(diffuse, dw, dh, 1, u, v, &d);
			for (int k = 0; k < 3; k++)
				c[k] = h16_tex(c[k]);
			float dd = disparity_filtered(disp, dw, dh, u, v, 2.0f / gw, 2.0f / gh);
			float s = sat(fminf(m[1], (dd - farv) / (nearv - farv)));
			float bg = (1 - s) * p->distance_modifier;
			float du = hf(u) - 0.5f, dv = hf(v) - 0.5f;
			float vig = 1 - sqrtf(du * du + dv * dv) * 2 / sqrtf(2), vp = powf(vig, p->vignette_pow);
			float sub = (1 - bg) * d;
			float y = p->rgb2yuv[0][0] * c[0] + p->rgb2yuv[0][1] * c[1] + p->rgb2yuv[0][2] * c[2];
			float L = hf(l->l), Ld = hf(1 - L);
			float gl = fmaxf(expo(y, L) / y, 0), gd = fmaxf(expo(y, Ld) / y, 0);
			float t = sat(y / hf(l->z)), w = t * t * (3 - 2 * t) * hf(l->y);
			float a = sat((m[0] + 1) * sub), *o = gain + ((size_t)j * gw + i) * 4;

			for (int k = 0; k < 3; k++) {
				float tint = p->bg_edge[k] + (p->bg_centre[k] - p->bg_edge[k]) * vp;
				float b0 = c[k] + (powf(c[k], p->bg_gamma[k]) - c[k]) * bg;
				float B = b0 + (b0 * tint - b0) * bg;
				float up = gl * c[k] + (expo(c[k], L) - gl * c[k]) * p->lum_mix;
				float dn = gd * c[k] + (expo(c[k], Ld) - gd * c[k]) * p->lum_mix;
				float lit = up + (fminf(dn, c[k]) - up) * w;
				float S = c[k] + (lit - c[k]) * p->subject_mix[k];
				float R = B + (S - B) * a;

				o[k] = hf(c[k] > 0 ? R / c[k] : 1);
			}
			o[3] = 1;
		}
}

/* ---- 5. blur and upsample ---- */

void sl_gain_blur(const float *in, int w, int h, float *out)
{
	sl_gain_upsample(in, w, h, out, w, h);
}

void sl_gain_upsample(const float *in, int w, int h, float *out, int uw, int uh)
{
	for (int j = 0; j < uh; j++)
		for (int i = 0; i < uw; i++) {
			float u = (i + 0.5f) / uw, v = (j + 0.5f) / uh;
			h16 acc[3] = { 0 };

			for (int a = 0; a < 3; a++)
				for (int b = 0; b < 3; b++) {
					float t[4];
					h16 k = (h16)gauss3[(a == 1) + (b == 1)];

					bilinear(in, w, h, 4, u + (a - 1) / (float)w, v + (b - 1) / (float)h, t);
					for (int c = 0; c < 3; c++)
						acc[c] = (h16)(acc[c] + (h16)((h16)t[c] * k));
				}
			for (int c = 0; c < 3; c++)
				out[((size_t)j * uw + i) * 4 + c] = (float)acc[c];
			out[((size_t)j * uw + i) * 4 + 3] = 1;
		}
}

/* ---- the colour cube ---- */

/* Trilinear in texel coordinates (the cubes are sampled in pixel coordinates at n-1 * v + 0.5). */
static void cube_tap(const float *cube, int n, int ch, const float pos[3], float *out)
{
	int i0[3], i1[3];
	float f[3];

	for (int k = 0; k < 3; k++) {
		float x = clampf(pos[k], 0, (float)(n - 1));

		i0[k] = (int)floorf(x);
		i1[k] = i0[k] + 1 < n ? i0[k] + 1 : n - 1;
		f[k] = x - i0[k];
	}
	for (int c = 0; c < ch; c++) {
		float r = 0;

		for (int z = 0; z < 2; z++)
			for (int y = 0; y < 2; y++)
				for (int x = 0; x < 2; x++) {
					float w = (x ? f[0] : 1 - f[0]) * (y ? f[1] : 1 - f[1]) * (z ? f[2] : 1 - f[2]);
					size_t idx = (((size_t)(z ? i1[2] : i0[2]) * n + (y ? i1[1] : i0[1])) * n +
						      (x ? i1[0] : i0[0]));

					r += w * cube[idx * ch + c];
				}
		out[c] = r;
	}
}

int sl_cube_build(struct sl_cube *c, int n, const uint8_t *correction, const struct sl_params *p)
{
	const float (*rgb2yuv)[4] = p->rgb2yuv, (*yuv2rgb)[4] = p->yuv2rgb;

	float *corr = NULL;
	size_t nn = (size_t)n * n * n;

	c->n = n;
	c->yuv = malloc(nn * 3 * sizeof(float));
	if (!c->yuv)
		return -1;
	if (correction) {
		corr = malloc(nn * 3 * sizeof(float));
		if (!corr) {
			free(c->yuv);
			return -1;
		}
		for (size_t i = 0; i < nn; i++)
			for (int k = 0; k < 3; k++)
				corr[i * 3 + k] = correction[i * 4 + k] / 255.0f;
	}
	/* the linear range: black and white YCbCr codes (0 / 1, chroma 0.5) through the matrix and the inverse OETF */
	for (int k = 0; k < 3; k++) {
		c->min[k] = hf(bt709_to_linear(hf(yuv2rgb[k][1] * 0.5f + yuv2rgb[k][2] * 0.5f + yuv2rgb[k][3])));
		c->max[k] = hf(bt709_to_linear(hf(yuv2rgb[k][0] + yuv2rgb[k][1] * 0.5f + yuv2rgb[k][2] * 0.5f +
						   yuv2rgb[k][3])));
	}
	for (int z = 0; z < n; z++)
		for (int y = 0; y < n; y++)
			for (int x = 0; x < n; x++) {
				int g[3] = { x, y, z };
				float e[3], yuv[3], rgb[3], cc[3];

				for (int k = 0; k < 3; k++)
					e[k] = linear_to_bt709(c->min[k] + (c->max[k] - c->min[k]) * hf((float)g[k] / (n - 1)));
				for (int r = 0; r < 3; r++)
					yuv[r] = rgb2yuv[r][0] * e[0] + rgb2yuv[r][1] * e[1] + rgb2yuv[r][2] * e[2] + rgb2yuv[r][3];
				/* the correction cube is indexed by full-range BT.601 "RGB" of those codes */
				float cb = yuv[1] - 0.5f, cr = yuv[2] - 0.5f;

				rgb[0] = yuv[0] + 1.40234f * cr;
				rgb[1] = yuv[0] - 0.344238f * cb - 0.714355f * cr;
				rgb[2] = yuv[0] + 1.77246f * cb;
				if (corr) {
					float pos[3] = { rgb[0] * (n - 1), rgb[1] * (n - 1), rgb[2] * (n - 1) };

					cube_tap(corr, n, 3, pos, cc);
				} else {
					for (int k = 0; k < 3; k++)
						cc[k] = sat(rgb[k]);
				}
				float *o = c->yuv + ((((size_t)z * n + y) * n) + x) * 3;

				o[0] = q8(0.299072f * cc[0] + 0.586914f * cc[1] + 0.114014f * cc[2]);
				o[1] = q8(-0.168701f * cc[0] - 0.331299f * cc[1] + 0.5f * cc[2] + 0.5f);
				o[2] = q8(0.5f * cc[0] - 0.418701f * cc[1] - 0.0812988f * cc[2] + 0.5f);
			}
	free(corr);
	return 0;
}

void sl_cube_free(struct sl_cube *c)
{
	free(c->yuv);
	c->yuv = NULL;
}

/* ---- 6. full size ---- */

void sl_relight_nv12(const uint8_t *y, const uint8_t *uv, int w, int h, int stride, const struct sl_yuv *m,
		     const float *gain, int gw, int gh, const struct sl_params *p, const struct sl_cube *c,
		     uint8_t *oy, uint8_t *ouv)
{
	float kg = 1 - m->kr - m->kb, ys = m->full_range ? 1 : 255.0f / 219, cs = m->full_range ? 1 : 255.0f / 224;
	float yo = m->full_range ? 0 : 16;
	float rv = 2 * (1 - m->kr), gu = -2 * (1 - m->kb) * m->kb / kg, gv = -2 * (1 - m->kr) * m->kr / kg;
	float bu = 2 * (1 - m->kb);

	for (int j = 0; j < h; j++)
		for (int i = 0; i < w; i++) {
			float u = (i + 0.5f) / w, v = (j + 0.5f) / h, g[4], rgb[3], o[3];
			float du = hf(u) - 0.5f, dv = hf(v) - 0.5f;
			float vig = powf(1 - sqrtf(du * du + dv * dv) * 2 / sqrtf(2), p->vignette_pow);
			float lo = p->hl_lo[0] + (p->hl_hi[0] - p->hl_lo[0]) * vig;
			float hi = p->hl_lo[1] + (p->hl_hi[1] - p->hl_lo[1]) * vig;
			const uint8_t *cc = uv + (size_t)(j / 2) * stride + (i / 2) * 2;
			float yy = (y[(size_t)j * stride + i] - yo) * ys / 255;
			float cb = (cc[0] - 128) * cs / 255, cr = (cc[1] - 128) * cs / 255;
			float e[3] = { yy + rv * cr, yy + gu * cb + gv * cr, yy + bu * cb };

			bilinear(gain, gw, gh, 4, u, v, g);
			for (int r = 0; r < 3; r++)
				rgb[r] = bt709_to_linear(hf(sat(e[r])));
			float lum = 0.211792f * rgb[0] + 0.712402f * rgb[1] + 0.0718994f * rgb[2];
			float t = sat((lum - hf(lo)) / (hf(hi) - hf(lo))), s = t * t * (3 - 2 * t);

			for (int k = 0; k < 3; k++) {
				float gg = h16_tex(g[k]) + (1 - h16_tex(g[k])) * s;

				o[k] = (gg * rgb[k] - c->min[k]) / (c->max[k] - c->min[k]) * (c->n - 1);
			}
			float out[3];

			cube_tap(c->yuv, c->n, 3, o, out);
			oy[(size_t)j * stride + i] = (uint8_t)lrintf(sat(out[0]) * 255);
			if (!(i & 1) && !(j & 1)) {
				ouv[(size_t)(j / 2) * stride + i] = (uint8_t)lrintf(sat(out[1]) * 255);
				ouv[(size_t)(j / 2) * stride + i + 1] = (uint8_t)lrintf(sat(out[2]) * 255);
			}
		}
}

int sl_render_nv12(const uint8_t *y, const uint8_t *uv, int w, int h, int stride, const struct sl_yuv *m,
		   const struct sl_params *p, const float *guide, int gdw, int gdh, const float *disp, int dw, int dh,
		   const struct sl_face *faces, int nfaces, const struct sl_light *l, float focus_offset,
		   const struct sl_cube *c, uint8_t *oy, uint8_t *ouv)
{
	/* the sizes for a 720p frame: gain 320x180 (a quarter), upsampled to 640x360 */
	int gw = w / 4, gh = h / 4, uw = w / 2, uh = h / 2, r = -1;
	float *diff = malloc((size_t)dw * dh * sizeof(float)), *d2 = malloc((size_t)dw * dh * sizeof(float));
	float *mask = malloc((size_t)gw * gh * 2 * sizeof(float)), *gain = malloc((size_t)gw * gh * 4 * sizeof(float));
	float *blur = malloc((size_t)gw * gh * 4 * sizeof(float)), *up = malloc((size_t)uw * uh * 4 * sizeof(float));

	if (!diff || !d2 || !mask || !gain || !blur || !up)
		goto out;
	sl_diffuse(disp, dw, dh, p, diff);
	for (int i = 0; i < dw * dh; i++)
		d2[i] = hf(disp[i] * 1.94097f);
	sl_light_mask(faces, nfaces, (float)w / h, gw, gh, mask);
	sl_gain(guide, gdw, gdh, diff, d2, dw, dh, mask, p, l, focus_offset, gain, gw, gh);
	sl_gain_blur(gain, gw, gh, blur);
	sl_gain_upsample(blur, gw, gh, up, uw, uh);
	sl_relight_nv12(y, uv, w, h, stride, m, up, uw, uh, p, c, oy, ouv);
	r = 0;
out:
	free(diff); free(d2); free(mask); free(gain); free(blur); free(up);
	return r;
}
