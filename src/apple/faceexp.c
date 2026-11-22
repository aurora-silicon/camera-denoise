// SPDX-License-Identifier: MIT
/* Studio Light's face exposure statistics; see faceexp.h. */
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "faceexp.h"
#include "tuning.h"

typedef _Float16 h16;

int fexp_params_read(struct fexp_params *p, const char *path, char *err, size_t errlen)
{
	float person_only = 0;
	const struct apple_tuning_key k[] = {
		{ "mask-threshold", &p->mask_threshold },
		{ "target-median.1", &p->target_median[0] }, { "target-median.2", &p->target_median[1] },
		{ "target-median.3", &p->target_median[2] }, { "target-median.4", &p->target_median[3] },
		{ "target-median.5", &p->target_median[4] }, { "target-median.6", &p->target_median[5] },
		{ "max-boost.1", &p->max_boost[0] }, { "max-boost.2", &p->max_boost[1] },
		{ "max-boost.3", &p->max_boost[2] }, { "max-boost.4", &p->max_boost[3] },
		{ "max-boost.5", &p->max_boost[4] }, { "max-boost.6", &p->max_boost[5] },
		{ "min-face-size", &p->min_face_size },
		{ "max-curve-boost", &p->max_curve_boost }, { "min-curve-boost", &p->min_curve_boost },
		{ "max-target-ratio-darkening", &p->max_target_ratio_darkening },
		{ "max-target-ratio-limit", &p->max_target_ratio_limit },
		{ "bias", &p->bias_factor },
		{ "tone-similarity-sigma", &p->tone_similarity_sigma },
		{ "face-difference-threshold", &p->face_exp_dif_threshold },
		{ "person-mask-only", &person_only },
	};

	memset(p, 0, sizeof(*p));
	if (apple_tuning_read(path, k, sizeof(k) / sizeof(k[0]), err, errlen))
		return -1;
	p->relight_only_person_mask = person_only != 0;
	return 0;
}

static float sat(float v)
{
	return v < 0 ? 0 : v > 1 ? 1 : v;
}

static float smooth(float t)
{
	return t * t * (3 - 2 * t);
}

/* fast_round: half away from zero */
static uint32_t rnd(float v)
{
	return (uint32_t)floorf(v + 0.5f);
}

static float nearest(const float *img, int w, int h, float u, float v)
{
	int x = (int)floorf(u * (float)w), y = (int)floorf(v * (float)h);

	x = x < 0 ? 0 : x >= w ? w - 1 : x;
	y = y < 0 ? 0 : y >= h ? h - 1 : y;
	return img[(size_t)y * w + x];
}

static float bilinear(const float *img, int w, int h, float u, float v)
{
	float x = u * (float)w - 0.5f, y = v * (float)h - 0.5f;
	int x0 = (int)floorf(x), y0 = (int)floorf(y);
	/* the texture unit weights the taps by 8-bit fractions (one sample on each face of srl3 crossed 0.3 without) */
	float fx = rintf((x - x0) * 256.0f) / 256.0f, fy = rintf((y - y0) * 256.0f) / 256.0f;
	int xa = x0 < 0 ? 0 : x0 >= w ? w - 1 : x0, xb = x0 + 1 < 0 ? 0 : x0 + 1 >= w ? w - 1 : x0 + 1;
	int ya = y0 < 0 ? 0 : y0 >= h ? h - 1 : y0, yb = y0 + 1 < 0 ? 0 : y0 + 1 >= h ? h - 1 : y0 + 1;
	float a = img[ya * w + xa], b = img[ya * w + xb], c = img[yb * w + xa], d = img[yb * w + xb];

	return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
}

/* a filtered fp16 texel as the texture unit returns it: rounded half away from zero, denormals flushed */
static float h16_tex(float v)
{
	int e;

	if (v == 0 || !isfinite(v))
		return v;
	frexpf(fabsf(v), &e);
	float ulp = e - 1 < -14 ? ldexpf(1, -24) : ldexpf(1, e - 11), r = floorf(fabsf(v) / ulp + 0.5f) * ulp;

	return r < ldexpf(1, -14) ? copysignf(0, v) : copysignf(r, v);
}

static int bin64(float v)
{
	h16 b = (h16)floorf((float)(h16)(sat(v) * 64));

	return b < 0 ? 0 : b > 63 ? 63 : (int)b;
}

/*
 * The interpolated position in bin t: (t + num / den) / 64, in half. A half of the quotient added to t in half
 * to t in half, but fast math keeps the sum in float (srl3: 19/70 rounds to a half tie otherwise, 1 ulp off).
 */
static float at(int t, float frac)
{
	return (float)(h16)((h16)((float)t + frac) * (h16)0.015625f);
}

/*
 * The percentile of rank r over an inclusive cumulative histogram, in one of Portrait's three forms (the
 * threads test their own bin; t = 0 also takes ranks below it in the low and high forms).
 */
enum { PCT_MID, PCT_LOW, PCT_HIGH };
static int percentile(const uint32_t *cum, uint32_t r, int form, float *out)
{
	for (int t = 0; t < 64; t++) {
		uint32_t lo = cum[t], hi = t < 63 ? cum[t + 1] : cum[63] + 1;
		int hit;

		if (form == PCT_MID)
			hit = (r >= lo && r < hi) || (t == 0 && r < lo);
		else if (form == PCT_LOW)
			hit = (lo <= r && r < hi) || (t == 0 && lo > r);
		else
			hit = !(lo >= r || r > hi) || (t == 0 && lo >= r);
		if (!hit)
			continue;
		if (form == PCT_MID)
			*out = at(t, (float)(uint32_t)(r - lo) / (float)(uint32_t)(hi - lo));
		else
			*out = at(t, fmaxf(0, (float)r - (float)lo) / fmaxf(1, (float)hi - (float)lo));
		return 1;
	}
	return 0;
}

static void scan(const int *hist, uint32_t *cum)
{
	uint32_t s = 0;

	for (int i = 0; i < 64; i++)
		cum[i] = s += (uint32_t)hist[i];
}

void fexp_luma_half(const uint8_t *y, int w, int h, int stride, float *out)
{
	for (int j = 0; j < h / 2; j++)
		for (int i = 0; i < w / 2; i++) {
			const uint8_t *r = y + (size_t)(2 * j) * stride + 2 * i;
			float m = (float)(r[0] + r[1] + r[stride] + r[stride + 1]) / (4 * 255.0f);

			out[(size_t)j * (w / 2) + i] = floorf((float)(h16)m * 255 + 0.5f) / 255;
		}
}

void fexp_global(const float *luma, int lw, int lh, struct fexp_global *g)
{
	uint32_t cum[64];

	memset(g, 0, sizeof(*g));
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 64; x++)
			g->hist[bin64((float)(h16)nearest(luma, lw, lh, ((float)x + 0.5f) * 0.015625f,
							  ((float)y + 0.5f) * 0.015625f))]++;
	scan(g->hist, cum);
	g->valid = cum[63] != 0;
	/* the median uses the plain test only (no t = 0 fallback) */
	uint32_t r = rnd((float)cum[63] * 0.5f);

	for (int t = 0; t < 64; t++) {
		uint32_t lo = cum[t], hi = t < 63 ? cum[t + 1] : cum[63] + 1;

		if (r >= lo && r < hi)
			g->median = at(t, (float)(r - lo) / (float)(hi - lo));
	}
	percentile(cum, rnd((float)cum[63] * 0.99f), PCT_LOW, &g->p99);
}

void fexp_face_stats(const float *luma, int lw, int lh, const float *skin, const float *person, int mw, int mh,
		    const struct fexp_face *f, const struct fexp_params *p, struct fexp_face_stats *s)
{
	int whole[64] = { 0 };
	uint32_t cs[64], cw[64];
	float sx = f->w * 0.015625f, sy = f->h * 0.015625f;

	memset(s, 0, sizeof(*s));
	memcpy(s->tone, f->tone, sizeof(s->tone));
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 64; x++) {
			float u = ((float)x + 0.5f) * sx + f->x, v = ((float)y + 0.5f) * sy + f->y;
			float sk = h16_tex(bilinear(skin, mw, mh, u, v)), pe = h16_tex(bilinear(person, mw, mh, u, v));
			int b = bin64((float)(h16)nearest(luma, lw, lh, u, v));

			if (!(p->mask_threshold > sk))
				s->hist[b]++;
			if (p->mask_threshold < pe)
				whole[b]++;
		}
	scan(s->hist, cs);
	scan(whole, cw);
	s->valid = cs[63] && cw[63];
	s->instance = 4;                /* faceInstances: no instance votes in this variant */
	s->instance_conf = smooth(sat((0 - 400.0f) * 0.01f));
	s->size = fmaxf(f->w, f->h);
	percentile(cs, rnd((float)cs[63] * 0.5f), PCT_MID, &s->skin_median);
	uint32_t r = rnd((float)cs[63] * 0.05f);

	percentile(cs, r < 1 ? 1 : r, PCT_LOW, &s->skin_lo);
	percentile(cs, rnd((float)cs[63] * 0.95f), PCT_HIGH, &s->skin_hi);
	r = rnd((float)cw[63] * 0.01f);
	percentile(cw, r < 1 ? 1 : r, PCT_LOW, &s->face_lo);
	percentile(cw, rnd((float)cw[63] * 0.95f), PCT_HIGH, &s->face_hi);
}

/*
 * Portrait's boost curve value for a boost b at a mean m: 1 - (b m - 1) / ((m - 0.5) 2b - 1), rounded to half.
 * With b and m and b m - 1 and 2b in half, the reference is 1 ulp off; it runs the chain in float (fast
 * math): only the result is rounded. Half inputs, or a half product, are 1 ulp off on srl3 or srl4.
 */
static float curve(float b, float m)
{
	return (float)(h16)(1 - (b * m - 1) / ((m - 0.5f) * 2 * b - 1));
}

static float clampb(const struct fexp_params *p, float v)
{
	return fmaxf(fminf(v, p->max_curve_boost), p->min_curve_boost);
}

void fexp_coefficients(const struct fexp_params *p, const struct fexp_global *g, struct fexp_face_stats *s, int nfaces,
		      float exp_bias, float face_exp_ratio, struct fexp_coeffs *c)
{
	float eb = fminf(fmaxf(exp_bias, -2), 2), tm[6], mb[6];
	unsigned char seen[5] = { 0 };
	int low_skin = 0, flagged = 0;    /* flagged: an invalid face, or one without a new confident instance */
	unsigned bits = 3;
	float sum_mean = 0, min_adj = 1, max_adj = 0, sum_bmax = 0, sum_adj = 0, cnt = 0, face_hi = 0;
	float a57 = 1, m58 = 1;

	memset(c, 0, sizeof(*c));
	for (int k = 0; k < 6; k++) {
		tm[k] = p->target_median[k] + p->bias_factor * eb;
		mb[k] = p->max_boost[k];
	}
	c->c6 = 1;
	for (int i = 0; i < nfaces; i++) {
		struct fexp_face_stats *f = &s[i];

		if (!f->valid) {
			c->flags2 |= 2048;
			if (!(f->size < p->min_face_size))
				bits &= ~2u;
			flagged = 1;
			continue;
		}
		if (!(f->size > p->min_face_size)) {
			bits &= ~1u;
			continue;
		}
		int count = 0;
		float acc = 0;

		for (int b = 0; b < 64; b++)
			count += f->hist[b];
		for (int b = 0; b < 64; b++)
			acc += (float)f->hist[b] * 4 * (float)b;
		f->count = count;
		float mean = count ? acc / ((float)count * 255) : f->skin_median;

		f->mean = mean;
		if (count < 50) {
			c->low_skin = 1;
			low_skin = 1;
			a57 = 0;
		}
		if (f->skin_median < m58) {
			face_hi = f->face_hi;
			m58 = f->skin_median;
		}
		int seenflag = 0;

		if (f->instance == 4) {
			c->flags2 |= 1024;
			seenflag = 1;
		}
		if (!seen[f->instance] && f->instance_conf > 0.9f)
			seen[f->instance] = 1;
		else {
			c->flags2 |= 512;
			seenflag = 1;
		}
		flagged |= seenflag;
		float sa = 0, sb = 0, sp = 0;

		for (int k = 0; k < 6; k++) {
			sa += tm[k] * f->tone[k];
			sb += mb[k] * f->tone[k];
			sp += f->tone[k];
		}
		float tgt = sa / sp, bmax = sb / sp, d = mean - tgt;
		float t1 = sat(d * 10), t2 = sat((mean - 0.45f) * 9.9999981f);
		float rng = f->skin_hi - f->skin_lo;
		float t3 = sat((rng - 0.6f) * 5.0000005f), t4 = sat((rng - 0.3f) * 3.3333333f);

		a57 = fminf(a57, smooth(t4));
		c->c6 = a57;
		float ratio = mean > 0 ? tgt / mean : 0;

		f->target = tgt;
		float adj = (t2 * t1) * (t2 * t1) * (d * 0.65f) * (3 - 2 * t1) * (3 - 2 * t2) + tgt;

		adj = t3 * t3 * 0.8f * (3 - 2 * t3) * (f->skin_median - adj) + adj;
		f->target_adj = adj;
		float t5 = sat((0.6f - bmax + ratio) * 3.3333333f), t6 = sat((rng - 0.4f) * 4.9999995f);

		f->boost_max = bmax - (t6 * t5) * (t6 * t5) * 0.5f * (3 - 2 * t5) * (3 - 2 * t6);
		max_adj = fmaxf(max_adj, adj);
		min_adj = fminf(min_adj, adj);
		cnt += 1;
		sum_adj += adj;
		sum_mean += mean;
		sum_bmax += f->boost_max;
		bits &= ~3u;
	}
	for (int k = 0; k < 4; k++)
		c->seen[k] = seen[k] ? 1 : 0;
	c->c8 = face_hi;
	int together = fabsf(max_adj - min_adj) < p->face_exp_dif_threshold || nfaces > 3 || flagged;

	c->p99 = g->p99;
	c->flags = bits | (g->valid ? 0 : 8);
	c->c10 = 0;
	for (int k = 0; k < 5; k++)
		c->boost[k] = 0.5f;
	float c5;

	if (!together)
		c->single = p->relight_only_person_mask;
	if (!together && !p->relight_only_person_mask) {
		/* per face, each weighted by the faces of a similar tone */
		float hiv = 0.5f, lov = 0.5f;

		for (int i = 0; i < nfaces; i++) {
			struct fexp_face_stats *f = &s[i];

			if (!f->valid || !(f->size > p->min_face_size))
				continue;
			float acc = 0, wsum = 0;

			for (int j = 0; j < nfaces; j++) {
				if (!s[j].valid || !(s[j].size > p->min_face_size))
					continue;
				float q = fabsf(s[j].target_adj - f->target_adj) / p->tone_similarity_sigma, w = expf(-q * q);

				acc += s[j].mean * w;
				wsum += w;
			}
			if (f->instance >= 4)
				continue;
			float avg = wsum > 0 ? acc / wsum : 1, m = f->mean;
			float sc = (m - c->c10) * c->p99 / (c->p99 - c->c10);

			sc = sc > 0 ? sc : m;
			float r = fminf(m / avg, 1.2f) * f->target_adj / sc;
			float b = fmaxf(fminf(f->boost_max, r), p->max_target_ratio_darkening);
			float ext = b < 1 ? (1 - b) * 0.5f : 0, bb = ext + b;

			c->boost[f->instance] = clampb(p, curve(bb, m));
			c->gain[f->instance] = ext > 0 ? bb : 1;
			float plain = clampb(p, curve(b, m));

			hiv = fmaxf(plain, hiv);
			lov = fminf(plain, lov);
		}
		float t = sat((face_exp_ratio - 1.8f) * 2.4999995f), w = 0.5f - t * t * 0.25f * (3 - 2 * t);

		c5 = w * (hiv - 1 + lov) + 0.5f;
		c->c13 = lov;
	} else {
		if (together)
			c->single = 1;
		float r = sum_bmax / cnt, m = sum_mean / cnt;

		c->c7 = m;
		float b = fmaxf(fminf(r, sum_adj / (m * cnt)), p->max_target_ratio_darkening);
		float ext = b < 1 ? (1 - b) * 0.5f : 0, bb = ext + b;

		c->boost[4] = clampb(p, curve(bb, m));
		c->gain[4] = ext > 0 ? bb : 1;
		c->c13 = clampb(p, curve(b, m));
		if (ext > 0) {
			c5 = c->boost[4];
		} else {
			float t = sat((face_exp_ratio - 1.8f) * 2.4999995f), w = 0.5f - t * t * 0.25f * (3 - 2 * t);

			c5 = w * (c->boost[4] - 0.5f) + 0.5f;
		}
	}
	c->c5 = c5;
	if (low_skin || c->flags) {
		c->c13 = 0.5f;
		c->c6 = 0;
		c->c7 = 0.5f;
	}
}
