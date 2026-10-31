// SPDX-License-Identifier: MIT
/* CPU backend: NEON and scalar agree bit for bit, and noise goes down. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/backend.h"
#include "../src/cpu/denoise_cpu.h"

#define W 320
#define H 180

static uint32_t rng = 12345;
static double gauss(void)
{
	double u, v;
	rng = rng * 1103515245u + 12345u; u = ((rng >> 8) + 1) / 16777217.0;
	rng = rng * 1103515245u + 12345u; v = ((rng >> 8) + 1) / 16777217.0;
	return sqrt(-2 * log(u)) * cos(6.283185307 * v);
}

/* clean scene: gradients, a few edges, and a block moving right */
static void truth(uint8_t *f, int t)
{
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++) {
			int v = 40 + x / 4 + ((y / 30) & 1) * 30;
			if (x >= 40 + 3 * t && x < 90 + 3 * t && y >= 60 && y < 120)
				v = 25;
			f[y * W + x] = (uint8_t)v;
		}
	for (int i = 0; i < W * H / 2; i++)
		f[W * H + i] = (uint8_t)(128 + ((i & 1) ? 10 : -6));
}

static void noisy(uint8_t *dst, const uint8_t *src, double sigma)
{
	for (int i = 0; i < W * H * 3 / 2; i++) {
		double v = src[i] + gauss() * (i < W * H ? sigma : sigma / 2);
		dst[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5);
	}
}

static double mse_y(const uint8_t *a, const uint8_t *b, int x0, int x1, int y0, int y1)
{
	double e = 0;
	for (int y = y0; y < y1; y++)
		for (int x = x0; x < x1; x++) {
			double d = (double)a[y * W + x] - b[y * W + x];
			e += d * d;
		}
	return e / ((x1 - x0) * (y1 - y0));
}

int main(void)
{
	size_t fs = W * H * 3 / 2;
	uint8_t *t = malloc(fs), *n = malloc(fs), *o1 = malloc(fs), *o2 = malloc(fs);
	void *sa = cdn_cpu_create(W, H, 1), *sb = cdn_cpu_create(W, H, 3);
	int fails = 0;
	double raw_mse = 0, den_mse = 0, ghost_raw = 0, ghost_den = 0;

	for (int i = 0; i < 40; i++) {
		struct cdn_frame fi = { n, n + W * H, W, H, W, W };
		struct cdn_frame fa = { o1, o1 + W * H, W, H, W, W };
		struct cdn_frame fb = { o2, o2 + W * H, W, H, W, W };
		truth(t, i);
		noisy(n, t, 8.0);
		cdn_cpu_process_ex(sa, &fi, &fa, 1.0f, false);
		cdn_cpu_process_ex(sb, &fi, &fb, 1.0f, true);
		if (memcmp(o1, o2, fs)) {
			size_t k = 0;
			while (o1[k] == o2[k]) k++;
			fprintf(stderr, "frame %d: scalar and NEON differ at byte %zu (%u vs %u)\n",
				i, k, o1[k], o2[k]);
			fails++;
			break;
		}
		if (i >= 15) {
			/* static region (top), and the band the block just left */
			raw_mse += mse_y(n, t, 0, W, 0, 50);
			den_mse += mse_y(o1, t, 0, W, 0, 50);
			int bx = 40 + 3 * i;
			ghost_raw += mse_y(n, t, bx - 12, bx, 60, 120);
			ghost_den += mse_y(o1, t, bx - 12, bx, 60, 120);
		}
	}
	double gain_db = 10 * log10(raw_mse / den_mse);
	printf("static region: %.2f dB better; trail behind the block: raw mse %.1f, denoised %.1f\n",
	       gain_db, ghost_raw / 25, ghost_den / 25);
	if (gain_db < 8) {
		fprintf(stderr, "static noise reduction %.2f dB < 8 dB\n", gain_db);
		fails++;
	}
	/* the trail must not be worse than leaving the noise in */
	if (ghost_den > ghost_raw) {
		fprintf(stderr, "ghosting: trail mse %.1f > raw %.1f\n", ghost_den / 25, ghost_raw / 25);
		fails++;
	}
	cdn_cpu_destroy(sa);
	cdn_cpu_destroy(sb);
	free(t); free(n); free(o1); free(o2);
	printf(fails ? "FAIL\n" : "PASS\n");
	return fails != 0;
}
