// SPDX-License-Identifier: MIT
#include "portrait.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static int clampi(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static float smoothstep(float e0, float e1, float v)
{
	float t = clampf((v - e0) / (e1 - e0), 0, 1);

	return t * t * (3 - 2 * t);
}

/* --- network input ------------------------------------------------------- */

void apple_nv12_to_rgb(const struct cdn_frame *in, int matrix, int full_range,
		       float *r, float *g, float *b, int w, int h)
{
	/* Y'CbCr -> R'G'B' (BT.601 or BT.709), video or full range */
	float kr = matrix == 709 ? 1.5748f : 1.402f;
	float kgb = matrix == 709 ? 0.187324f : 0.344136f;
	float kgr = matrix == 709 ? 0.468124f : 0.714136f;
	float kb = matrix == 709 ? 1.8556f : 1.772f;
	float ys = full_range ? 1.0f / 255 : 1.0f / 219, yo = full_range ? 0 : 16;
	float cs = full_range ? 1.0f / 255 : 1.0f / 224;
	float fx = (float)in->width / w, fy = (float)in->height / h;
	/* area average over the footprint: 4 x 4 samples per output pixel */
	const int n = 4;

	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			float sy = 0, su = 0, sv = 0;

			for (int j = 0; j < n; j++) {
				int py = clampi((int)((y + (j + 0.5f) / n) * fy), 0, in->height - 1);
				const uint8_t *row = in->y + (size_t)py * in->stride_y;
				const uint8_t *crow = in->uv + (size_t)(py / 2) * in->stride_uv;

				for (int i = 0; i < n; i++) {
					int px = clampi((int)((x + (i + 0.5f) / n) * fx), 0, in->width - 1);

					sy += row[px];
					su += crow[(px & ~1)];
					sv += crow[(px & ~1) + 1];
				}
			}
			float yy = (sy / (n * n) - yo) * ys;
			float cb = (su / (n * n) - 128) * cs, cr = (sv / (n * n) - 128) * cs;
			size_t o = (size_t)y * w + x;

			r[o] = clampf(yy + kr * cr, 0, 1);
			g[o] = clampf(yy - kgb * cb - kgr * cr, 0, 1);
			b[o] = clampf(yy + kb * cb, 0, 1);
		}
	}
}

void apple_masks_update(struct apple_masks *m, const float *person, const float *skin, float a)
{
	/*
	 * a is the weight of the new run where the mask holds still: that
	 * hides the network's flicker. Where it changed by MOTION or more the
	 * new run is taken as it is, so a moving person is not trailed by an
	 * old mask (the lag of a plain running mean).
	 */
	const float motion = 0.25f;

	if (!m->valid)
		a = 1;
	for (int i = 0; i < APPLE_SEG_W * APPLE_SEG_H; i++) {
		float np = clampf(person[i], 0, 1), ns = clampf(skin[i], 0, 1);
		float d = fabsf(np - m->person[i]);
		float w = a + (1 - a) * clampf(d / motion, 0, 1);

		m->person[i] += w * (np - m->person[i]);
		m->skin[i] += w * (ns - m->skin[i]);
	}
	m->valid = 1;
}

/* --- the subject for Center Stage and Studio Light ------------------------- */

void apple_masks_subject(const struct apple_masks *m, struct apple_subject *s, float aspect)
{
	enum { W = APPLE_SEG_W, H = APPLE_SEG_H };
	int cols[W] = { 0 }, rows[H] = { 0 }, total = 0;

	memset(s, 0, sizeof(*s));
	if (!m->valid)
		return;
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
			if (m->person[y * W + x] > 0.5f) {
				cols[x]++;
				rows[y]++;
				total++;
			}
	/* someone: at least 0.5 % of the picture */
	if (total < W * H / 200)
		return;
	/* the extent, trimmed of 1 % stray pixels each side */
	int acc = 0, x0 = 0, x1 = W - 1, y0 = 0, y1 = H - 1;

	for (x0 = 0, acc = 0; x0 < W && (acc += cols[x0]) < total / 100; x0++)
		;
	for (x1 = W - 1, acc = 0; x1 > 0 && (acc += cols[x1]) < total / 100; x1--)
		;
	for (y0 = 0; y0 < H && rows[y0] < 2; y0++)
		;
	for (y1 = H - 1; y1 > 0 && rows[y1] < 2; y1--)
		;
	if (x1 <= x0 || y1 <= y0)
		return;
	/* people: column runs with a person, apart by more than 8 % of the width */
	int people = 0, gap = W, run = 0;

	for (int x = 0; x < W; x++) {
		if (cols[x] > 3) {
			if (!run && gap > W * 8 / 100)
				people++;
			run = 1;
			gap = 0;
		} else {
			run = 0;
			gap++;
		}
	}
	/* the head: skin in the top 45 % of the person (the face, not the hands) */
	int band = y0 + (y1 - y0) * 45 / 100, skin = 0, skin_bottom = y0;
	int widths[H], nw = 0;
	float sx = 0;

	for (int y = y0; y <= band; y++) {
		int row = 0;

		for (int x = x0; x <= x1; x++)
			if (m->skin[y * W + x] > 0.5f && m->person[y * W + x] > 0.3f) {
				skin++;
				row++;
				sx += x;
				skin_bottom = y;
			}
		if (row)
			widths[nw++] = row;
	}
	float head_x, head_h;

	if (skin >= 20) {
		head_x = sx / skin;
		head_h = (skin_bottom - y0 + 1) * 1.1f;
		/*
		 * A hand at the chin or an open collar stretch the skin down: a
		 * head is about 1.5 faces wide, and the face is the median skin
		 * row (in mask rows: the mask is the frame stretched to 4:3).
		 */
		if (aspect > 0 && nw >= 3) {
			for (int i = 1; i < nw; i++)
				for (int j = i; j > 0 && widths[j - 1] > widths[j]; j--) {
					int t = widths[j];

					widths[j] = widths[j - 1];
					widths[j - 1] = t;
				}
			float face_w = widths[nw / 2] * aspect * H / W;

			head_h = fminf(head_h, 1.5f * face_w);
		}
	} else {
		/* no face visible: the middle of the person's top 15 % */
		int top = y0 + (y1 - y0) * 15 / 100, n = 0;
		float cx = 0;

		for (int y = y0; y <= top; y++)
			for (int x = x0; x <= x1; x++)
				if (m->person[y * W + x] > 0.5f) {
					cx += x;
					n++;
				}
		head_x = n ? cx / n : (x0 + x1) / 2.0f;
		head_h = (y1 - y0) * 0.25f;
	}
	head_h = clampf(head_h, (y1 - y0) * 0.1f, (y1 - y0) * 0.6f + 1);
	s->present = true;
	s->people = people > 1 ? 2 : 1;
	s->x0 = (float)x0 / W;
	s->x1 = (float)(x1 + 1) / W;
	s->y0 = (float)y0 / H;
	s->y1 = (float)(y1 + 1) / H;
	s->head_x = (head_x + 0.5f) / W;
	s->head_top = (float)y0 / H;
	s->head_h = head_h / H;
}

/* --- quarter-size refinement and the effects ------------------------------- */

struct apple_portrait {
	int width, height, qw, qh;
	float *guide, *alpha, *skin;       /* qw x qh */
	float *t0, *t1, *t2, *t3, *t4;     /* scratch, qw x qh */
	float *by, *bu, *bv;               /* blurred background, qw x qh */
	/* upsampling to the frame: per column the two quarter-size taps and the
	 * weight (luma columns, then chroma columns), and four quarter-size rows */
	int *lx0, *lx1, *cx0, *cx1;
	float *lfx, *cfx;
	float *r0, *r1, *r2, *r3;
	uint8_t lut[64][256];              /* Studio Light curve per light level */
	float lut_gain;                    /* the key gain the curves are for */
	float face_luma;                   /* the face's mean luma, smoothed; < 0 = none yet */
};

/* bilinear taps at quarter-size coordinate q (pixel centres at +0.5) */
static void taps(float q, int n, int *i0, int *i1, float *f)
{
	q -= 0.5f;
	*i0 = clampi((int)floorf(q), 0, n - 1);
	*i1 = clampi(*i0 + 1, 0, n - 1);
	*f = clampf(q - *i0, 0, 1);
}

struct apple_portrait *apple_portrait_new(int width, int height)
{
	struct apple_portrait *p = calloc(1, sizeof(*p));

	if (!p)
		return NULL;
	p->width = width;
	p->height = height;
	p->qw = (width + 3) / 4;
	p->qh = (height + 3) / 4;
	size_t n = (size_t)p->qw * p->qh;
	float **bufs[] = { &p->guide, &p->alpha, &p->skin, &p->t0, &p->t1, &p->t2, &p->t3,
			   &p->t4, &p->by, &p->bu, &p->bv };

	for (size_t i = 0; i < sizeof(bufs) / sizeof(bufs[0]); i++) {
		*bufs[i] = calloc(n, sizeof(float));
		if (!*bufs[i]) {
			apple_portrait_free(p);
			return NULL;
		}
	}
	p->lx0 = calloc((size_t)width, sizeof(int));
	p->lx1 = calloc((size_t)width, sizeof(int));
	p->lfx = calloc((size_t)width, sizeof(float));
	p->cx0 = calloc((size_t)width / 2 + 1, sizeof(int));
	p->cx1 = calloc((size_t)width / 2 + 1, sizeof(int));
	p->cfx = calloc((size_t)width / 2 + 1, sizeof(float));
	p->r0 = calloc((size_t)p->qw, sizeof(float));
	p->r1 = calloc((size_t)p->qw, sizeof(float));
	p->r2 = calloc((size_t)p->qw, sizeof(float));
	p->r3 = calloc((size_t)p->qw, sizeof(float));
	if (!p->lx0 || !p->lx1 || !p->lfx || !p->cx0 || !p->cx1 || !p->cfx ||
	    !p->r0 || !p->r1 || !p->r2 || !p->r3) {
		apple_portrait_free(p);
		return NULL;
	}
	for (int x = 0; x < width; x++)
		taps((x + 0.5f) / 4, p->qw, &p->lx0[x], &p->lx1[x], &p->lfx[x]);
	for (int x = 0; x < width / 2; x++)
		taps((2 * x + 1.0f) / 4, p->qw, &p->cx0[x], &p->cx1[x], &p->cfx[x]);
	p->lut_gain = -1;
	p->face_luma = -1;
	return p;
}

void apple_portrait_free(struct apple_portrait *p)
{
	if (!p)
		return;
	free(p->guide); free(p->alpha); free(p->skin);
	free(p->t0); free(p->t1); free(p->t2); free(p->t3); free(p->t4);
	free(p->by); free(p->bu); free(p->bv);
	free(p->lx0); free(p->lx1); free(p->lfx); free(p->cx0); free(p->cx1); free(p->cfx);
	free(p->r0); free(p->r1); free(p->r2); free(p->r3);
	free(p);
}

const float *apple_portrait_alpha(const struct apple_portrait *p, int *w, int *h)
{
	*w = p->qw;
	*h = p->qh;
	return p->alpha;
}

/* Box mean of radius r (window 2r+1, clipped at the edges), separable, src -> dst.
 * The vertical pass runs over whole rows (a running column sum), not down
 * the columns: contiguous, so it vectorizes and stays in the cache. */
static __thread float *box_acc;
static __thread int box_acc_n;

static void box(const float *src, float *dst, float *tmp, int w, int h, int r)
{
	for (int y = 0; y < h; y++) {
		const float *s = src + (size_t)y * w;
		float *d = tmp + (size_t)y * w, sum = 0;
		int lo = 0, hi = -1;

		for (int x = 0; x < w; x++) {
			int a = x - r < 0 ? 0 : x - r, b = x + r >= w ? w - 1 : x + r;

			while (hi < b)
				sum += s[++hi];
			while (lo < a)
				sum -= s[lo++];
			d[x] = sum / (b - a + 1);
		}
	}
	if (box_acc_n < w) {
		float *n = realloc(box_acc, sizeof(float) * (size_t)w);

		if (!n)
			return;
		box_acc = n;
		box_acc_n = w;
	}
	float *acc = box_acc;
	int lo = 0, hi = -1;

	memset(acc, 0, sizeof(float) * (size_t)w);
	for (int y = 0; y < h; y++) {
		int a = y - r < 0 ? 0 : y - r, b = y + r >= h ? h - 1 : y + r;

		while (hi < b) {
			const float *row = tmp + (size_t)++hi * w;

			for (int x = 0; x < w; x++)
				acc[x] += row[x];
		}
		while (lo < a) {
			const float *row = tmp + (size_t)lo++ * w;

			for (int x = 0; x < w; x++)
				acc[x] -= row[x];
		}
		float inv = 1.0f / (b - a + 1);
		float *d = dst + (size_t)y * w;

		for (int x = 0; x < w; x++)
			d[x] = acc[x] * inv;
	}
}

static float sample_mask(const float *m, float x, float y)
{
	/* bilinear in the 256 x 192 mask, x and y in mask pixels (centres at +0.5) */
	x -= 0.5f;
	y -= 0.5f;
	int x0 = clampi((int)floorf(x), 0, APPLE_SEG_W - 1), y0 = clampi((int)floorf(y), 0, APPLE_SEG_H - 1);
	int x1 = clampi(x0 + 1, 0, APPLE_SEG_W - 1), y1 = clampi(y0 + 1, 0, APPLE_SEG_H - 1);
	float fx = clampf(x - x0, 0, 1), fy = clampf(y - y0, 0, 1);
	const float *a = m + y0 * APPLE_SEG_W, *b = m + y1 * APPLE_SEG_W;

	return (a[x0] * (1 - fx) + a[x1] * fx) * (1 - fy) + (b[x0] * (1 - fx) + b[x1] * fx) * fy;
}

/*
 * Guided filter (He, Sun, Tang): q = mean(a) * I + mean(b) with a, b fitted
 * per window so q follows p but keeps the edges of the guide I.
 */
static void guided(struct apple_portrait *p, const float *in, float *out, int r, float eps)
{
	int w = p->qw, h = p->qh;
	size_t n = (size_t)w * h;
	float *mi = p->t0, *mp = p->t1, *cov = p->t2, *var = p->t3, *tmp = p->t4;

	for (size_t i = 0; i < n; i++) {
		cov[i] = p->guide[i] * in[i];
		var[i] = p->guide[i] * p->guide[i];
	}
	box(p->guide, mi, tmp, w, h, r);
	box(in, mp, tmp, w, h, r);
	box(cov, cov, tmp, w, h, r);
	box(var, var, tmp, w, h, r);
	for (size_t i = 0; i < n; i++) {
		float a = (cov[i] - mi[i] * mp[i]) / (var[i] - mi[i] * mi[i] + eps);

		cov[i] = a;                  /* a */
		var[i] = mp[i] - a * mi[i];  /* b */
	}
	box(cov, cov, tmp, w, h, r);
	box(var, var, tmp, w, h, r);
	for (size_t i = 0; i < n; i++)
		out[i] = clampf(cov[i] * p->guide[i] + var[i], 0, 1);
}

void apple_portrait_refine(struct apple_portrait *p, const struct apple_masks *m,
			   const struct cdn_frame *f)
{
	int w = p->qw, h = p->qh;

	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			int sum = 0, cnt = 0;

			for (int j = 0; j < 4; j++) {
				int py = y * 4 + j;

				if (py >= f->height)
					break;
				const uint8_t *row = f->y + (size_t)py * f->stride_y;

				for (int i = 0; i < 4 && x * 4 + i < f->width; i++, cnt++)
					sum += row[x * 4 + i];
			}
			p->guide[(size_t)y * w + x] = cnt ? sum / (255.0f * cnt) : 0;
		}
	}
	float sx = (float)APPLE_SEG_W / w, sy = (float)APPLE_SEG_H / h;
	float *pp = p->by, *ps = p->bu; /* scratch until blur/light reuse them */

	/*
	 * The network is unsure (0.3-0.7) on arms, shoulders and clothes that
	 * look like the background; blending there blurs part of the person.
	 * Lean those to the person before the edges are fitted to the frame.
	 */
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			float v = m->valid ? sample_mask(m->person, (x + 0.5f) * sx, (y + 0.5f) * sy) : 0;

			pp[(size_t)y * w + x] = smoothstep(0.2f, 0.65f, v);
			ps[(size_t)y * w + x] = m->valid ? sample_mask(m->skin, (x + 0.5f) * sx, (y + 0.5f) * sy) : 0;
		}
	int r = w / 160 > 2 ? w / 160 : 2;

	guided(p, pp, p->alpha, r, 1e-3f);
	guided(p, ps, p->skin, r, 1e-3f);
	/* the guided fit leaves a soft ramp inside the edge: firm up the inside */
	for (size_t i = 0; i < (size_t)w * h; i++)
		p->alpha[i] = smoothstep(0.05f, 0.85f, p->alpha[i]);
}

/* the quarter-size row at quarter-size coordinate qy, interpolated vertically */
static void vrow(const float *q, int w, int h, float qy, float *out)
{
	int y0, y1;
	float fy;

	taps(qy, h, &y0, &y1, &fy);
	const float *a = q + (size_t)y0 * w, *b = q + (size_t)y1 * w;

	for (int x = 0; x < w; x++)
		out[x] = a[x] + (b[x] - a[x]) * fy;
}

static inline float hs(const float *row, const int *x0, const int *x1, const float *fx, int i)
{
	return row[x0[i]] + (row[x1[i]] - row[x0[i]]) * fx[i];
}

static inline uint8_t u8(float v)
{
	return v <= 0 ? 0 : v >= 255 ? 255 : (uint8_t)(v + 0.5f);
}

void apple_portrait_blur(struct apple_portrait *p, struct cdn_frame *f, float strength)
{
	int w = p->qw, h = p->qh;
	size_t n = (size_t)w * h;
	float *wt = p->t0, *tmp = p->t4, *num = p->t1, *den = p->t2;
	/*
	 * The blur's size as a Gaussian sigma in frame pixels at 1280 wide:
	 * 4 + 20 x strength (other widths scale). Three box passes of radius q
	 * at quarter size give sigma^2 = q (q + 1) quarter pixels.
	 */
	float sigma_q = (4 + 20 * clampf(strength, 0, 1)) * f->width / 1280.0f / 4;
	int pass_r = (int)lroundf(sqrtf(sigma_q * sigma_q + 0.25f) - 0.5f);

	if (pass_r < 1)
		pass_r = 1;

	/* quarter-size planes of the frame: area averages (4 x 4 luma, 2 x 2 chroma) */
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			float sy = 0, su = 0, sv = 0;
			int ny = 0, nc = 0;

			for (int j = 0; j < 4; j++) {
				int py = clampi(y * 4 + j, 0, f->height - 1);
				const uint8_t *row = f->y + (size_t)py * f->stride_y;

				for (int i = 0; i < 4; i++, ny++)
					sy += row[clampi(x * 4 + i, 0, f->width - 1)];
			}
			for (int j = 0; j < 2; j++) {
				int cy = clampi(y * 2 + j, 0, f->height / 2 - 1);
				const uint8_t *row = f->uv + (size_t)cy * f->stride_uv;

				for (int i = 0; i < 2; i++, nc++) {
					int cx = clampi(x * 2 + i, 0, f->width / 2 - 1);

					su += row[2 * cx];
					sv += row[2 * cx + 1];
				}
			}
			size_t o = (size_t)y * w + x;

			p->by[o] = sy / ny;
			p->bu[o] = su / nc;
			p->bv[o] = sv / nc;
		}
	/* normalized convolution of the background only: no halo from the person */
	for (size_t i = 0; i < n; i++)
		wt[i] = 1.0f - p->alpha[i];
	float *planes[3] = { p->by, p->bu, p->bv };

	box(wt, den, tmp, w, h, pass_r);
	box(den, den, tmp, w, h, pass_r);
	box(den, den, tmp, w, h, pass_r);
	for (int c = 0; c < 3; c++) {
		float *pl = planes[c];

		for (size_t i = 0; i < n; i++)
			num[i] = pl[i] * wt[i];
		box(num, num, tmp, w, h, pass_r);
		box(num, num, tmp, w, h, pass_r);
		box(num, num, tmp, w, h, pass_r);
		for (size_t i = 0; i < n; i++)
			pl[i] = den[i] > 1e-3f ? num[i] / den[i] : pl[i];
	}
	/* composite at full size: a = person, kept sharp */
	for (int y = 0; y < f->height; y++) {
		uint8_t *row = f->y + (size_t)y * f->stride_y;
		float qy = (y + 0.5f) / 4;

		vrow(p->alpha, w, h, qy, p->r0);
		vrow(p->by, w, h, qy, p->r1);
		for (int x = 0; x < f->width; x++) {
			float a = hs(p->r0, p->lx0, p->lx1, p->lfx, x);

			if (a > 0.995f)
				continue;
			float bg = hs(p->r1, p->lx0, p->lx1, p->lfx, x);

			row[x] = u8(a * row[x] + (1 - a) * bg);
		}
	}
	for (int y = 0; y < f->height / 2; y++) {
		uint8_t *row = f->uv + (size_t)y * f->stride_uv;
		float qy = (2 * y + 1.0f) / 4;

		vrow(p->alpha, w, h, qy, p->r0);
		vrow(p->bu, w, h, qy, p->r1);
		vrow(p->bv, w, h, qy, p->r2);
		for (int x = 0; x < f->width / 2; x++) {
			float a = hs(p->r0, p->cx0, p->cx1, p->cfx, x);

			if (a > 0.995f)
				continue;
			float u = hs(p->r1, p->cx0, p->cx1, p->cfx, x), v = hs(p->r2, p->cx0, p->cx1, p->cfx, x);

			row[2 * x] = u8(a * row[2 * x] + (1 - a) * u);
			row[2 * x + 1] = u8(a * row[2 * x + 1] + (1 - a) * v);
		}
	}
}

/*
 * Studio Light, after the structure of Portrait.framework's PTSyntheticLight
 * (names in its default.metallib: face exposure statistics with target
 * medians and a boost limit, a light mask around the face and body with a
 * penumbra, a gain map applied to the picture, the background dimmed with a
 * vignette). The numbers are ours, tuned by eye, not Apple's:
 *  - the face is brought towards a mean luma of 0.5 (SRLv2's TargetMedian
 *    0.47-0.53 in Portrait's still-photo tuning plist; an inference for
 *    video), by at most +55 % luma at full strength, and always by at least
 *    +8 % (the key light);
 *  - the light falls off smoothly from the face over the upper body and is
 *    cut by a widely feathered person mask, so it has no edge;
 *  - the background is dimmed by at most 18 %, a little more at the
 *    picture's edges, and desaturated a little;
 *  - highlights roll off instead of clipping.
 * The face statistics are smoothed over about half a second.
 */
#define LIGHT_TARGET 0.5f
#define LIGHT_MAX_BOOST 0.55f
#define LIGHT_MIN_BOOST 0.08f
#define LIGHT_BG_DIM 0.18f

/* y * g, rolled off towards 1: identity at g = 1, f(1) = 1, monotonic for g <= 2 */
static float light_curve(float y, float g)
{
	return y * g / (1 + (g - 1) * y * y);
}

void apple_portrait_light(struct apple_portrait *p, struct cdn_frame *f,
			  const struct apple_subject *s, float strength)
{
	int w = p->qw, h = p->qh;
	size_t n = (size_t)w * h;
	float k = clampf(strength, 0, 1);
	float *light = p->t0, *soft = p->t1, *tmp = p->t4, *bg = p->t2;
	bool head = s && s->present;

	if (k <= 0)
		return;
	/* the face's mean luma: skin inside the person, in the head's box */
	if (head) {
		int hx0 = clampi((int)((s->head_x - s->head_h * 0.6f * h / w) * w), 0, w - 1);
		int hx1 = clampi((int)((s->head_x + s->head_h * 0.6f * h / w) * w), 0, w - 1);
		int hy0 = clampi((int)(s->head_top * h), 0, h - 1);
		int hy1 = clampi((int)((s->head_top + s->head_h) * h), 0, h - 1);
		float sum = 0, wsum = 0;

		for (int y = hy0; y <= hy1; y++)
			for (int x = hx0; x <= hx1; x++) {
				size_t o = (size_t)y * w + x;
				float wt = p->skin[o] * p->alpha[o];

				sum += wt * p->guide[o];
				wsum += wt;
			}
		if (wsum > 4) {
			float m = sum / wsum;

			p->face_luma = p->face_luma < 0 ? m : p->face_luma + 0.07f * (m - p->face_luma);
		}
	}
	float boost = LIGHT_MIN_BOOST;

	if (p->face_luma > 0.02f)
		boost = clampf(LIGHT_TARGET / p->face_luma - 1, LIGHT_MIN_BOOST, LIGHT_MAX_BOOST);
	float g = 1 + k * boost;

	if (fabsf(g - p->lut_gain) > 0.004f) {
		for (int l = 0; l < 64; l++) {
			float gl = 1 + (g - 1) * l / 63.0f;

			for (int y = 0; y < 256; y++)
				p->lut[l][y] = (uint8_t)clampi((int)lroundf(255 * light_curve(y / 255.0f, gl)), 0, 255);
		}
		p->lut_gain = g;
	}
	/* the person, feathered wide: no edge in the light or the dimming */
	int fr = w / 64 > 2 ? w / 64 : 2;

	box(p->alpha, soft, tmp, w, h, fr);
	box(soft, soft, tmp, w, h, fr);
	box(soft, soft, tmp, w, h, fr);
	/* the light: the face most, falling off over the shoulders (an ellipse
	 * a little taller than wide, centred below the eyes) */
	float hx = 0, hy = 0, rx = 1, ry = 1;

	if (head) {
		hx = s->head_x * w;
		hy = (s->head_top + 0.6f * s->head_h) * h;
		ry = fmaxf(1.6f * s->head_h * h, 2);
		rx = fmaxf(ry * 1.1f, 2);
	}
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			size_t o = (size_t)y * w + x;
			float fall = 0.35f;

			if (head) {
				float dx = (x + 0.5f - hx) / rx, dy = (y + 0.5f - hy) / ry;

				fall = 0.35f + 0.65f * expf(-(dx * dx + dy * dy));
			}
			light[o] = clampf(soft[o] * fall, 0, 1);
			/* the background: dimmer towards the edges (r^2 = 1 in the corners) */
			float vx = (x + 0.5f) / w - 0.5f, vy = (y + 0.5f) / h - 0.5f;
			float vig = 0.7f + 0.6f * (vx * vx + vy * vy);

			bg[o] = 1 - k * LIGHT_BG_DIM * vig * (1 - soft[o]);
		}
	(void)n;
	for (int y = 0; y < f->height; y++) {
		uint8_t *row = f->y + (size_t)y * f->stride_y;
		float qy = (y + 0.5f) / 4;

		vrow(light, w, h, qy, p->r0);
		vrow(bg, w, h, qy, p->r1);
		for (int x = 0; x < f->width; x++) {
			float l = hs(p->r0, p->lx0, p->lx1, p->lfx, x), d = hs(p->r1, p->lx0, p->lx1, p->lfx, x);
			int v = p->lut[clampi((int)(l * 63 + 0.5f), 0, 63)][row[x]];

			row[x] = u8(v * d);
		}
	}
	/* colour: a lit face keeps its saturation, the background loses a little */
	for (int y = 0; y < f->height / 2; y++) {
		uint8_t *row = f->uv + (size_t)y * f->stride_uv;
		float qy = (2 * y + 1.0f) / 4;

		vrow(light, w, h, qy, p->r0);
		vrow(bg, w, h, qy, p->r1);
		for (int x = 0; x < f->width / 2; x++) {
			float l = hs(p->r0, p->cx0, p->cx1, p->cfx, x), d = hs(p->r1, p->cx0, p->cx1, p->cfx, x);
			float sat = (1 + 0.5f * (g - 1) * l) * d;

			row[2 * x] = u8(128 + (row[2 * x] - 128) * sat);
			row[2 * x + 1] = u8(128 + (row[2 * x + 1] - 128) * sat);
		}
	}
}
