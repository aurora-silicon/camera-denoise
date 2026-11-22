// SPDX-License-Identifier: MIT
/* Projective NV12 resampling for Center Stage's rotated virtual camera, see warp.h. */
#include "warp.h"

#include <pthread.h>
#include <stdint.h>
#include <unistd.h>

#define SPAN 16
#define MAX_THREADS 8

struct job {
	const struct cdn_frame *src;
	struct cdn_frame *dst;
	const double *H;
	int y0, y1; /* luma rows; chroma rows y0/2 .. y1/2 */
};

/* input index coordinates (pixel centres at integers), 16.16, for output point (u, v) in a plane scaled by s */
static void map(const double *H, double u, double v, double s, int64_t *x, int64_t *y)
{
	/* the homography works in luma pixels; s = 2 for the chroma plane */
	double U = u * s, V = v * s;
	double z = H[6] * U + H[7] * V + H[8];
	double X = (H[0] * U + H[1] * V + H[2]) / z / s - 0.5;
	double Y = (H[3] * U + H[4] * V + H[5]) / z / s - 0.5;
	*x = (int64_t)(X * 65536.0);
	*y = (int64_t)(Y * 65536.0);
}

/* the last row or column: its left/top neighbour with full weight on it */
#define EDGE(i, f, n)                \
	do {                         \
		if ((i) >= (n) - 1) { \
			(i) = (n) - 2; \
			(f) = 256;     \
		}                    \
	} while (0)

static inline int64_t clamp64(int64_t v, int64_t lo, int64_t hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static void rows_luma(const struct job *j)
{
	const struct cdn_frame *s = j->src;
	const int64_t xmax = ((int64_t)s->width - 1) * 65536, ymax = ((int64_t)s->height - 1) * 65536;
	for (int v = j->y0; v < j->y1; v++) {
		uint8_t *out = j->dst->y + (size_t)v * j->dst->stride_y;
		int64_t ax, ay, bx, by;
		map(j->H, 0.5, v + 0.5, 1, &ax, &ay);
		for (int u0 = 0; u0 < j->dst->width; u0 += SPAN) {
			int n = j->dst->width - u0 < SPAN ? j->dst->width - u0 : SPAN;
			map(j->H, u0 + SPAN + 0.5, v + 0.5, 1, &bx, &by);
			int64_t dx = (bx - ax) / SPAN, dy = (by - ay) / SPAN, x = ax, y = ay;
			for (int k = 0; k < n; k++, x += dx, y += dy) {
				int64_t cx = clamp64(x, 0, xmax), cy = clamp64(y, 0, ymax);
				int ix = (int)(cx >> 16), iy = (int)(cy >> 16);
				uint32_t fx = (uint32_t)(cx >> 8) & 0xff, fy = (uint32_t)(cy >> 8) & 0xff;
				EDGE(ix, fx, s->width);
				EDGE(iy, fy, s->height);
				const uint8_t *p = s->y + (size_t)iy * s->stride_y + ix;
				uint32_t top = p[0] * (256 - fx) + p[1] * fx;
				uint32_t bot = p[s->stride_y] * (256 - fx) + p[s->stride_y + 1] * fx;
				out[u0 + k] = (uint8_t)((top * (256 - fy) + bot * fy + 32768) >> 16);
			}
			ax = bx;
			ay = by;
		}
	}
}

static void rows_chroma(const struct job *j)
{
	const struct cdn_frame *s = j->src;
	int cw = s->width / 2, ch = s->height / 2, ow = j->dst->width / 2;
	const int64_t xmax = ((int64_t)cw - 1) * 65536, ymax = ((int64_t)ch - 1) * 65536;
	for (int v = j->y0 / 2; v < j->y1 / 2; v++) {
		uint8_t *out = j->dst->uv + (size_t)v * j->dst->stride_uv;
		int64_t ax, ay, bx, by;
		map(j->H, 0.5, v + 0.5, 2, &ax, &ay);
		for (int u0 = 0; u0 < ow; u0 += SPAN) {
			int n = ow - u0 < SPAN ? ow - u0 : SPAN;
			map(j->H, u0 + SPAN + 0.5, v + 0.5, 2, &bx, &by);
			int64_t dx = (bx - ax) / SPAN, dy = (by - ay) / SPAN, x = ax, y = ay;
			for (int k = 0; k < n; k++, x += dx, y += dy) {
				int64_t cx = clamp64(x, 0, xmax), cy = clamp64(y, 0, ymax);
				int ix = (int)(cx >> 16), iy = (int)(cy >> 16);
				uint32_t fx = (uint32_t)(cx >> 8) & 0xff, fy = (uint32_t)(cy >> 8) & 0xff;
				EDGE(ix, fx, cw);
				EDGE(iy, fy, ch);
				const uint8_t *p = s->uv + (size_t)iy * s->stride_uv + 2 * ix;
				for (int c = 0; c < 2; c++) {
					uint32_t top = p[c] * (256 - fx) + p[c + 2] * fx;
					uint32_t bot = p[s->stride_uv + c] * (256 - fx) + p[s->stride_uv + c + 2] * fx;
					out[2 * (u0 + k) + c] = (uint8_t)((top * (256 - fy) + bot * fy + 32768) >> 16);
				}
			}
			ax = bx;
			ay = by;
		}
	}
}

static void *worker(void *arg)
{
	rows_luma(arg);
	rows_chroma(arg);
	return NULL;
}

void apple_warp_nv12(const struct cdn_frame *src, struct cdn_frame *dst, const double Hm[9], int threads)
{
	if (threads <= 0) {
		long n = sysconf(_SC_NPROCESSORS_ONLN);
		threads = n > 4 ? 4 : n > 0 ? (int)n : 1;
	}
	if (threads > MAX_THREADS)
		threads = MAX_THREADS;
	struct job jobs[MAX_THREADS];
	pthread_t tid[MAX_THREADS];
	int started[MAX_THREADS] = { 0 };
	/* even row bands, so each band owns whole chroma rows */
	int band = ((dst->height / threads) + 1) & ~1;
	for (int i = 0; i < threads; i++) {
		jobs[i] = (struct job){ src, dst, Hm, i * band, (i + 1) * band };
		if (jobs[i].y0 > dst->height)
			jobs[i].y0 = dst->height;
		if (jobs[i].y1 > dst->height || i == threads - 1)
			jobs[i].y1 = dst->height;
	}
	for (int i = 1; i < threads; i++)
		started[i] = !pthread_create(&tid[i], NULL, worker, &jobs[i]);
	worker(&jobs[0]);
	for (int i = 1; i < threads; i++) {
		if (started[i])
			pthread_join(tid[i], NULL);
		else
			worker(&jobs[i]);
	}
}
