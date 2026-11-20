/* SPDX-License-Identifier: MIT */
/*
 * aurora-camera-zoom-check [WIDTH HEIGHT ZOOM FRAMES]: the camera's digital zoom
 * on the SoC scaler against the CPU path, on a synthetic NV12 picture (smooth
 * gradients and a soft zone plate, video range). Prints per-frame time for each
 * and the PSNR between their outputs. The two filters differ (polyphase vs
 * bilinear), so the outputs are close, not equal.
 * Exit: 0 PASS (scaler ran, every frame, PSNR >= 30 dB), 1 FAIL, 77 SKIP (no scaler).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/zoom.h"

static double now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static void fill(uint8_t *y, uint8_t *uv, int w, int h)
{
	for (int j = 0; j < h; j++)
		for (int i = 0; i < w; i++) {
			double cx = (i - w / 2.0) / w, cy = (j - h / 2.0) / h;
			double v = 0.5 + 0.25 * sin(2 * M_PI * (cx * 3 + cy * 2)) +
				   0.2 * cos(40 * M_PI * (cx * cx + cy * cy));
			y[(size_t)j * w + i] = (uint8_t)(16 + 219 * fmin(1, fmax(0, v)));
		}
	for (int j = 0; j < h / 2; j++)
		for (int i = 0; i < w / 2; i++) {
			uv[(size_t)j * w + 2 * i] = (uint8_t)(128 + 60 * sin(2 * M_PI * i / (w / 2.0)));
			uv[(size_t)j * w + 2 * i + 1] = (uint8_t)(128 + 60 * cos(2 * M_PI * j / (h / 2.0)));
		}
}

static double psnr(const uint8_t *a, const uint8_t *b, size_t n)
{
	double se = 0;
	for (size_t i = 0; i < n; i++) {
		double d = (double)a[i] - b[i];
		se += d * d;
	}
	return se == 0 ? 99.0 : 10 * log10(255.0 * 255.0 * n / se);
}

int main(int argc, char **argv)
{
	int w = argc > 2 ? atoi(argv[1]) : 1920, h = argc > 2 ? atoi(argv[2]) : 1080;
	float zoom = argc > 3 ? (float)atof(argv[3]) : 2.0f;
	int frames = argc > 4 ? atoi(argv[4]) : 60;
	size_t ylen = (size_t)w * h, uvlen = ylen / 2;
	uint8_t *src = malloc(ylen + uvlen), *a = malloc(ylen + uvlen), *b = malloc(ylen + uvlen);
	struct cdn_rect r = cdn_zoom_rect(w, h, zoom);
	struct cdn_zoom *hw = cdn_zoom_new(w, h, CDN_ZOOM_SCALER), *cpu = cdn_zoom_new(w, h, CDN_ZOOM_CPU);
	double t_hw = 0, t_cpu = 0;
	int ok_hw = 0;

	if (!src || !a || !b || !hw || !cpu)
		return 1;
	fill(src, src + ylen, w, h);
	printf("zoom %.2fx on %dx%d: crop %dx%d at %d,%d, %d frames\n", zoom, w, h, r.w, r.h, r.x, r.y, frames);
	for (int i = 0; i < frames; i++) {
		struct cdn_frame fa = { a, a + ylen, w, h, w, w }, fb = { b, b + ylen, w, h, w, w };
		memcpy(a, src, ylen + uvlen);
		memcpy(b, src, ylen + uvlen);
		double t0 = now_ms();
		int rc = cdn_zoom_apply(hw, &fa, r);
		double t1 = now_ms();
		cdn_zoom_apply(cpu, &fb, r);
		double t2 = now_ms();
		if (rc == 0 && !strcmp(cdn_zoom_used(hw), "scaler"))
			ok_hw++;
		t_hw += t1 - t0;
		t_cpu += t2 - t1;
		if (rc && i == 0) {
			printf("SKIP: no scaler (%d)\n", rc);
			return 77;
		}
	}
	double py = psnr(a, b, ylen), pc = psnr(a + ylen, b + ylen, uvlen);
	printf("scaler: %d/%d frames, %.2f ms/frame (two copies included)\n", ok_hw, frames, t_hw / frames);
	printf("cpu:    %.2f ms/frame (bilinear)\n", t_cpu / frames);
	printf("scaler vs cpu: PSNR Y %.1f dB, CbCr %.1f dB\n", py, pc);
	FILE *o = getenv("CDN_ZOOM_DUMP") ? fopen(getenv("CDN_ZOOM_DUMP"), "wb") : NULL;
	if (o) { /* the scaler's last frame, raw NV12 */
		fwrite(a, 1, ylen + uvlen, o);
		fclose(o);
	}
	int pass = ok_hw == frames && py >= 30 && pc >= 30;
	printf("%s\n", pass ? "PASS" : "FAIL");
	cdn_zoom_free(hw);
	cdn_zoom_free(cpu);
	return pass ? 0 : 1;
}
