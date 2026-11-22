/* SPDX-License-Identifier: MIT */
/* The Apple effects' GPU passes against the CPU loops: the quarter-size sums
 * (bit for bit), and the composite on the same maps (Portrait, Studio Light
 * and both), on an odd-width frame (the J815's 1242 is not a multiple of 4),
 * with the frame sent again and reused from the quarter pass. Skips (77)
 * without a Vulkan device. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/apple/gpu_fx.h"
#include "../src/apple/portrait.h"

#define W 1242
#define H 700

static uint8_t y0buf[W * H], uv0buf[W * H / 2];

static void person_masks(struct apple_masks *m)
{
	memset(m, 0, sizeof(*m));
	for (int y = 0; y < APPLE_SEG_H; y++)
		for (int x = 0; x < APPLE_SEG_W; x++) {
			float fx = (x + 0.5f) / APPLE_SEG_W, fy = (y + 0.5f) / APPLE_SEG_H;
			int in = fx >= 0.3f && fx < 0.7f && fy >= 0.2f;

			m->person[y * APPLE_SEG_W + x] = in;
			m->skin[y * APPLE_SEG_W + x] = in && fy < 0.4f && fx >= 0.42f && fx < 0.58f;
		}
	m->valid = 1;
}

static void quarter_exact(struct apple_gpu *g)
{
	static uint8_t yb[W * H], uvb[W * H / 2];
	struct cdn_frame f = { yb, uvb, W, H, W, W };
	struct apple_portrait *p = apple_portrait_new(W, H);
	struct apple_quarter qc, qg;
	size_t n = (size_t)((W + 3) / 4) * ((H + 3) / 4);

	assert(p);
	memcpy(yb, y0buf, sizeof(yb));
	memcpy(uvb, uv0buf, sizeof(uvb));
	apple_portrait_quarter(p, &f, &qc);
	assert(!apple_gpu_quarter(g, &f, &qg));
	assert(!memcmp(qc.gsum, qg.gsum, n * sizeof(float)));
	assert(!memcmp(qc.ysum, qg.ysum, n * sizeof(float)));
	assert(!memcmp(qc.usum, qg.usum, n * sizeof(float)));
	assert(!memcmp(qc.vsum, qg.vsum, n * sizeof(float)));
	/* the last column is partial (1242 = 4 x 310 + 2): the guide sums 2 x 4 there */
	assert(qg.gsum[(W + 3) / 4 - 1] < qg.ysum[(W + 3) / 4 - 1]);
	apple_portrait_free(p);
	printf("quarter sums: gpu == cpu over %zu quarter pixels\n", n);
}

static int run(struct apple_gpu *g, bool blur, bool light, bool reuse)
{
	static uint8_t yc[W * H], uvc[W * H / 2], yg[W * H], uvg[W * H / 2];
	static struct apple_masks m;
	struct cdn_frame fc = { yc, uvc, W, H, W, W }, fg = { yg, uvg, W, H, W, W };
	struct apple_portrait *pc = apple_portrait_new(W, H), *pg = apple_portrait_new(W, H);
	struct apple_subject s;
	struct apple_fx_maps maps;
	bool lc = false, lg = false;
	int worst = 0;

	assert(pc && pg);
	person_masks(&m);
	apple_masks_subject(&m, &s, (float)W / H);
	memcpy(yc, y0buf, sizeof(yc));
	memcpy(uvc, uv0buf, sizeof(uvc));
	memcpy(yg, y0buf, sizeof(yg));
	memcpy(uvg, uv0buf, sizeof(uvg));
	struct apple_quarter qg;

	apple_portrait_refine(pc, &m, &fc);
	assert(!apple_gpu_quarter(g, &fg, &qg));
	apple_portrait_refine_q(pg, &m, &qg);
	if (blur) {
		apple_portrait_blur_prepare(pc, &fc, 0.3f);
		apple_portrait_blur_prepare_q(pg, &qg, 0.3f);
	}
	if (light) {
		lc = apple_portrait_light_prepare(pc, &s, 0.7f);
		lg = apple_portrait_light_prepare(pg, &s, 0.7f);
		assert(lc && lg);
	}
	if (blur)
		apple_portrait_blur_apply(pc, &fc);
	if (lc)
		apple_portrait_light_apply(pc, &fc);
	apple_portrait_maps(pg, &maps);
	assert(!apple_gpu_apply(g, &fg, &maps, blur, lg, reuse));
	for (size_t i = 0; i < sizeof(yc); i++) {
		int d = abs(yc[i] - yg[i]);

		worst = d > worst ? d : worst;
	}
	for (size_t i = 0; i < sizeof(uvc); i++) {
		int d = abs(uvc[i] - uvg[i]);

		worst = d > worst ? d : worst;
	}
	/* and the effect did something */
	assert(memcmp(yc, y0buf, sizeof(yc)));
	apple_portrait_free(pc);
	apple_portrait_free(pg);
	printf("blur %d light %d (frame %s): max |cpu - gpu| = %d\n", blur, light,
	       reuse ? "reused" : "sent again", worst);
	return worst;
}

int main(void)
{
	struct apple_gpu *g = apple_gpu_new(W, H, (W + 3) / 4, (H + 3) / 4);

	if (!g) {
		printf("no Vulkan device: skipped\n");
		return 77;
	}
	printf("gpu: %s\n", apple_gpu_name(g));
	srand(1);
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
			y0buf[y * W + x] = (uint8_t)(((x / 3 + y / 3) & 1 ? 190 : 50) + rand() % 16);
	for (size_t i = 0; i < sizeof(uv0buf); i++)
		uv0buf[i] = (uint8_t)(100 + rand() % 56);
	quarter_exact(g);
	for (int reuse = 0; reuse < 2; reuse++) {
		assert(run(g, true, false, reuse) <= 1);
		assert(run(g, false, true, reuse) <= 1);
		assert(run(g, true, true, reuse) <= 2);
	}
	apple_gpu_free(g);
	printf("apple gpu ok\n");
	return 0;
}
