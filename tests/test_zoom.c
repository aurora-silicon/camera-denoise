/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/zoom.h"

#define W 256
#define H 128

static uint8_t ybuf[W * H], uvbuf[W * H / 2];

static struct cdn_frame frame(void)
{
	return (struct cdn_frame){ ybuf, uvbuf, W, H, W, W };
}

int main(void)
{
	struct cdn_rect r = cdn_zoom_rect(1920, 1080, 2.0f);
	assert(r.x == 480 && r.y == 270 && r.w == 960 && r.h == 540);
	r = cdn_zoom_rect(1920, 1080, 1.0f);
	assert(r.x == 0 && r.y == 0 && r.w == 1920 && r.h == 1080);
	r = cdn_zoom_rect(1920, 1080, 0.5f); /* below 1 is no zoom */
	assert(r.w == 1920 && r.h == 1080);
	r = cdn_zoom_rect(1920, 1080, 100.0f); /* never under 64 */
	assert(r.w == 64 && r.h == 64 && !(r.x & 1) && !(r.y & 1));

	/* a flat frame stays flat */
	memset(ybuf, 100, sizeof(ybuf));
	memset(uvbuf, 128, sizeof(uvbuf));
	struct cdn_frame f = frame();
	struct cdn_zoom *z = cdn_zoom_new(W, H, CDN_ZOOM_CPU);
	assert(z);
	assert(!cdn_zoom_apply(z, &f, cdn_zoom_rect(W, H, 2.0f)));
	assert(!strcmp(cdn_zoom_used(z), "cpu"));
	for (int i = 0; i < W * H; i++)
		assert(ybuf[i] == 100);
	for (int i = 0; i < W * H / 2; i++)
		assert(uvbuf[i] == 128);

	/* a horizontal ramp: a 2x zoom halves its slope (each source pixel spans two) */
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
			ybuf[y * W + x] = (uint8_t)(x / 2 + 32);
	assert(!cdn_zoom_apply(z, &f, cdn_zoom_rect(W, H, 2.0f)));
	int a = ybuf[(H / 2) * W + 100], b = ybuf[(H / 2) * W + 140];
	assert(abs((b - a) - 10) <= 1); /* 40 output px = 20 source px = 10 levels */

	/* zoom 1: untouched, nothing used */
	assert(!cdn_zoom_apply(z, &f, cdn_zoom_rect(W, H, 1.0f)));
	assert(!strcmp(cdn_zoom_used(z), "none"));
	cdn_zoom_free(z);

	/* scaler only, without one (a test host): refused, frame unchanged */
	setenv("CDN_ZOOM_DEVICE", "/nonexistent/video", 1);
	z = cdn_zoom_new(W, H, CDN_ZOOM_SCALER);
	assert(z);
	memset(ybuf, 7, sizeof(ybuf));
	assert(cdn_zoom_apply(z, &f, cdn_zoom_rect(W, H, 2.0f)) == -ENODEV);
	assert(ybuf[0] == 7 && ybuf[W * H - 1] == 7);
	cdn_zoom_free(z);

	/* bad sizes */
	assert(!cdn_zoom_new(63, 64, CDN_ZOOM_CPU) && !cdn_zoom_new(65, 64, CDN_ZOOM_CPU));
	puts("zoom: ok");
	return 0;
}
