// SPDX-License-Identifier: MIT
/*
 * Motion trail benchmark for the CPU backend.
 *
 *   cdn-motion [-s sigma] [-b background.nv12] [-W width -H height] [-q] [-t] [-v]
 *
 * Builds a clean 1080p scene (a dim synthetic room, or the average of every
 * frame of a static NV12 capture with -b), adds camera-like noise and moves a
 * textured hand-sized patch across it at several contrasts and speeds. The
 * truth is known, so for every pixel the patch has just left it measures the
 * bias of the output toward the patch (the trail) against the frames since
 * the patch left. It reports:
 *
 *   trail@1/@4: mean bias (levels) 1 and 4 frames after the patch left
 *   frames:     frames until the bias stays under 1 level
 *   energy:     sum of the bias over 30 frames (level frames)
 *   lag:        the same for the background showing through the patch
 *               after it arrived (sum over 30 frames)
 *   rms behind: rms error 1..8 frames behind the patch (grain where the
 *               motion reset the temporal filter)
 *
 * Also the static luma temporal std (median per-pixel std over 60 frames,
 * as the camera-quality noise.py) and the CPU time per frame on one
 * thread. -v prints the trail and lag curves frame by frame, -q runs
 * half the contrasts, -t uses heavier-tailed noise.
 *
 * Noise is synthetic (see noise_field), scaled to the requested std; its
 * detector statistics are printed next to those of static J700 captures:
 * 3x3 sum |d| and |5x5 sum d| percentiles in medians of |d|.
 */
#include <getopt.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/backend.h"
#include "../src/cpu/denoise_cpu.h"

#define NBANK 24
#define KMAX_T 30	/* frames tracked after the patch left / arrived */
#define BORDER 16

/*
 * Noise model. Default: Gaussian, low-passed with [1 4 1] in x and y. On
 * the main branch filter that gives a static filtered std of 1.83 from 12.5,
 * as measured live on the J616s (12.70 -> 1.79). -t adds heavy tails (10%
 * of samples at 3.5x the std of the rest), closer to the J700 statistics.
 */
static double noise_p = 0, noise_hi = 1, noise_lo = 1;

static int W = 1920, H = 1080;
static uint64_t rng_s = 0x9e3779b97f4a7c15ull;

static double urand(void)
{
	rng_s ^= rng_s << 13;
	rng_s ^= rng_s >> 7;
	rng_s ^= rng_s << 17;
	return ((rng_s >> 11) + 0.5) / 9007199254740992.0;
}

static double gauss(void)
{
	return sqrt(-2 * log(urand())) * cos(6.283185307179586 * urand());
}

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static int cmpf(const void *a, const void *b)
{
	float x = *(const float *)a, y = *(const float *)b;
	return x < y ? -1 : x > y;
}

/* one noise field of unit std, spatially correlated over about a pixel */
static void noise_field(float *f, int w, int h)
{
	float *a = malloc(sizeof(float) * w * h), *b = malloc(sizeof(float) * w * h);

	for (int i = 0; i < w * h; i++)
		a[i] = (float)(gauss() * (urand() < noise_p ? noise_hi : noise_lo));
	{
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++) {
				int xl = x > 0 ? x - 1 : x, xr = x < w - 1 ? x + 1 : x;
				b[y * w + x] = a[y * w + xl] + 4 * a[y * w + x] + a[y * w + xr];
			}
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++) {
				int ya = y > 0 ? y - 1 : y, yb = y < h - 1 ? y + 1 : y;
				a[y * w + x] = b[ya * w + x] + 4 * b[y * w + x] + b[yb * w + x];
			}
	}
	double s = 0;
	for (int i = 0; i < w * h; i++)
		s += (double)a[i] * a[i];
	float k = (float)(1.0 / sqrt(s / (w * h)));
	for (int i = 0; i < w * h; i++)
		f[i] = a[i] * k;
	free(a);
	free(b);
}

static float *bank_y[NBANK], *bank_c[NBANK];

/* 3x3 sum |n| and |5x5 sum n| percentiles in medians of |n| */
static void noise_stats(void)
{
	const float *f = bank_y[0];
	int n = 0, cap = (W / 4) * (H / 4);
	float *ad = malloc(sizeof(float) * cap), *m3 = malloc(sizeof(float) * cap),
	      *a5 = malloc(sizeof(float) * cap);
	/* d = frame - history, history ~ 1/15 of the noise */
	for (int y = 4; y < H - 4; y += 4)
		for (int x = 4; x < W - 4; x += 4) {
			if (n >= cap)
				break;
			float s3 = 0, s5 = 0;
			for (int dy = -2; dy <= 2; dy++)
				for (int dx = -2; dx <= 2; dx++) {
					float v = f[(y + dy) * W + x + dx];
					s5 += v;
					if (abs(dx) <= 1 && abs(dy) <= 1)
						s3 += fabsf(v);
				}
			ad[n] = fabsf(f[y * W + x]);
			m3[n] = s3;
			a5[n] = fabsf(s5);
			n++;
		}
	qsort(ad, n, sizeof(float), cmpf);
	qsort(m3, n, sizeof(float), cmpf);
	qsort(a5, n, sizeof(float), cmpf);
	float med = ad[n / 2];
	printf("noise model: std/median|n| %.2f (J700 1.75), 3x3 sum|n| p95 %.1f p99 %.1f medians (24.5, 32.6), "
	       "|5x5 sum n| p95 %.1f p99 %.1f (35.5, 57.6)\n",
	       1.0 / med, m3[n * 95 / 100] / med, m3[n * 99 / 100] / med, a5[n * 95 / 100] / med,
	       a5[n * 99 / 100] / med);
	free(ad);
	free(m3);
	free(a5);
}

static uint8_t *bg;	/* clean NV12 background */

static void synth_background(void)
{
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++) {
			double v = 60 + 40.0 * x / W + 20.0 * y / H + 6 * sin(x / 37.0) * sin(y / 23.0) +
				   3 * sin(x / 5.0 + y / 9.0);
			/* a few hard edges: door frames, a shelf */
			if ((x / 300) % 3 == 1 && y < H / 3)
				v += 25;
			bg[y * W + x] = (uint8_t)(v + 0.5);
		}
	for (int i = 0; i < W * H / 2; i++)
		bg[W * H + i] = (uint8_t)((i & 1) ? 134 : 122);
}

static int load_background(const char *path)
{
	size_t fs = (size_t)W * H * 3 / 2;
	FILE *f = fopen(path, "rb");
	if (!f)
		return -1;
	uint32_t *acc = calloc(fs, sizeof(uint32_t));
	uint8_t *buf = malloc(fs);
	int n = 0;
	while (fread(buf, fs, 1, f) == 1) {
		for (size_t i = 0; i < fs; i++)
			acc[i] += buf[i];
		n++;
	}
	fclose(f);
	if (n)
		for (size_t i = 0; i < fs; i++)
			bg[i] = (uint8_t)((acc[i] + n / 2) / n);
	free(acc);
	free(buf);
	return n ? 0 : -1;
}

#define OBJ 192

/*
 * Patch coverage (0..1) of luma pixel x for a patch whose left edge moves
 * from x0 to x0 + v / 2 during the exposure (half the frame time).
 */
static float coverage(int x, double x0, double v)
{
	float c = 0;
	for (int i = 0; i < 8; i++) {
		double l = x0 + v * 0.5 * (i + 0.5) / 8;
		if (x + 0.5 >= l && x + 0.5 < l + OBJ)
			c += 1.0f / 8;
	}
	return c;
}

struct result {
	double trail[KMAX_T + 1], lag[KMAX_T + 1];
	long ntrail[KMAX_T + 1], nlag[KMAX_T + 1];
	double behind_sq;
	long nbehind;
};

/* frame t of the clean scene with the patch at x0; cov is filled per pixel */
static void compose(uint8_t *f, float *cov, double x0, double v, int contrast, int y0, int base)
{
	memcpy(f, bg, (size_t)W * H * 3 / 2);
	for (int x = 0; x < W; x++) {
		float c = x0 > -1e8 ? coverage(x, x0, v) : 0;
		for (int y = y0; y < y0 + OBJ; y++) {
			cov[y * W + x] = c;
			if (c <= 0)
				continue;
			/* skin-like: smooth shading plus fine texture, moving with the patch */
			double u = x - x0, w = y - y0;
			double obj = base + contrast + contrast * 0.25 * sin(u / 11.0) * cos(w / 13.0) +
				     2 * sin(u / 3.0 + w / 4.0);
			double val = bg[y * W + x] * (1 - c) + obj * c;
			f[y * W + x] = (uint8_t)(val < 0 ? 0 : val > 255 ? 255 : val + 0.5);
		}
	}
}

static void add_noise(uint8_t *dst, const uint8_t *src, double sigma)
{
	const float *ny = bank_y[(int)(urand() * NBANK) % NBANK];
	const float *nc = bank_c[(int)(urand() * NBANK) % NBANK];
	int ox = (int)(urand() * W), oy = (int)(urand() * H);
	for (int y = 0; y < H; y++) {
		const float *row = ny + (size_t)((y + oy) % H) * W;
		for (int x = 0; x < W; x++) {
			double v = src[y * W + x] + sigma * row[(x + ox) % W];
			dst[y * W + x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5);
		}
	}
	/* chroma: about a tenth of the luma noise, as on the J616s */
	for (int y = 0; y < H / 2; y++)
		for (int x = 0; x < W; x++) {
			double v = src[W * H + y * W + x] + sigma * 0.1 * nc[((y + oy) % (H / 2)) * W + (x + ox) % W];
			dst[W * H + y * W + x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5);
		}
}

static double proc_ms;
static long proc_n;

static void run(void *st, uint8_t *in, uint8_t *out)
{
	struct cdn_frame fi = { in, in + (size_t)W * H, W, H, W, W };
	struct cdn_frame fo = { out, out + (size_t)W * H, W, H, W, W };
	double a = now_ms();
	cdn_cpu_process_ex(st, &fi, &fo, 1.0f, true);
	proc_ms += now_ms() - a;
	proc_n++;
}

static void motion_case(double sigma, int contrast, int speed, struct result *r)
{
	size_t fs = (size_t)W * H * 3 / 2;
	uint8_t *clean = malloc(fs), *in = malloc(fs), *out = malloc(fs);
	float *cov = calloc((size_t)W * H, sizeof(float));
	int *left = malloc(sizeof(int) * W * H), *arrived = malloc(sizeof(int) * W * H);
	int y0 = H / 2 - OBJ / 2 + 40;
	void *st = cdn_cpu_create(W, H, 1);

	memset(r, 0, sizeof(*r));
	for (int i = 0; i < W * H; i++)
		left[i] = arrived[i] = -1;
	/* mean background under the path: the patch is `contrast` above it */
	long sum = 0;
	for (int y = y0; y < y0 + OBJ; y++)
		for (int x = 0; x < W; x++)
			sum += bg[y * W + x];
	int base = (int)(sum / ((long)OBJ * W));

	/* 30 static frames, then the patch crosses from x = 200 for up to
	 * 1300 px or 120 frames, then 40 more static frames */
	int warm = 30, nmove = 1300 / speed;
	if (nmove > 120)
		nmove = 120;
	int total = warm + nmove + 40;
	for (int t = 0; t < total; t++) {
		double x0 = t >= warm && t < warm + nmove ? 200.0 + (t - warm) * speed : -1e9;
		compose(clean, cov, x0, speed, contrast, y0, base);
		add_noise(in, clean, sigma);
		run(st, in, out);
		if (t < warm)
			continue;
		for (int y = y0 + 4; y < y0 + OBJ - 4; y++)
			for (int x = BORDER; x < W - BORDER; x++) {
				int i = y * W + x;
				double e = (double)out[i] - clean[i];
				if (cov[i] > 0.99f) {
					if (arrived[i] < 0)
						arrived[i] = t;
					left[i] = -2;
					int k = t - arrived[i];
					if (k <= KMAX_T) {
						r->lag[k] -= e;
						r->nlag[k]++;
					}
				} else if (cov[i] <= 0) {
					if (left[i] == -2)
						left[i] = t;
					if (left[i] >= 0) {
						int k = t - left[i] + 1;
						if (k <= KMAX_T) {
							r->trail[k] += e;
							r->ntrail[k]++;
						}
						if (k <= 8) {
							r->behind_sq += e * e;
							r->nbehind++;
						}
					}
					arrived[i] = -1;
				}
			}
	}
	for (int k = 0; k <= KMAX_T; k++) {
		if (r->ntrail[k])
			r->trail[k] /= r->ntrail[k];
		if (r->nlag[k])
			r->lag[k] /= r->nlag[k];
	}
	cdn_cpu_destroy(st);
	free(clean); free(in); free(out); free(cov); free(left); free(arrived);
}

static void static_case(double sigma, double *raw_std, double *out_std)
{
	size_t fs = (size_t)W * H * 3 / 2;
	uint8_t *in = malloc(fs), *out = malloc(fs);
	int n = 60, warm = 30, step = 3;	/* every third pixel: plenty for a median */
	int gw = (W - 2 * BORDER) / step, gh = (H - 2 * BORDER) / step;
	double *s1 = calloc((size_t)gw * gh, sizeof(double)), *s2 = calloc((size_t)gw * gh, sizeof(double));
	double *r1 = calloc((size_t)gw * gh, sizeof(double)), *r2 = calloc((size_t)gw * gh, sizeof(double));
	void *st = cdn_cpu_create(W, H, 1);

	for (int t = 0; t < warm + n; t++) {
		add_noise(in, bg, sigma);
		run(st, in, out);
		if (t < warm)
			continue;
		for (int j = 0; j < gh; j++)
			for (int i = 0; i < gw; i++) {
				size_t p = (size_t)(BORDER + j * step) * W + BORDER + i * step, q = (size_t)j * gw + i;
				s1[q] += out[p]; s2[q] += (double)out[p] * out[p];
				r1[q] += in[p]; r2[q] += (double)in[p] * in[p];
			}
	}
	float *so = malloc(sizeof(float) * gw * gh), *sr = malloc(sizeof(float) * gw * gh);
	for (int q = 0; q < gw * gh; q++) {
		double m = s1[q] / n, mr = r1[q] / n;
		so[q] = (float)sqrt(fmax(s2[q] / n - m * m, 0));
		sr[q] = (float)sqrt(fmax(r2[q] / n - mr * mr, 0));
	}
	qsort(so, gw * gh, sizeof(float), cmpf);
	qsort(sr, gw * gh, sizeof(float), cmpf);
	*out_std = so[gw * gh / 2];
	*raw_std = sr[gw * gh / 2];
	cdn_cpu_destroy(st);
	free(in); free(out); free(s1); free(s2); free(r1); free(r2); free(so); free(sr);
}

int main(int argc, char **argv)
{
	const char *bgpath = NULL;
	double sigmas[2] = { 8, 13 };
	int nsig = 2, c, quick = 0, verbose = 0;

	while ((c = getopt(argc, argv, "s:b:W:H:qtv")) != -1) {
		switch (c) {
		case 's': sigmas[0] = atof(optarg); nsig = 1; break;
		case 'b': bgpath = optarg; break;
		case 'W': W = atoi(optarg); break;
		case 'H': H = atoi(optarg); break;
		case 'q': quick = 1; break;
		case 'v': verbose = 1; break;
		case 't': noise_p = 0.1; noise_hi = 2.8; noise_lo = 0.8; break;
		default:
			fprintf(stderr, "usage: cdn-motion [-s sigma] [-b background.nv12] [-W w -H h] [-q] [-t] [-v]\n");
			return 2;
		}
	}
	bg = malloc((size_t)W * H * 3 / 2);
	if (bgpath) {
		if (load_background(bgpath)) {
			fprintf(stderr, "cannot read %s\n", bgpath);
			return 1;
		}
	} else {
		synth_background();
	}
	for (int i = 0; i < NBANK; i++) {
		bank_y[i] = malloc(sizeof(float) * W * H);
		bank_c[i] = malloc(sizeof(float) * W * H / 2);
		noise_field(bank_y[i], W, H);
		noise_field(bank_c[i], W, H / 2);
	}
	noise_stats();

	static const int contrasts[] = { 8, 16, 32, 64 }, speeds[] = { 4, 16, 48 };
	for (int si = 0; si < nsig; si++) {
		double raw, filt;
		static_case(sigmas[si], &raw, &filt);
		printf("\nnoise std %.0f: static luma temporal std raw %.2f, filtered %.2f\n", sigmas[si], raw, filt);
		printf("contrast speed | trail@1 trail@4 frames energy | lag energy | rms behind\n");
		for (int ci = 0; ci < 4; ci++)
			for (int vi = 0; vi < 3; vi++) {
				if (quick && (ci & 1))
					continue;
				struct result r;
				motion_case(sigmas[si], contrasts[ci], speeds[vi], &r);
				double energy = 0, lag = 0;
				int frames = 0;
				for (int k = 1; k <= KMAX_T; k++) {
					energy += r.trail[k] > 0 ? r.trail[k] : 0;
					if (r.trail[k] >= 1.0)
						frames = k;
				}
				for (int k = 0; k <= KMAX_T; k++)
					lag += r.lag[k] > 0 ? r.lag[k] : 0;
				if (verbose) {
					for (int k = 1; k <= 12; k++) printf(" %.2f", r.trail[k]);
					printf("  | lag");
					for (int k = 0; k <= 12; k++) printf(" %.2f", r.lag[k]);
					printf("\n");
				}
				printf("%8d %5d | %7.2f %7.2f %6d %6.1f | %10.1f | %10.2f\n", contrasts[ci], speeds[vi],
				       r.trail[1], r.trail[4], frames, energy, lag,
				       sqrt(r.behind_sq / (r.nbehind ? r.nbehind : 1)));
			}
	}
	printf("\ncpu ms per %dx%d frame (one thread, NEON): %.2f\n", W, H, proc_ms / proc_n);
	return 0;
}
