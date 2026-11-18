// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/picture.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

#define W 64
#define H 48

static void fill(uint8_t *y, uint8_t *uv, int luma, int u, int v)
{
	memset(y, luma, W * H);
	for (int i = 0; i < W * H / 2; i += 2) {
		uv[i] = (uint8_t)u;
		uv[i + 1] = (uint8_t)v;
	}
}

int main(void)
{
	static uint8_t y[W * H], uv[W * H / 2];
	struct cdn_frame f = { y, uv, W, H, W, W };
	struct cdn_picture_cfg p;
	struct cdn_picture *s = cdn_picture_create();

	CHECK(s);
	cdn_picture_defaults(&p);
	CHECK(cdn_picture_neutral(&p));

	/* neutral: untouched */
	fill(y, uv, 100, 140, 120);
	CHECK(cdn_picture_apply(s, &p, &f) == 0);
	CHECK(y[0] == 100 && y[W * H / 2 + W / 2] == 100 && uv[0] == 140 && uv[1] == 120);

	/* vignette: centre about unchanged, corners brighter, twice the signal above black at 1 */
	p.vignette = 1;
	CHECK(!cdn_picture_neutral(&p));
	CHECK(cdn_picture_apply(s, &p, &f) == 0);
	CHECK(abs(y[(H / 2) * W + W / 2] - 100) <= 1);
	CHECK(y[0] >= 180 && y[0] <= 184);              /* 16 + 84 * 2 */
	CHECK(y[W * H - 1] == y[0]);                    /* symmetric */
	CHECK(uv[0] > 140 && uv[1] < 120);              /* chroma scaled with the light */

	/* contrast / brightness / saturation / warmth */
	cdn_picture_defaults(&p);
	p.brightness = 0.25f;
	fill(y, uv, 100, 128, 128);
	cdn_picture_apply(s, &p, &f);
	CHECK(y[5] == 116);
	cdn_picture_defaults(&p);
	p.contrast = 2;
	fill(y, uv, 100, 128, 128);
	cdn_picture_apply(s, &p, &f);
	CHECK(y[5] == 72);
	cdn_picture_defaults(&p);
	p.saturation = 0;
	fill(y, uv, 100, 160, 90);
	cdn_picture_apply(s, &p, &f);
	CHECK(uv[0] == 128 && uv[1] == 128 && y[5] == 100);
	cdn_picture_defaults(&p);
	p.warmth = 1;
	fill(y, uv, 100, 128, 128);
	cdn_picture_apply(s, &p, &f);
	CHECK(uv[0] < 128 && uv[1] > 128);

	/* sharpen: a flat frame stays flat, an edge gets steeper */
	cdn_picture_defaults(&p);
	p.sharpen = 1;
	fill(y, uv, 100, 128, 128);
	cdn_picture_apply(s, &p, &f);
	CHECK(y[10 * W + 10] == 100);
	for (int r = 0; r < H; r++)
		for (int c = 0; c < W; c++)
			y[r * W + c] = c < W / 2 ? 80 : 160;
	cdn_picture_apply(s, &p, &f);
	CHECK(y[10 * W + W / 2 - 1] < 80 && y[10 * W + W / 2] > 160);
	CHECK(y[10 * W + 5] == 80);

	cdn_picture_destroy(s);
	if (fails)
		return 1;
	printf("picture: ok\n");
	return 0;
}
