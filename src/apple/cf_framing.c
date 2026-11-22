// SPDX-License-Identifier: MIT
/*
 * Center Stage framing, see cf_framing.h.  Float vs double and the operation order are on purpose (the
 * reference traces reproduce to ~1e-6 rad); keep -ffp-contract=off and do not "simplify".
 */
#include "cf_framing.h"
#include "tuning.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define NONE INT64_MIN
#define NKF ((int)(sizeof(((struct cf_framing *)0)->kf) / sizeof(struct cf_kf)))

typedef struct cf_rect rect;

static const rect RNULL = { INFINITY, INFINITY, 0, 0 };

/* the ease-in curves the session uses (fixed: not part of the tuning) */
#define EASE_XA 4.0f
#define EASE_XB 0.2f
#define EASE_YA 2.5f
#define EASE_YB 0.2f
#define EASE_ZA 1.0f
#define EASE_ZB 0.2f

/* ---- small helpers ------------------------------------------------------------------------------------ */

static int isnull(rect r) { return r.x == INFINITY || r.y == INFINITY; }
static double minx(rect r) { return r.x; }
static double maxx(rect r) { return r.x + r.w; }
static double miny(rect r) { return r.y; }
static double maxy(rect r) { return r.y + r.h; }
static double midx(rect r) { return r.x + r.w * 0.5; }
static double midy(rect r) { return r.y + r.h * 0.5; }
static double dmin(double a, double b) { return a < b ? a : b; }
static double dmax(double a, double b) { return a > b ? a : b; }
static rect R4(double x, double y, double w, double h) { rect r = { x, y, w, h }; return r; }

/* float a*b+c with the product exact and one rounding */
static float fma32(float a, float b, float c) { return (float)((double)a * (double)b + (double)c); }
static float sq32(float a) { return a * a; }

static int64_t us(double s) { return (int64_t)(s * 1e6); }          /* CMTimeMakeWithSeconds truncates */
static double sec(int64_t a, int64_t b) { return (double)(a - b) / 1e6; }

static rect runion(rect a, rect b)
{
	if (isnull(a))
		return b;
	if (isnull(b))
		return a;
	double x0 = dmin(a.x, b.x), y0 = dmin(a.y, b.y);
	double x1 = dmax(maxx(a), maxx(b)), y1 = dmax(maxy(a), maxy(b));
	return R4(x0, y0, x1 - x0, y1 - y0);
}

static int contains_point(rect r, double x, double y)
{
	return r.x <= x && x < r.x + r.w && r.y <= y && y < r.y + r.h;
}

static int contains_rect(rect a, rect b)
{
	return a.x <= b.x && a.y <= b.y && maxx(b) <= maxx(a) && maxy(b) <= maxy(a);
}

static rect safe_intersection(rect a, const double *b)
{
	rect B = R4(b[0], b[1], b[2], b[3]);
	float x0 = (float)dmin(dmax(minx(a), minx(B)), maxx(a));
	float y0 = (float)dmin(dmax(miny(a), miny(B)), maxy(a));
	float x1 = (float)dmax(dmin(maxx(a), maxx(B)), minx(a));
	float y1 = (float)dmax(dmin(maxy(a), maxy(B)), miny(a));
	return R4(x0, y0, dmax(x1 - x0, 0.0f), dmax(y1 - y0, 0.0f));
}

/* ---- geometry ----------------------------------------------------------------------------------------- */

static float gH(const struct cf_framing *f, float w) { return 2.0f * atanf(tanf(w * 0.5f) / f->aspect); }
static float gW(const struct cf_framing *f, float h) { return 2.0f * atanf(f->aspect * tanf(h * 0.5f)); }

static float gdH(const struct cf_framing *f, float w, float dw)
{
	float a = f->aspect, s = sinf(w * 0.5f), c = cosf(w * 0.5f);
	float den = fma32(c * (a * a), c, s * s);
	return (dw * a) / den;
}

/* ---- renderer validity model (virtual camera rotated to the rect centre) -------------------------------- */

static void quat_mat(double x, double y, double z, double r, float M[3][3])
{
	double c0[3] = { 1 - 2 * (y * y + z * z), 2 * (x * y + z * r), 2 * (x * z - y * r) };
	double c1[3] = { 2 * (x * y - z * r), 1 - 2 * (x * x + z * z), 2 * (y * z + x * r) };
	double c2[3] = { 2 * (x * z + y * r), 2 * (y * z - x * r), 1 - 2 * (x * x + y * y) };
	for (int i = 0; i < 3; i++) {
		M[i][0] = (float)c0[i];
		M[i][1] = (float)c1[i];
		M[i][2] = (float)c2[i];
	}
}

static void rot(rect R, float M[3][3])
{
	float Mx[3][3], My[3][3];
	float a = (float)midy(R) * -0.5f, b = (float)midx(R) * 0.5f;
	quat_mat(sinf(a), 0, 0, cosf(a), Mx);
	quat_mat(0, sinf(b), 0, cosf(b), My);
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			M[i][j] = My[i][0] * Mx[0][j] + My[i][1] * Mx[1][j] + My[i][2] * Mx[2][j];
}

/* the forward distortion polynomial, no clamp */
static void distort(const struct cf_framing *f, float *u, float *v)
{
	float dc0 = (float)f->cam.dcx, dc1 = (float)f->cam.dcy;
	float d0 = *u - dc0, d1 = *v - dc1;
	float rs = (float)f->pix2 * (d0 * d0 + d1 * d1);
	const float *c = f->cam.fwd;
	float p = fmaf(c[7], rs, c[6]);
	p = fmaf(p, rs, c[5]);
	p = fmaf(p, rs, c[4]);
	p = fmaf(p, rs, c[3]);
	p = fmaf(p, rs, c[2]);
	p = fmaf(p, rs, c[1]);
	p = fmaf(p, rs, c[0]);
	float m = 1.0f / fmaf(p, 0.01f, 1.0f);
	*u = fmaf(d0, m, dc0);
	*v = fmaf(d1, m, dc1);
}

static int valid(const struct cf_framing *f, float px, float py, rect R)
{
	const struct cf_camera *c = &f->cam;
	float M[3][3];
	rot(R, M);
	float tx = tanf((float)((double)px - midx(R)));
	float ty = tanf((float)((double)py - midy(R)));
	float v[3];
	for (int i = 0; i < 3; i++)
		v[i] = M[i][0] * tx + M[i][1] * ty + M[i][2];
	float fx = (float)c->fx, fy = (float)c->fy, cx = (float)c->cx, cy = (float)c->cy;
	float u = (fx * v[0] + cx * v[2]) / v[2];
	float w = (fy * v[1] + cy * v[2]) / v[2];
	distort(f, &u, &w);
	if (c->radius >= 1) {
		float d = sqrtf(sq32(u - (float)c->dcx) + sq32(w - (float)c->dcy));
		if (d > (float)c->radius)
			return 0;
	}
	float un = u / (float)c->width, wn = w / (float)c->height;
	return 0 <= un && un < 1 && 0 <= wn && wn < 1;
}

static void search_size(const struct cf_framing *f, double *w, double *h, float p0, float p1)
{
	float hi = 1.0f, lo = 0.5f, wf = (float)*w, hf = (float)*h;
	for (int k = 0; k < 10; k++) {
		float mid = (hi + lo) * 0.5f;
		float s = (float)((double)mid + 0.05);
		float sw = wf * s, sh = hf * s;
		if (valid(f, p0 * s, p1 * s, R4((double)-sw * 0.5, (double)-sh * 0.5, sw, sh)))
			lo = mid;
		else
			hi = mid;
	}
	*w *= lo;
	*h *= lo;
}

static void scale_size(const struct cf_framing *f, double *w, double *h)
{
	for (int i = -1; i <= 1; i += 2)
		for (int j = -1; j <= 1; j += 2) {
			float p0 = (float)(*w * i * 0.5), p1 = (float)(*h * j * 0.5);
			if (!valid(f, p0, p1, R4(*w * -0.5, *h * -0.5, *w, *h)))
				search_size(f, w, h, p0, p1);
		}
}

static void search_point(const struct cf_framing *f, float p[2], float e[2], rect R)
{
	float q0 = p[0], q1 = p[1];
	for (int k = 0; k < 10; k++) {
		float m0 = (p[0] + e[0]) * 0.5f, m1 = (p[1] + e[1]) * 0.5f;
		rect Rs = R4(R.x + (double)m0 - (double)q0, R.y + (double)m1 - (double)q1, R.w, R.h);
		if (valid(f, m0, m1, Rs)) {
			e[0] = m0;
			e[1] = m1;
		} else {
			p[0] = m0;
			p[1] = m1;
		}
	}
}

static rect move(const struct cf_framing *f, rect R)
{
	const double *b = f->b;
	float mx = (float)dmin((b[2] - R.w) * 0.5, R.w * 0.05);
	float my = (float)dmin((b[3] - R.h) * 0.5, R.h * 0.05);
	R = R4(R.x - mx, R.y - my, R.w + 2.0 * mx, R.h + 2.0 * my);
	for (int i = -10; i < 11; i++) {
		float fx = (float)i / 10.0f;
		double ax = (double)(fx + 1.0f) * 0.5;
		int step = (i == -10 || i == 10) ? 20 : 2;
		for (int j = -10; j < 11; j += step) {
			float fy = (float)j / 10.0f;
			float p[2] = { (float)fma(ax, R.w, R.x), (float)fma((double)(fy + 1.0f) * 0.5, R.h, R.y) };
			if (!valid(f, p[0], p[1], R)) {
				float off[2] = { (float)(R.w * (double)fx * 0.5), (float)(R.h * (double)fy * 0.5) };
				float e[2] = { off[0], off[1] };
				search_point(f, p, e, R);
				float c0 = e[0] - off[0], c1 = e[1] - off[1];
				R = R4((double)c0 - R.w * 0.5, (double)c1 - R.h * 0.5, R.w, R.h);
			}
		}
	}
	return R4(R.x + mx, R.y + my, R.w - 2.0 * mx, R.h - 2.0 * my);
}

/* _rotateRectCCW(R, 4): float corners turned a full turn (sin(pi) != 0 in float) */
static rect rotate_full(rect R)
{
	float half = (float)(4.0 * 1.5707963267948966) * 0.5f;
	float s = sinf(half), c = cosf(half);
	float cc = c * c, ss = s * s, sc2 = 2.0f * (s * c);
	float a = cc - ss;
	double cx[4] = { minx(R), maxx(R), minx(R), maxx(R) }, cy[4] = { miny(R), miny(R), maxy(R), maxy(R) };
	float x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
	for (int k = 0; k < 4; k++) {
		float x = (float)cx[k], y = (float)cy[k];
		float u = x * a - sc2 * y, v = sc2 * x + y * a;
		x0 = u < x0 ? u : x0;
		y0 = v < y0 ? v : y0;
		x1 = u > x1 ? u : x1;
		y1 = v > y1 ? v : y1;
	}
	return R4(x0, y0, x1 - x0, y1 - y0);
}

/* adjustDisplayRectToFitInFramingSpace */
static rect adjust(const struct cf_framing *f, rect R)
{
	if (isnull(R))
		return R;
	R = rotate_full(R);
	double w = R.w, h = R.h;
	scale_size(f, &w, &h);
	return move(f, R4(R.x + (R.w - w) * 0.5, R.y + (R.h - h) * 0.5, w, h));
}

/* ---- warp: normalised input rect -> framing space ----------------------------------------------------- */

static void undistort(const struct cf_framing *f, float *u, float *v)
{
	float dc0 = (float)f->cam.dcx, dc1 = (float)f->cam.dcy;
	float d0 = *u - dc0, d1 = *v - dc1;
	float r = sqrtf(d0 * d0 + d1 * d1), rmax = (float)f->rmax;
	if (r > rmax) {
		d0 = d0 / r * rmax;
		d1 = d1 / r * rmax;
	}
	/* rs = pixel size^2 * r^2 after the clamp, Horner from inv[7], then 1 / (1 + 0.01 P) */
	float rs = (float)f->pix2 * (d0 * d0 + d1 * d1);
	const float *c = f->cam.inv;
	float p = fmaf(c[7], rs, c[6]);
	p = fmaf(p, rs, c[5]);
	p = fmaf(p, rs, c[4]);
	p = fmaf(p, rs, c[3]);
	p = fmaf(p, rs, c[2]);
	p = fmaf(p, rs, c[1]);
	p = fmaf(p, rs, c[0]);
	float m = 1.0f / fmaf(p, 0.01f, 1.0f);
	*u = fmaf(d0, m, dc0);
	*v = fmaf(d1, m, dc1);
}

static rect warp(const struct cf_framing *f, rect r)
{
	const struct cf_camera *c = &f->cam;
	float W = (float)c->width, H = (float)c->height, cx = (float)c->cx, cy = (float)c->cy;
	float fx = (float)c->fx, fy = (float)c->fy;
	float tu = (float)midx(r) * W, tv = (float)miny(r) * H;
	float bu = (float)midx(r) * W, bv = (float)maxy(r) * H;
	float lu = (float)minx(r) * W, lv = (float)midy(r) * H;
	float ru = (float)maxx(r) * W, rv = (float)midy(r) * H;
	undistort(f, &tu, &tv);
	undistort(f, &bu, &bv);
	undistort(f, &lu, &lv);
	undistort(f, &ru, &rv);
	float x0 = atanf((lu - cx) / fx), y1 = atanf((bv - cy) / fy);
	float y0 = atanf((tv - cy) / fy), x1 = atanf((ru - cx) / fx);
	return R4(x0, y0, x1 - x0, y1 - y0);
}

/* ---- scene framing engine ----------------------------------------------------------------------------- */

static float default_screen_w(const struct cf_framing *f)
{
	float v = f->o.default_fov, w = gW(f, v);
	return v > w ? v : w;
}

static float min_screen_w(const struct cf_framing *f)
{
	float v = f->o.min_fov, w = gW(f, v);
	return v > w ? v : w;
}

static float min_db_w(const struct cf_framing *f) { return min_screen_w(f) * f->fp->w; }
static float vw_from_dbw(const struct cf_framing *f, float w) { return w / f->fp->w; }
static float dbw_from_vw(const struct cf_framing *f, float w) { return w * f->fp->w; }
static float dbh_from_vh(const struct cf_framing *f, float h) { return h * f->fp->h; }
static float dbh_for_dbw(const struct cf_framing *f, float w) { return gH(f, w / f->fp->w) * f->fp->h; }

static rect db_from_viewport(const struct cf_framing *f, rect V)
{
	if (isnull(V))
		return RNULL;
	float dbw = dbw_from_vw(f, (float)V.w), dbh = dbh_from_vh(f, (float)V.h);
	float top = (float)(V.y + V.h * (double)f->fp->top);
	return R4(midx(V) + (double)dbw * -0.5, top, dbw, dbh);
}

static rect viewport_from_db(const struct cf_framing *f, rect B)
{
	if (isnull(B))
		return RNULL;
	float vw = vw_from_dbw(f, (float)B.w), vh = gH(f, vw);
	float x = (float)((double)vw * -0.5 + midx(B));
	float y = (float)(B.y - (double)(vh * f->fp->top));
	if (vw > f->maxw * 0.95f) {
		float w, h;
		if (vw > vh) {
			w = f->maxw;
			h = gH(f, f->maxw);
		} else {
			w = gW(f, f->maxw);
			h = f->maxw;
		}
		return R4(w * -0.5f, h * -0.5f, w, h);
	}
	return R4(x, y, vw, vh);
}

static rect target(const struct cf_framing *f) { return adjust(f, viewport_from_db(f, f->deadband)); }

static rect centred(const struct cf_framing *f, rect R, float w)
{
	const double *b = f->b;
	float h = gH(f, w);
	float x = (float)dmin(b[0] + b[2] - w, dmax(b[0], midx(R) + (double)w * -0.5));
	float y = (float)dmin(b[1] + b[3] - h, dmax(b[1], miny(R)));
	return R4(x, y, w, h);
}

static void range1(float smin, float smax, float lo, float hi, float out[2])
{
	if (lo <= smax) {
		if (hi >= smin) {
			out[0] = smin > lo ? smin : lo;
			out[1] = smax < hi ? smax : hi;
		} else {
			out[0] = out[1] = hi;
		}
	} else {
		out[0] = out[1] = lo;
	}
}

static rect cropped(const struct cf_framing *f, float vw, rect SE)
{
	const double *b = f->b;
	float m = (float)((1.0 - f->fp->w) * 0.5);
	float rx[2], ry[2];
	float f0 = (float)b[0], f1 = (float)(b[0] + b[2]);
	range1((float)minx(SE), (float)maxx(SE), fma32(vw, m, f0), (float)((double)f1 - (double)vw * m), rx);
	float top = f->fp->top, vh = gH(f, vw);
	float bf = (float)((1.0 - f->fp->h) - top);
	f0 = (float)b[1];
	f1 = (float)(b[1] + b[3]);
	range1((float)miny(SE), (float)maxy(SE), fma32(vh, top, f0), (float)((double)f1 - (double)vh * bf), ry);
	return R4(rx[0], ry[0], rx[1] - rx[0], ry[1] - ry[0]);
}

static rect baseline(const struct cf_framing *f, struct cf_track **trs, int n)
{
	const double *b = f->b;
	float dsw = default_screen_w(f), dsh = gH(f, dsw);
	rect U = RNULL;
	for (int i = 0; i < n; i++) {
		rect bbI = safe_intersection(trs[i]->bb, b), fbI = safe_intersection(trs[i]->fb, b);
		float wv = (float)dmin(fbI.w, dsw), hv = (float)dmin(bbI.h, dsh);
		U = runion(U, R4(midx(fbI) + (double)wv * -0.5, dmin(miny(fbI), miny(bbI)), wv, hv));
	}
	rect UI = safe_intersection(U, b);
	float w = (float)dmax(UI.w, gW(f, (float)UI.h));
	w = (float)dmin(f->maxw, dmax(min_screen_w(f), w));
	float h = gH(f, w);
	float x = (float)dmin(b[0] + b[2] - w, dmax(b[0], midx(UI) + (double)w * -0.5));
	float y = (float)dmin(b[1] + b[3] - h, miny(UI));
	return R4(x, y, w, h);
}

static rect subject_rect(const struct cf_framing *f, struct cf_track **trs, int n, float bw)
{
	const struct cf_params *fp = f->fp;
	float vh = gH(f, bw), dbh = dbh_from_vh(f, vh), mdbw = min_db_w(f);
	double half_mdbw = mdbw * 0.5f, half_dbh = dbh * 0.5f;
	rect U = RNULL;
	for (int i = 0; i < n; i++) {
		rect fb = trs[i]->fb;
		float pad = (float)dmax(dmin(fb.h * 0.5, fb.h * -0.6 + dbh), 0.0);
		rect R0 = R4(fb.x, fb.y - pad, fb.w, fb.h + pad);
		float a = (float)dmax((((1.0 - fp->top) - fp->h) + (double)-0.1f) * vh, 0.0);
		float s8 = (float)(R0.h - a);
		float hh = (float)(dmax(s8, dmin(half_dbh, R0.h)) * fp->hpad);
		float ww = (float)dmax(half_mdbw, R0.w * fp->wpad);
		rect R1 = R4(midx(R0) - (double)ww * 0.5, R0.y, ww, hh);
		float mx = (float)midx(R1);
		float den = (float)((double)fabsf(mx) * (double)fabsf(mx) + 1.0);
		float y0 = (float)miny(R1) / den, y1 = (float)maxy(R1) / den;
		U = runion(U, R4(R1.x, y0, R1.w, y1 - y0));
	}
	return safe_intersection(U, f->b);
}

static void fs_rng(float S, float a, float b, float smin, float smax, float f0, float f1, float out[2])
{
	float lo = fma32(S, a, f0);
	if (smax < lo) {
		out[0] = out[1] = lo;
		return;
	}
	float hi = (float)((double)f1 - (double)S * b);
	if (hi < smin) {
		out[0] = out[1] = hi;
		return;
	}
	out[0] = smin > lo ? smin : lo;
	out[1] = smax < hi ? smax : hi;
}

/* _ComputeSizeToFitSubjectsInDeadband (one axis) */
static float fit_size(float S0, float smin, float smax, float a, float b, float f0, float f1)
{
	float r[2];
	fs_rng(S0, a, b, smin, smax, f0, f1, r);
	float e = r[1] - r[0];
	double d7 = (1.0 - a) - b;
	if ((double)S0 * d7 + 0.001 > e)
		return S0;
	float S1 = (float)(e / d7);
	fs_rng(S1, a, b, smin, smax, f0, f1, r);
	if (fabs((double)((r[1] - r[0]) - e)) < 0.001)
		return S1;
	float c0 = (smin - f0) / a, c1 = (f1 - smax) / b;
	float Sx = c0 > c1 ? c0 : c1;
	fs_rng(Sx, a, b, smin, smax, f0, f1, r);
	double ext = r[1] - r[0];
	float s0p, s5p, s0, s5;
	if (c0 < c1) {
		s0p = c0;
		s5p = a;
	} else {
		s0p = c1;
		s5p = b;
	}
	if ((double)Sx * d7 + 0.001 > ext) {
		s0 = s0p;
		s5 = s5p;
	} else {
		s0 = Sx;
		s5 = a + b;
	}
	fs_rng(s0, a, b, smin, smax, f0, f1, r);
	double d1 = (double)(r[1] - r[0]) - (double)s0 * d7;
	return s0 + (float)(d1 / (d7 + s5));
}

static float width_to_fit(const struct cf_framing *f, rect SE)
{
	const double *b = f->b;
	float msw = min_screen_w(f);
	float m = (float)((1.0 - f->fp->w) * 0.5);
	float xs = fit_size(msw, (float)minx(SE), (float)maxx(SE), m, m, (float)b[0], (float)(b[0] + b[2]));
	float top = f->fp->top, bot = (float)((1.0 - f->fp->h) - top);
	float ys = fit_size(gH(f, msw), (float)miny(SE), (float)maxy(SE), top, bot, (float)b[1],
			    (float)(b[1] + b[3]));
	float wy = gW(f, ys);
	float w = xs > wy ? xs : wy;
	return f->maxw < w ? f->maxw : w;
}

static rect summary(const struct cf_framing *f, struct cf_track **trs, int n, rect *SEo)
{
	rect base = baseline(f, trs, n);
	rect SE = subject_rect(f, trs, n, (float)base.w);
	float fitw = width_to_fit(f, SE);
	float w = (float)dmax(base.w, fitw), vh = gH(f, w);
	rect C = cropped(f, w, SE);
	double dbh = dbh_for_dbw(f, dbw_from_vw(f, w));
	float pad = (float)dmax(dmin(dbh * 0.125, (dbh - C.h) * 0.5), 0.0);
	float y1 = (float)(C.y - pad);
	float vy = (float)((double)y1 - (double)vh * f->fp->top);
	*SEo = SE;
	return db_from_viewport(f, centred(f, R4(base.x, vy, base.w, base.h), w));
}

static float db_step(float m0, float m1, float lo, float hi)
{
	if ((double)lo + 0.001 > m0) {
		if ((double)m1 + 0.001 > hi)
			return m0;
		float v = hi - (m1 - m0);
		return v < lo ? v : lo;
	}
	if ((double)m1 + 0.001 > hi) {
		float v = hi - (m1 - m0);
		return v > lo ? v : lo;
	}
	return m0;
}

/* _DetermineNewDeadbandMinComponent */
static float new_min(float s0, float s1, float o0, float o1, float a, float b, float f0, float f1, float n)
{
	double c = (double)(s0 + s1) * 0.5, h = (double)n * 0.5;
	float m0 = (float)(c - h), m1 = (float)(c + h);
	m0 = db_step(m0, m1, o0, o1);
	m1 = n + m0;
	m0 = db_step(m0, m1, s0, s1);
	float lo = fma32(n, a, f0), hi = (float)((double)f1 - (double)n * b);
	m1 = n + m0;
	return db_step(m0, m1, lo, hi);
}

static rect place_deadband(const struct cf_framing *f, rect SE, rect old, float dbw)
{
	const struct cf_params *fp = f->fp;
	const double *b = f->b;
	rect C = cropped(f, vw_from_dbw(f, dbw), SE);
	float ax = (float)(((1.0 - fp->w) * 0.5) / fp->w);
	float x0 = new_min((float)minx(C), (float)maxx(C), (float)minx(old), (float)maxx(old), ax, ax,
			   (float)b[0], (float)(b[0] + b[2]), dbw);
	float ay = fp->top / fp->h;
	float by = (float)((1.0 - fp->h) / fp->h - ay);
	float dbh = dbh_for_dbw(f, dbw);
	float y0 = new_min((float)miny(C), (float)maxy(C), (float)miny(old), (float)maxy(old), ay, by,
			   (float)b[1], (float)(b[1] + b[3]), dbh);
	return R4(x0, y0, dbw, dbh);
}

static int framing_ideal(const struct cf_framing *f, rect R, float tol)
{
	rect db = f->deadband;
	float dx = fabsf((float)(midx(db) - midx(R)));
	if ((double)dx / db.w >= tol)
		return 0;
	float dys = (float)(midy(db) - midy(R));
	float fac = dys > 0 ? 0.3f : 1.0f;
	return (double)fabsf(dys) / (db.h * fac) < tol;
}

static void clear_history(struct cf_framing *f)
{
	memset(f->hist, 0, sizeof(f->hist));
	f->hcount = f->hhead = 0;
	f->hupdate_t = NONE;
	f->stationary = 0;
	f->stationary_t = NONE;
}

static int is_stationary(const struct cf_framing *f, rect SE)
{
	if (f->hcount != 10)
		return 0;
	float c[2] = { 0, 0 }, s[2] = { 0, 0 };
	for (int i = 0; i < 10; i++) {
		c[0] += (float)midx(f->hist[i]);
		c[1] += (float)midy(f->hist[i]);
		s[0] += (float)f->hist[i].w;
		s[1] += (float)f->hist[i].h;
	}
	float tol = f->o.move_tol + 1.0f, sz[2], o[2];
	for (int i = 0; i < 2; i++) {
		c[i] = c[i] / 10.0f;
		s[i] = s[i] / 10.0f;
		sz[i] = s[i] * tol;
		o[i] = fma32(-0.5f, sz[i], c[i]);
	}
	return contains_rect(R4(o[0], o[1], sz[0], sz[1]), SE);
}

static void update_movement(struct cf_framing *f, rect SE, int64_t t)
{
	if (f->hcount == 0) {
		f->hist[0] = SE;
		f->hcount = f->hhead = 1;
		f->hupdate_t = t;
		return;
	}
	if ((float)sec(t, f->hupdate_t) < 0.1f)
		return;
	f->hist[f->hhead] = SE;
	f->hcount = (f->hcount < 9 ? f->hcount : 9) + 1;
	f->hhead = (f->hhead + 1) % 10;
	f->hupdate_t = t;
	int st = is_stationary(f, SE);
	if (st != f->stationary) {
		f->stationary = st;
		f->stationary_t = st ? t : NONE;
	}
}

static void transition(struct cf_framing *f, int nw, int64_t t)
{
	if (nw == f->state)
		return;
	if (nw == 2) {
		f->rec_start_db = f->deadband;
		f->rec_start_t = t;
		f->rec_end_t = t + us(f->o.rec_dur);
	} else if (nw == 1) {
		f->drift_start_db = f->deadband;
		f->drift_start_t = t;
		f->drift_end_t = t + us(0.1f);
	} else {
		clear_history(f);
	}
	f->state = nw;
}

static void update_with_subject(struct cf_framing *f, rect SE, int64_t t)
{
	const struct cf_opts *o = &f->o;
	int st = f->state, nw = st;
	rect Dd = f->deadband, S = f->slack;
	double ix = dmin(maxx(Dd), maxx(S)) - dmax(minx(Dd), minx(S));
	double iy = dmin(maxy(Dd), maxy(S)) - dmax(miny(Dd), miny(S));
	double ux = dmax(maxx(Dd), maxx(S)) - dmin(minx(Dd), minx(S));
	double uy = dmax(maxy(Dd), maxy(S)) - dmin(miny(Dd), miny(S));
	double r = (ix * iy) / (ux * uy);
	if (r > (double)0.9f) {
		if (st == 2) {
			float tol = is_stationary(f, SE) ? 0.01f : 0.1f;
			if (framing_ideal(f, f->ideal, tol) || t >= f->rec_end_t) {
				nw = 0;
			} else {
				double s = sec(t, f->rec_start_t) / sec(f->rec_end_t, f->rec_start_t);
				s = s < 0 ? 0 : s > 1 ? 1 : s;
				float e = (float)(s * s * (s * -2.0 + 3.0));
				rect A = f->rec_start_db, B = f->ideal;
				float c0[2] = { (float)midx(A), (float)midy(A) }, c1[2] = { (float)midx(B), (float)midy(B) };
				float s0[2] = { (float)A.w, (float)A.h }, s1[2] = { (float)B.w, (float)B.h };
				float org[2], sz[2];
				for (int i = 0; i < 2; i++) {
					float c = fma32(c1[i] - c0[i], e, c0[i]);
					sz[i] = fma32(s1[i] - s0[i], e, s0[i]);
					org[i] = fma32(-0.5f, sz[i], c);
				}
				f->deadband = R4(org[0], org[1], sz[0], sz[1]);
			}
		} else if (st == 1) {
			if (t > f->drift_end_t) {
				float el = (float)sec(t, f->drift_start_t);
				int w21 = o->drift_dur_thr < el;
				rect a = f->deadband, b = f->drift_start_db;
				float dx = (float)midx(a) - (float)midx(b), dy = (float)midy(a) - (float)midy(b);
				float dist = sqrtf(dx * dx + dy * dy);
				if (!(dist <= o->drift_off_thr))
					w21 = 1;
				nw = (o->drift_enabled && w21) ? 2 : 0;
			}
		} else {
			update_movement(f, SE, t);
			int ideal = framing_ideal(f, f->ideal, 0.2f);
			if (f->stationary && !ideal) {
				int w21 = f->stationary_t != NONE ? o->rec_delay < (float)sec(t, f->stationary_t) : 1;
				if (f->rec_end_t != NONE && !(o->rec_idle < (float)sec(t, f->rec_end_t)))
					w21 = 0;
				if (w21)
					nw = 2;
			}
		}
	} else {
		if (st == 1) {
			f->deadband = f->slack;
			f->drift_end_t = t + us(0.1f);
		}
		nw = 1;
	}
	transition(f, nw, t);
}

static void fit_scene(struct cf_framing *f, rect S, rect SE, int n, int64_t t)
{
	if (isnull(f->deadband) || !f->last_had_subjects) {
		f->deadband = f->slack = f->ideal = S;
		return;
	}
	const struct cf_params *fp = f->fp;
	float maxw = f->maxw, fitw = width_to_fit(f, SE), mdb = min_db_w(f);
	float v = mdb > fitw ? mdb : fitw;
	float s8 = dbw_from_vw(f, v < maxw ? v : maxw);
	float s9 = dbw_from_vw(f, maxw);
	double curw = f->deadband.w;
	float s15 = (float)dmin(dmax(curw * fp->minf, s8), s9);
	float s12 = (float)S.w, s10;
	if ((double)s15 - 0.001 > s12) {
		s10 = s12 / fp->minf;
	} else {
		double d3 = (float)dmin(dmax(curw * fp->maxf, s8), s9);
		s10 = (double)s12 - 0.001 > d3 ? s12 / fp->maxf : (float)curw;
	}
	float s0 = n < 2 ? s12 : s10;
	s8 = (float)dmin(dmax(s0, s8), s9);
	double d13 = fabs((double)s8 - curw) / curw;
	if (d13 > (double)0.1f)
		s8 = s12;
	rect V = viewport_from_db(f, S);
	f->ideal = db_from_viewport(f, centred(f, V, vw_from_dbw(f, s8)));
	f->slack = place_deadband(f, SE, f->deadband, s8);
	if (d13 > (double)0.1f)
		f->deadband = f->ideal;
	else
		update_with_subject(f, SE, t);
}

/* the home view's width: view.default of the framing space (of the whole sensor for a cropped input) */
static float home_w(const struct cf_framing *f)
{
	float w = (float)((f->cam.sensor_w > 0 ? f->cam.sensor_w : f->b[2]) * f->o.default_fov);
	return f->cam.sensor_w > 0 && w > f->maxw ? f->maxw : w;
}

static rect default_viewport(const struct cf_framing *f)
{
	const double *b = f->b;
	float w = home_w(f), h = gH(f, w);
	double x = (b[0] + b[2] * 0.5) - (double)(w * 0.5f);
	double y = (b[1] + b[3] * 0.5) - (double)(h * 0.5f);
	return adjust(f, R4(x, y, w, h));
}

static rect initial_viewport(const struct cf_framing *f)
{
	float w = home_w(f), h = gH(f, w);
	return adjust(f, R4(w * -0.5f, h * -0.5f, w, h));
}

static void restore_default(struct cf_framing *f, int64_t t)
{
	if (!isnull(f->deadband)) {
		float el = f->last_detect_t != NONE ? (float)sec(t, f->last_detect_t) : 0.0f;
		if (el <= f->o.home_delay)
			return;
	}
	if (f->home_start_t == NONE) {
		rect Dv = default_viewport(f);
		f->home_start_t = t;
		float from[4] = { (float)f->cur.x, (float)f->cur.y, (float)f->cur.w, (float)f->cur.h };
		float to[4] = { (float)Dv.x, (float)Dv.y, (float)Dv.w, (float)Dv.h };
		memcpy(f->home_from, from, sizeof(from));
		memcpy(f->home_to, to, sizeof(to));
	}
	double u = sec(t, f->home_start_t) / f->o.home_dur;
	float uf = (float)(u < 0 ? 0 : u > 1 ? 1 : u), R[4];
	for (int i = 0; i < 4; i++)
		R[i] = fma32(f->home_to[i] - f->home_from[i], uf, f->home_from[i]);
	f->deadband = db_from_viewport(f, R4(R[0], R[1], R[2], R[3]));
	f->ideal = f->slack = f->deadband;
}

static void set_params(struct cf_framing *f, const struct cf_params *fp)
{
	f->fp = fp;
	rect T = target(f);
	if (!isnull(T))
		f->deadband = db_from_viewport(f, T);
}

static void engine_update(struct cf_framing *f, struct cf_track **trs, int n, int64_t t)
{
	if (!n) {
		restore_default(f, t);
	} else {
		rect SE, S = summary(f, trs, n, &SE);
		fit_scene(f, S, SE, n, t);
		f->home_start_t = NONE;
		f->last_detect_t = t;
	}
	f->last_had_subjects = n > 0;
}

/* ---- rectangle animator ------------------------------------------------------------------------------- */

static float clampf(float x, float lo, float hi)
{
	x = x > lo ? x : lo;
	return x < hi ? x : hi;
}

static void combine(const float a[2], const float b[2], float out[2])
{
	out[0] = a[0] > b[0] ? a[0] : b[0];
	out[1] = a[1] < b[1] ? a[1] : b[1];
}

static void eng_animate(struct cf_engine1 *e, float dur, float target, int opts, double t)
{
	e->has_start = 1;
	e->dur = dur;
	e->target = target;
	e->opts = opts;
	if (!e->has_last) {
		e->has_last = 1;
		e->last = t;
	}
}

/* c: accel[2], vel[2], pos[2] */
static void eng_drive(struct cf_engine1 *e, double t, const float c[6], float mult)
{
	double dt = t - e->last;
	if (dt > 0.1)
		dt = 0.1;
	if (fabs(dt) < 1e-4)
		return;
	if (e->has_start) {
		float a = 0;
		if (e->opts & 6) {
			double rem = dmax((double)e->dur - dt, 0.0010000000474974513);
			a = (float)((double)(e->target - e->pos) / rem * (double)mult);
			a = (float)((double)(a - e->vel) / dt);
		}
		a = clampf(a, c[0], c[1]);
		float v = clampf((float)((double)a * dt + (double)e->vel), c[2], c[3]);
		float p0 = e->pos;
		float p = clampf((float)((double)v * dt + (double)p0), c[4], c[5]);
		e->pos = p;
		e->vel = (float)((double)(p - p0) / dt);
	}
	e->last = t;
}

static float tpivot(float a0, float a1, float b0, float b1, float dflt)
{
	float L1 = a1 - a0, L2 = b1 - b0;
	if (fabs((double)fabsf(L1 - L2)) < 0.001)
		return dflt;
	float Dd = L1 - L2, t = (b0 - a0) / Dd, o = a0 - b0;
	float err = (t < 1 && t > 0) ? sq32(fma32(Dd, t, o)) : 1e9f;
	float e0 = sq32(fma32(Dd, 0.0f, o));
	if (e0 < err) {
		t = 0;
		err = e0;
	}
	float e1 = sq32(o + Dd);
	if (e1 < err)
		t = 1;
	return t;
}

/* ease-in acceleration limits; size > 0: positional (scaled), else zoom (log space, times accm) */
static void ease(float v, float d, float size, float A, float B, float accm, float out[2])
{
	if (size > 0) {
		v = v / size;
		d = d / size;
	}
	float sq = fabsf(sqrtf(fabsf(v)));
	float lo = d < 0 ? fma32(d * sq, A, -B) : -1e8f;
	float hi = d > 0 ? fma32(d * sq, A, B) : 1e8f;
	if (size > 0) {
		out[0] = size * lo;
		out[1] = size * hi;
	} else {
		out[0] = lo * accm;
		out[1] = hi * accm;
	}
}

static void edge(rect r, const double *b, float scale, float accm, int axis, float acc[2], float vel[2])
{
	double mn, mx, bmn, bmx;
	if (axis == 0) {
		mn = r.x;
		mx = r.x + r.w;
		bmn = b[0];
		bmx = b[0] + b[2];
	} else {
		mn = r.y;
		mx = r.y + r.h;
		bmn = b[1];
		bmx = b[1] + b[3];
	}
	float dl = mn < bmn ? 0.0f : ((float)(mn - bmn) * scale) * 2.0f;
	float dr = bmx < mx ? 0.0f : ((float)(bmx - mx) * scale) * 2.0f;
	float a = (float)(r.w * 0.6 * (double)accm);
	float q0 = dl * a, q1 = dr * a;
	float o0 = sqrtf(q0 < 0 ? 0 : q0), o1 = sqrtf(q1 < 0 ? 0 : q1);
	acc[0] = -a;
	acc[1] = a;
	vel[0] = o0 > 1e8f ? -1e8f : -o0;
	vel[1] = o1 > 1e8f ? 1e8f : o1;
}

static void p2s(struct cf_framing *f, float px, float py, float w, float vpx, float vpy, float vw, float tx,
		float ty)
{
	float h = gH(f, w), dh = gdH(f, w, vw);
	f->s[0] = fma32(-tx, w, px);
	f->s[1] = fma32(-ty, h, py);
	f->s[2] = w;
	f->s[3] = fma32(-tx, vw, vpx);
	f->s[4] = fma32(-ty, dh, vpy);
	f->s[5] = vw;
}

static void anim_width(struct cf_framing *f, const float T[3], float tx, float ty, double t)
{
	const struct cf_opts *p = &f->o;
	float x0 = f->s[0], y0 = f->s[1], w0 = f->s[2], vx0 = f->s[3], vy0 = f->s[4], vw0 = f->s[5];
	float h0 = gH(f, w0), dh0 = gdH(f, w0, vw0);
	float lt = logf(T[2]);
	eng_animate(&f->Z, 1.75f, lt, 4, t);
	float acc = (float)(0.425 * (double)p->zacc);
	float c[6], e[2], lim[2] = { -acc, acc };
	ease(f->Z.vel, lt - f->Z.pos, 0, p->zA, p->zB, p->zacc, e);
	combine(lim, e, c);
	c[2] = (float)((double)p->zin * -0.3);
	c[3] = (float)((double)p->zout * 0.3);
	c[4] = logf(p->min_fov);
	c[5] = logf(f->maxw);
	float mult = f->Z.vel < 0 ? p->zin : p->zout;
	float vpx = fma32(vw0, tx, vx0), vpy = fma32(dh0, ty, vy0);
	float px = fma32(w0, tx, x0), py = fma32(h0, ty, y0);
	eng_drive(&f->Z, t, c, mult);
	float w1 = expf(f->Z.pos);
	p2s(f, px, py, w1, vpx, vpy, w1 * f->Z.vel, tx, ty);
}

static void anim_pivots(struct cf_framing *f, const float T[3], float tx, float ty, double t)
{
	const struct cf_opts *p = &f->o;
	const double *b = f->b;
	float x = f->s[0], y = f->s[1], w = f->s[2], vx = f->s[3], vy = f->s[4], vw = f->s[5];
	float h = gH(f, w);
	float px = fma32(w, tx, x), py = fma32(h, ty, y);
	float vpx = fma32(vw, tx, vx), vpy = fma32(gdH(f, w, vw), ty, vy);
	float tpx = fma32(T[2], tx, T[0]), tpy = fma32(gH(f, T[2]), ty, T[1]);
	rect r = R4(x, y, w, h);
	float acc[2], vel[2], e[2], c[6], lim[2];

	struct cf_engine1 *E = &f->X;
	E->pos = px;
	E->vel = vpx;
	eng_animate(E, 0.75f, tpx, 4, t);
	edge(r, b, 100.0f, p->hacc, 0, acc, vel);
	float vh = (float)((double)w / 3.0 * (double)p->hspeed);
	lim[0] = -vh;
	lim[1] = vh;
	combine(vel, lim, c + 2);
	ease(E->vel, tpx - px, w, p->xA, p->xB, 0, e);
	combine(acc, e, c);
	c[4] = (float)(b[0] + (double)(tx * w));
	c[5] = (float)(b[0] + b[2] - (double)((1.0f - tx) * w));
	eng_drive(E, t, c, w * p->hspeed);

	E = &f->Y;
	E->pos = py;
	E->vel = vpy;
	eng_animate(E, 0.75f, tpy, 4, t);
	edge(r, b, 10000.0f, p->vacc, 1, acc, vel);
	float vv = (float)((double)h / 3.0 * (double)p->vspeed);
	lim[0] = -vv;
	lim[1] = vv;
	combine(vel, lim, c + 2);
	ease(E->vel, tpy - py, w, p->yA, p->yB, 0, e);
	combine(acc, e, c);
	c[4] = (float)(b[1] + (double)(ty * h));
	c[5] = (float)(b[1] + b[3] - (double)((1.0f - ty) * h));
	eng_drive(E, t, c, w * p->vspeed);

	p2s(f, f->X.pos, f->Y.pos, w, f->X.vel, f->Y.vel, vw, tx, ty);
}

static rect anim_rect(const struct cf_framing *f)
{
	return R4(f->s[0], f->s[1], f->s[2], gH(f, f->s[2]));
}

static void anim_reset(struct cf_framing *f, rect r)
{
	memset(f->s, 0, sizeof(f->s));
	f->s[0] = (float)r.x;
	f->s[1] = (float)r.y;
	f->s[2] = (float)r.w;
	memset(&f->X, 0, sizeof(f->X));
	memset(&f->Y, 0, sizeof(f->Y));
	memset(&f->Z, 0, sizeof(f->Z));
	f->Z.pos = logf(f->s[2]);
}

static rect anim_drive(struct cf_framing *f, rect tgt, double t)
{
	float T[3] = { (float)tgt.x, (float)tgt.y, (float)tgt.w };
	float x = f->s[0], y = f->s[1], w = f->s[2];
	float tx = tpivot(x, x + w, T[0], T[0] + T[2], 0.5f);
	float ty = tpivot(y, y + gH(f, w), T[1], T[1] + gH(f, T[2]), 0.0f);
	anim_width(f, T, tx, ty, t);
	anim_pivots(f, T, tx, ty, t);
	return anim_rect(f);
}

/* ---- face Kalman filter ------------------------------------------------------------------------------- */

static float kf1_add(struct cf_kf1 *k, float z)
{
	const float a = 0.65f;
	if (!k->init) {
		k->init = 1;
		memset(k->P, 0, sizeof(k->P));
		k->x[0] = z;
		k->x[1] = k->x[2] = 0;
		k->P[0][0] = 10;
		k->P[1][1] = k->P[2][2] = 1;
		k->prev = z;
		k->pv = k->mean = k->msq = 0;
	}
	float d = z - k->prev, dd = d - k->pv;
	k->mean = dd * (1 - a) + k->mean * a;
	k->msq = dd * dd * (1 - a) + k->msq * a;
	float q = k->msq - k->mean * k->mean;
	/* x = F x, P = F P F^T + qI with F = [[1,1,0],[0,1,1],[0,0,1]] */
	k->x[0] = k->x[0] + k->x[1];
	k->x[1] = k->x[1] + k->x[2];
	float A[3][3], P[3][3];
	for (int j = 0; j < 3; j++) {
		A[0][j] = k->P[0][j] + k->P[1][j];
		A[1][j] = k->P[1][j] + k->P[2][j];
		A[2][j] = k->P[2][j];
	}
	for (int i = 0; i < 3; i++) {
		P[i][0] = A[i][0] + A[i][1];
		P[i][1] = A[i][1] + A[i][2];
		P[i][2] = A[i][2];
		P[i][i] += q;
	}
	float S = P[0][0] + k->R, K[3], y = 0;
	for (int i = 0; i < 3; i++)
		K[i] = P[i][0] / S;
	y = z - k->x[0];
	for (int i = 0; i < 3; i++)
		k->x[i] = k->x[i] + K[i] * y;
	float M0[3] = { 1 - K[0], -K[1], -K[2] };
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			k->P[i][j] = M0[i] * P[0][j] + (i == 1 ? P[1][j] : 0.0f) + (i == 2 ? P[2][j] : 0.0f);
	k->prev = z;
	k->pv = d;
	return k->x[0];
}

static rect kf_add(struct cf_kf *k, rect r)
{
	float cx = kf1_add(&k->f[0], (float)(r.x + r.w / 2)), cy = kf1_add(&k->f[1], (float)(r.y + r.h / 2));
	float ew = kf1_add(&k->f[2], (float)r.w), eh = kf1_add(&k->f[3], (float)r.h);
	return R4((double)cx - (double)ew / 2, (double)cy - (double)eh / 2, ew, eh);
}

static struct cf_kf *kf_get(struct cf_framing *f, int oid)
{
	struct cf_kf *fr = NULL;
	for (int i = 0; i < NKF; i++) {
		if (f->kf[i].used && f->kf[i].oid == oid)
			return &f->kf[i];
		if (!f->kf[i].used && !fr)
			fr = &f->kf[i];
	}
	if (fr) {
		memset(fr, 0, sizeof(*fr));
		fr->used = 1;
		fr->oid = oid;
		fr->f[0].R = fr->f[1].R = 0.05f;
		fr->f[2].R = fr->f[3].R = 0.45f;
	}
	return fr;
}

/* ---- tracker (bodies = kind 0, the framing tracks; faces = kind 1) ---------------------------------------- */

static double inter_area(rect a, rect b)
{
	double x0 = dmax(a.x, b.x), y0 = dmax(a.y, b.y);
	double x1 = dmin(maxx(a), maxx(b)), y1 = dmin(maxy(a), maxy(b));
	if (x1 < x0 || y1 < y0)
		return 0;
	return (x1 - x0) * (y1 - y0);
}

static struct cf_track *trk_by_oid(struct cf_framing *f, long oid)
{
	for (int k = 0; k < 2; k++)
		for (int i = 0; i < f->nlist[k]; i++)
			if (f->tr[f->list[k][i]].oid == oid)
				return &f->tr[f->list[k][i]];
	return NULL;
}

static int trk_new(struct cf_framing *f, int kind, long id, long oid, rect obs, int64_t T)
{
	for (int s = 0; s < CF_MAX_TRACKS; s++) {
		struct cf_track *t = &f->tr[s];
		if (t->used)
			continue;
		memset(t, 0, sizeof(*t));
		t->used = 1;
		t->kind = kind;
		t->id = id;
		t->oid = oid;
		t->obs = obs;
		t->last_t = T;
		t->pair = -1;
		f->list[kind][f->nlist[kind]++] = s;
		return s;
	}
	return -1;
}

static void trk_unpair(struct cf_framing *f, struct cf_track *t)
{
	if (t->pair >= 0) {
		f->tr[t->pair].pair = -1;
		t->pair = -1;
	}
}

/* removeTrackOfType:atIndex: swaps the last track into the hole */
static void trk_remove(struct cf_framing *f, int kind, int i)
{
	struct cf_track *t = &f->tr[f->list[kind][i]];
	trk_unpair(f, t);
	t->used = 0;
	f->list[kind][i] = f->list[kind][--f->nlist[kind]];
}

struct det {
	long oid;
	rect r;
};

/* processDetections:ofType: known ids first, then a new id takes the nearest track not seen this frame */
static void process_detections(struct cf_framing *f, const struct det *d, int n, int kind, int64_t T)
{
	for (int i = 0; i < n; i++) {
		struct cf_track *t = trk_by_oid(f, d[i].oid);
		if (t) {
			t->obs = d[i].r;
			t->last_t = T;
		}
	}
	for (int i = 0; i < n; i++) {
		if (trk_by_oid(f, d[i].oid))
			continue;
		double best = INFINITY;
		int pick = -1;
		for (int j = 0; j < f->nlist[kind]; j++) {
			struct cf_track *t = &f->tr[f->list[kind][j]];
			if (t->last_t == T)
				continue;
			float dx = (float)(midx(t->obs) - midx(d[i].r));
			float dy = (float)(midy(t->obs) - midy(d[i].r));
			float dw = (float)(t->obs.w - d[i].r.w);
			double dist = fma32(dw, dw, fma32(dx, dx, dy * dy));
			if (best > dist) {
				best = dist;
				pick = j;
			}
		}
		if (pick >= 0) {
			struct cf_track *t = &f->tr[f->list[kind][pick]];
			t->oid = d[i].oid;
			t->obs = d[i].r;
			t->last_t = T;
		} else {
			f->counter++;
			trk_new(f, kind, f->counter, d[i].oid, d[i].r, T);
		}
	}
}

/* updateBodyFacePairsAtTime: */
static void update_pairs(struct cf_framing *f, int64_t T)
{
	for (int i = 0; i < f->nlist[0]; i++) {
		struct cf_track *b = &f->tr[f->list[0][i]];
		if (b->pair < 0 || b->id < 0)
			continue;
		rect P = f->tr[b->pair].obs;
		float in = (float)inter_area(b->obs, P);
		if ((float)(P.w * P.h) * 0.3f > in)
			trk_unpair(f, b);
	}
	for (int i = 0; i < f->nlist[1]; i++) {
		int fs = f->list[1][i];
		struct cf_track *fc = &f->tr[fs];
		if (fc->pair >= 0 && f->tr[fc->pair].id > 0)
			continue;
		float best = 0;
		int pick = -1;
		for (int j = 0; j < f->nlist[0]; j++) {
			struct cf_track *b = &f->tr[f->list[0][j]];
			if (b->pair >= 0 || b->id < 1)
				continue;
			float a = (float)inter_area(fc->obs, b->obs);
			if (best < a) {
				best = a;
				pick = f->list[0][j];
			}
		}
		float ratio = best / (float)(fc->obs.w * fc->obs.h);
		if (ratio > 0.3f && pick >= 0) {
			if (fc->pair >= 0)
				f->tr[fc->pair].pair = -1;
			fc->pair = pick;
			f->tr[pick].pair = fs;
		} else {
			/* inferSubjectBodyFromFaceLocation */
			float w = (float)(fc->obs.w + fc->obs.w), h = (float)(fc->obs.h * 3.0);
			rect o = R4(midx(fc->obs) + (double)w * -0.5, miny(fc->obs) + (double)h / -10.0, w, h);
			if (fc->pair >= 0) {
				f->tr[fc->pair].obs = o;
				f->tr[fc->pair].last_t = T;
			} else {
				f->counter++;
				int s = trk_new(f, 0, -f->counter, -fc->id, o, T);
				if (s >= 0) {
					fc = &f->tr[fs];
					fc->pair = s;
					f->tr[s].pair = fs;
				}
			}
		}
	}
}

/* faceBoundingBoxForFramingAtTime: the paired face relative to the body, remembered once seen */
static rect face_bb(struct cf_framing *f, struct cf_track *t)
{
	if (t->pair >= 0) {
		rect P = f->tr[t->pair].obs, B = t->obs;
		t->rel = R4((minx(P) - minx(B)) / B.w, (miny(P) - miny(B)) / B.h, P.w / B.w, P.h / B.h);
		t->has_rel = 1;
	}
	rect bb = t->bb;
	if (!t->has_rel) {
		double w = (float)(bb.w / 3.0);
		double h = bb.h > w ? w : (double)(float)bb.h;
		return R4(midx(bb) + w * -0.5, miny(bb), w, h);
	}
	return R4(fma(t->rel.x, bb.w, bb.x), fma(t->rel.y, bb.h, bb.y), t->rel.w * bb.w, t->rel.h * bb.h);
}

/* processFaceDetections:bodyDetections: */
static void track(struct cf_framing *f, const struct det *faces, int nf, const struct det *bodies, int nb,
		  int64_t T)
{
	process_detections(f, bodies, nb, 0, T);
	process_detections(f, faces, nf, 1, T);
	for (int k = 0; k < 2; k++)
		for (int i = 0; i < f->nlist[k];) {
			if (sec(T, f->tr[f->list[k][i]].last_t) > 1.0)
				trk_remove(f, k, i);
			else
				i++;
		}
	update_pairs(f, T);
	for (int i = 0; i < f->nlist[0];) {
		struct cf_track *t = &f->tr[f->list[0][i]];
		if (t->id < 0 && t->pair < 0)
			trk_remove(f, 0, i);
		else
			i++;
	}
	for (int i = 0; i < f->nlist[0]; i++)
		f->tr[f->list[0][i]].bb = f->tr[f->list[0][i]].obs;
	for (int i = 0; i < f->nlist[0]; i++) {
		struct cf_track *t = &f->tr[f->list[0][i]];
		t->fb = face_bb(f, t);
	}
}

/* ---- director ----------------------------------------------------------------------------------------- */

static int director_filter(struct cf_framing *f, struct cf_track **out)
{
	struct cf_track *srt[CF_MAX_TRACKS];
	float dist[CF_MAX_TRACKS];
	int n = f->nlist[0], m = 0;
	if (!n)
		return 0;
	float cx = (float)midx(f->cur), cy = (float)midy(f->cur);
	for (int i = 0; i < n; i++) {          /* stable insertion sort by distance to the viewport centre */
		struct cf_track *t = &f->tr[f->list[0][i]];
		float px = (float)maxx(t->fb), py = (float)maxy(t->fb);
		float d = sq32(cx - px) + sq32(cy - py);
		int j = i;
		while (j > 0 && dist[j - 1] > d) {
			dist[j] = dist[j - 1];
			srt[j] = srt[j - 1];
			j--;
		}
		dist[j] = d;
		srt[j] = t;
	}
	const double *b = f->b;
	rect U = RNULL;
	for (int i = 0; i < n; i++) {
		struct cf_track *t = srt[i];
		rect U2 = runion(U, t->fb);
		float lim = t->tracked ? 0.9f : 0.75f;
		if (U2.w > (double)(lim * f->maxw)) {
			t->tracked = 0;
			continue;
		}
		double mg = t->tracked ? (double)0.05f : (double)0.1f;
		float x0 = (float)(b[2] * mg + b[0]);
		float y0 = (float)dmin(b[1], miny(t->fb));
		double mb = t->tracked ? (double)0.02f : (double)0.05f;
		float y1 = (float)((b[1] + b[3] - b[3] * mb) - (double)y0);
		rect reg = R4(x0, y0, (mg * -2.0 + 1.0) * b[2], y1);
		if (!contains_point(reg, midx(t->fb), midy(t->fb))) {
			t->tracked = 0;
			continue;
		}
		float th = t->tracked ? f->o.tiny_min : f->o.tiny_max;
		if (t->fb.w < (double)th) {
			t->tracked = 0;
			continue;
		}
		t->tracked = 1;
		out[m++] = t;
		U = U2;
	}
	return m;
}

/* ---- public ------------------------------------------------------------------------------------------- */

int cf_tuning_read(struct cf_tuning *t, const char *path, char *err, size_t errlen)
{
#define P(name, field) { name ".deadband-width", &t->field.w }, { name ".deadband-height", &t->field.h }, \
	{ name ".top-offset", &t->field.top }, { name ".deadband-width-min", &t->field.minf }, \
	{ name ".deadband-width-max", &t->field.maxf }, { name ".padding-width", &t->field.wpad }, \
	{ name ".padding-height", &t->field.hpad }
	const struct apple_tuning_key k[] = {
		P("single", single), P("multi", multi),
		{ "speed.horizontal", &t->hspeed }, { "speed.vertical", &t->vspeed },
		{ "speed.zoom-in", &t->zin }, { "speed.zoom-out", &t->zout },
		{ "acceleration.horizontal", &t->hacc }, { "acceleration.vertical", &t->vacc },
		{ "acceleration.zoom", &t->zacc },
		{ "view.default", &t->default_fov }, { "view.minimum", &t->min_fov },
		{ "head.tiny-min", &t->tiny_min }, { "head.tiny-max", &t->tiny_max },
		{ "subject.movement-tolerance", &t->move_tol },
		{ "recenter.duration", &t->rec_dur }, { "recenter.idle", &t->rec_idle },
		{ "recenter.delay", &t->rec_delay },
		{ "home.delay", &t->home_delay }, { "home.duration", &t->home_dur },
		{ "drift.enabled", &t->drift_enabled }, { "drift.duration", &t->drift_dur },
		{ "drift.offset", &t->drift_off },
		{ "camera.focal-length", &t->focal_length },
	};
#undef P

	memset(t, 0, sizeof(*t));
	if (apple_tuning_read(path, k, sizeof(k) / sizeof(k[0]), err, errlen))
		return -1;
	if (!(t->default_fov > 0 && t->default_fov <= 1 && t->min_fov > 0 && t->min_fov <= t->default_fov &&
	      t->focal_length > 0 && t->single.w > 0 && t->single.h > 0 && t->multi.w > 0 && t->multi.h > 0)) {
		snprintf(err, errlen, "%s: values out of range", path);
		return -1;
	}
	return 0;
}

void cf_init(struct cf_framing *f, const struct cf_camera *cam, const struct cf_tuning *t)
{
	memset(f, 0, sizeof(*f));
	f->cam = *cam;
	f->single = t->single;
	f->multi = t->multi;
	f->o = (struct cf_opts){
		.hacc = t->hacc, .hspeed = t->hspeed, .vacc = t->vacc, .vspeed = t->vspeed,
		.min_fov = t->min_fov, .default_fov = t->default_fov,
		.xA = EASE_XA, .xB = EASE_XB, .yA = EASE_YA, .yB = EASE_YB, .zacc = t->zacc, .zA = EASE_ZA,
		.zB = EASE_ZB, .zin = t->zin, .zout = t->zout,
		.tiny_min = t->tiny_min, .tiny_max = t->tiny_max, .move_tol = t->move_tol,
		.rec_dur = t->rec_dur, .rec_idle = t->rec_idle, .rec_delay = t->rec_delay,
		.home_delay = t->home_delay, .home_dur = t->home_dur,
		/* the drift thresholds act as on / off: any non-zero value is 1 */
		.drift_dur_thr = t->drift_dur != 0 ? 1.0f : 0.0f, .drift_off_thr = t->drift_off != 0 ? 1.0f : 0.0f,
		.drift_enabled = t->drift_enabled != 0,
	};
	float fx = (float)cam->fx, fy = (float)cam->fy;
	float left = atan2f((float)cam->cx, fx), right = atan2f((float)(cam->width - cam->cx), fx);
	float top = atan2f((float)cam->cy, fy), bottom = atan2f((float)(cam->height - cam->cy), fy);
	f->b[0] = -(double)left;
	f->b[1] = -(double)top;
	f->b[2] = left + right;
	f->b[3] = top + bottom;
	f->aspect = (float)cam->out_w / (float)cam->out_h;
	if (f->b[3] + 1e-6 > (double)gH(f, (float)f->b[2]))
		f->maxw = (float)f->b[2];
	else
		f->maxw = gW(f, (float)f->b[3]);

	/* the undistortion's radial clamp */
	int iw = (int)(float)cam->width, ih = (int)(float)cam->height, R = cam->radius;
	float diag = sqrtf(sq32((float)iw * 0.5f) + sq32((float)ih * 0.5f));
	int w9 = diag > (float)R ? (int)(diag - (float)R) : 0;
	int w8 = (iw / 2 - R) > 0 ? iw / 2 - R : w9;
	float s4 = (float)w8, W = (float)cam->width, H = (float)cam->height;
	float dc0 = (float)cam->dcx, dc1 = (float)cam->dcy;
	float a = (W - dc0) - s4, bb = dc0 - s4;
	float c = H - dc1, d = dc1;
	float m1 = a > bb ? a : bb, m2 = c > d ? c : d;
	f->rmax = m1 > m2 ? m1 : m2;
	f->pix2 = (float)cam->pix * (float)cam->pix;
	cf_reset(f);
}

void cf_reset(struct cf_framing *f)
{
	memset(f->kf, 0, sizeof(f->kf));
	memset(f->tr, 0, sizeof(f->tr));
	f->nlist[0] = f->nlist[1] = 0;
	f->counter = 0;
	f->fp = &f->single;
	f->deadband = f->slack = f->ideal = RNULL;
	f->last_detect_t = NONE;
	f->last_had_subjects = 0;
	f->state = 0;
	clear_history(f);
	f->drift_start_db = f->rec_start_db = RNULL;
	f->drift_start_t = f->drift_end_t = f->rec_start_t = f->rec_end_t = f->home_start_t = NONE;
	anim_reset(f, initial_viewport(f));
	f->cur = anim_rect(f);
	f->target = RNULL;
}

static int observe(struct cf_framing *f, const struct cf_face *in, int n, int body, struct det *out)
{
	int m = 0;
	if (n > CF_MAX_FACES)
		n = CF_MAX_FACES;
	for (int i = 0; i < n; i++) {
		long oid = (long)in[i].id * 2 + body;
		struct cf_kf *k = kf_get(f, (int)oid);
		if (!k)
			continue;
		k->seen = 1;
		out[m].oid = oid;
		out[m].r = warp(f, kf_add(k, R4(in[i].x, in[i].y, in[i].w, in[i].h)));
		m++;
	}
	return m;
}

struct cf_rect cf_step(struct cf_framing *f, int64_t T, const struct cf_face *faces, int nf,
		       const struct cf_face *bodies, int nb)
{
	struct det fd[CF_MAX_FACES], bd[CF_MAX_FACES];
	for (int i = 0; i < NKF; i++)
		f->kf[i].seen = 0;
	nf = observe(f, faces, nf, 0, fd);
	nb = observe(f, bodies, nb, 1, bd);
	for (int i = 0; i < NKF; i++)          /* a filter whose id is missing this frame restarts */
		if (f->kf[i].used && !f->kf[i].seen)
			f->kf[i].used = 0;
	track(f, fd, nf, bd, nb, T);

	struct cf_track *trs[CF_MAX_TRACKS];
	int m = director_filter(f, trs);
	set_params(f, m > 1 ? &f->multi : &f->single);
	engine_update(f, trs, m, T);
	f->target = target(f);
	if (!isnull(f->target))
		f->cur = anim_drive(f, f->target, (double)T / 1e6);
	return f->cur;
}

void cf_viewport_homography(const struct cf_framing *f, struct cf_rect v, int out_w, int out_h, double Hm[9])
{
	/* output pixel -> ray of a pinhole camera with the viewport's FOV, rotated by Ry(midX) Rx(-midY),
	 * projected with the input intrinsics */
	const struct cf_camera *c = &f->cam;
	double fo = (out_w * 0.5) / tan(v.w * 0.5);
	double ax = midy(v) * -0.5, bx = midx(v) * 0.5;
	double sa = sin(ax), ca = cos(ax), sb = sin(bx), cb = cos(bx);
	/* quaternion matrices as in rot(), in double */
	double Mx[3][3] = { { 1, 0, 0 }, { 0, 1 - 2 * sa * sa, -2 * sa * ca }, { 0, 2 * sa * ca, 1 - 2 * sa * sa } };
	double My[3][3] = { { 1 - 2 * sb * sb, 0, 2 * sb * cb }, { 0, 1, 0 }, { -2 * sb * cb, 0, 1 - 2 * sb * sb } };
	double M[3][3], K[3][3] = { { c->fx, 0, c->cx }, { 0, c->fy, c->cy }, { 0, 0, 1 } };
	double V[3][3] = { { 1 / fo, 0, -out_w * 0.5 / fo }, { 0, 1 / fo, -out_h * 0.5 / fo }, { 0, 0, 1 } };
	double KM[3][3];
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			M[i][j] = My[i][0] * Mx[0][j] + My[i][1] * Mx[1][j] + My[i][2] * Mx[2][j];
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			KM[i][j] = K[i][0] * M[0][j] + K[i][1] * M[1][j] + K[i][2] * M[2][j];
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			Hm[i * 3 + j] = KM[i][0] * V[0][j] + KM[i][1] * V[1][j] + KM[i][2] * V[2][j];
}
