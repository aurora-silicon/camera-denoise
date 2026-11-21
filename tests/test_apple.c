/* SPDX-License-Identifier: MIT */
/* Host tests of the Apple effects' image and framing halves, and of the HWX
 * tensor reader, with made-up masks and a made-up HWX (no Apple files). */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/apple/ane_prog.h"
#include "../src/apple/framing.h"
#include "../src/apple/portrait.h"

#define W 320
#define H 240

static uint8_t ybuf[W * H], uvbuf[W * H / 2];

static struct cdn_frame frame(void)
{
	return (struct cdn_frame){ ybuf, uvbuf, W, H, W, W };
}

/* a person: a box mask in the 256 x 192 segmentation, skin in its top part */
static void person_masks(struct apple_masks *m, float x0, float y0, float x1, float y1)
{
	memset(m, 0, sizeof(*m));
	for (int y = 0; y < APPLE_SEG_H; y++)
		for (int x = 0; x < APPLE_SEG_W; x++) {
			float fx = (x + 0.5f) / APPLE_SEG_W, fy = (y + 0.5f) / APPLE_SEG_H;
			int in = fx >= x0 && fx < x1 && fy >= y0 && fy < y1;

			m->person[y * APPLE_SEG_W + x] = in;
			/* a face: the middle third, top quarter of the person */
			m->skin[y * APPLE_SEG_W + x] = in && fy < y0 + (y1 - y0) * 0.25f &&
				fx >= x0 + (x1 - x0) / 3 && fx < x1 - (x1 - x0) / 3;
		}
	m->valid = 1;
}

static void test_subject(void)
{
	static struct apple_masks m;
	struct apple_subject s;

	memset(&m, 0, sizeof(m));
	apple_masks_subject(&m, &s, 4.0f / 3);
	assert(!s.present);
	m.valid = 1;
	apple_masks_subject(&m, &s, 4.0f / 3);
	assert(!s.present);
	person_masks(&m, 0.25f, 0.2f, 0.55f, 1.0f);
	apple_masks_subject(&m, &s, 4.0f / 3);
	assert(s.present && s.people == 1);
	assert(fabsf(s.x0 - 0.25f) < 0.02f && fabsf(s.x1 - 0.55f) < 0.02f);
	assert(fabsf(s.y0 - 0.2f) < 0.02f && s.y1 > 0.98f);
	assert(fabsf(s.head_x - 0.40f) < 0.02f);
	assert(fabsf(s.head_top - 0.2f) < 0.02f);
	assert(s.head_h > 0.12f && s.head_h < 0.35f);
	/* two people apart */
	for (int y = 0; y < APPLE_SEG_H; y++)
		for (int x = 200; x < 240; x++)
			m.person[y * APPLE_SEG_W + x] = y > 60;
	apple_masks_subject(&m, &s, 4.0f / 3);
	assert(s.present && s.people == 2 && s.x1 > 0.9f);
	printf("subject ok\n");
}

static void test_framing(void)
{
	struct apple_framing f;
	struct apple_viewport v;
	struct apple_subject s = { .present = true, .people = 1, .x0 = 0.6f, .y0 = 0.3f,
				   .x1 = 0.8f, .y1 = 1.0f, .head_x = 0.7f, .head_top = 0.2f,
				   .head_h = 0.17f };
	int x, y, w, h;

	apple_framing_init(&f, NULL, 0);
	assert(fabsf(f.pos.size - 0.83f) < 1e-4f && f.pos.cx == 0.5f);
	/* move towards the subject, smoothly (speed and acceleration caps) */
	float last = f.pos.cx, max_step = 0;

	for (int i = 0; i < 300; i++) {
		v = apple_framing_step(&f, &s, 1.0f / 30);
		max_step = fmaxf(max_step, fabsf(v.cx - last));
		last = v.cx;
	}
	assert(fabsf(v.cx - fminf(0.7f, 1 - v.size / 2)) < 0.01f);
	assert(max_step <= 0.25f * 2.0f / 30 + 1e-4f);
	/* head 0.17 high: viewport 4.4 x head, clamped to [0.7, 0.86] for one person */
	assert(fabsf(v.size - 0.748f) < 0.02f);
	/* the head top sits about 0.15 of the viewport below its top */
	assert(fabsf((s.head_top - (v.cy - v.size / 2)) / v.size - 0.15f) < 0.05f);
	/* at rest the viewport stops: no 2-pixel creep at the tail of the spring */
	{
		struct apple_viewport a = apple_framing_step(&f, &s, 1.0f / 30);
		struct apple_viewport b = apple_framing_step(&f, &s, 1.0f / 30);

		assert(a.cx == b.cx && a.cy == b.cy && a.size == b.size);
	}
	/* small moves inside the deadband do not move the viewport */
	struct apple_viewport before = v;

	s.head_x += 0.05f;
	for (int i = 0; i < 30; i++)
		v = apple_framing_step(&f, &s, 1.0f / 30);
	assert(fabsf(v.cx - before.cx) < 1e-3f);
	/* nobody: wait 5 s, then home */
	for (int i = 0; i < 30 * 4; i++)
		v = apple_framing_step(&f, NULL, 1.0f / 30);
	assert(fabsf(v.cx - before.cx) < 1e-3f);
	for (int i = 0; i < 30 * 10; i++)
		v = apple_framing_step(&f, NULL, 1.0f / 30);
	assert(fabsf(v.cx - 0.5f) < 0.01f && fabsf(v.size - 0.83f) < 0.01f);
	/* crop rectangle: even, inside, the viewport's size */
	apple_framing_crop(&v, 1920, 1080, &x, &y, &w, &h);
	assert(!(x & 1) && !(y & 1) && !(w & 1) && !(h & 1));
	assert(x >= 0 && y >= 0 && x + w <= 1920 && y + h <= 1080);
	assert(abs(w - (int)(0.83f * 1920)) <= 2 && abs(h - (int)(0.83f * 1080)) <= 2);
	/* close to the camera (a head 0.45 of the frame): still a crop, and it
	 * follows the head across the frame (2026-11-21: it grew to the whole
	 * frame and stopped) */
	apple_framing_init(&f, NULL, 0);
	s.head_h = 0.45f;
	s.head_x = 0.5f;
	s.head_top = 0.15f;
	for (int i = 0; i < 90; i++)
		v = apple_framing_step(&f, &s, 1.0f / 30);
	assert(v.size <= 0.86f + 1e-3f && v.size > 0.8f);
	/* a quick move to the side (1 s): followed within 1.5 s more, well
	 * before the 5 s recentring could do it */
	for (int i = 0; i < 30; i++) {
		s.head_x = 0.5f + 0.35f * (i + 1) / 30;
		v = apple_framing_step(&f, &s, 1.0f / 30);
	}
	for (int i = 0; i < 45; i++)
		v = apple_framing_step(&f, &s, 1.0f / 30);
	assert(f.still_for < 5.0f);
	assert(fabsf(v.cx - (1 - v.size / 2)) < 0.02f);
	s.head_h = 0.22f;
	s.head_top = 0.3f;
	s.head_x = 0.7f;
	/* user zoom: fixed size, still follows */
	apple_framing_init(&f, NULL, 2.0f);
	for (int i = 0; i < 300; i++)
		v = apple_framing_step(&f, &s, 1.0f / 30);
	assert(fabsf(v.size - 0.5f) < 1e-3f && v.cx > 0.6f);
	printf("framing ok\n");
}

static void test_portrait(void)
{
	static struct apple_masks m;
	struct cdn_frame f = frame();
	struct apple_portrait *p = apple_portrait_new(W, H);
	struct apple_subject s;
	int qw, qh;

	assert(p);
	/* background: a fine checkerboard; the person: flat grey 100 */
	person_masks(&m, 0.3f, 0.25f, 0.7f, 1.0f);
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++) {
			float fx = (x + 0.5f) / W, fy = (y + 0.5f) / H;
			int in = fx >= 0.3f && fx < 0.7f && fy >= 0.25f;

			ybuf[y * W + x] = in ? 100 : ((x / 2 + y / 2) & 1) ? 200 : 40;
		}
	memset(uvbuf, 128, sizeof(uvbuf));
	apple_portrait_refine(p, &m, &f);
	const float *a = apple_portrait_alpha(p, &qw, &qh);

	assert(qw == W / 4 && qh == H / 4);
	assert(a[(qh * 3 / 4) * qw + qw / 2] > 0.9f);  /* inside */
	assert(a[(qh / 8) * qw + qw / 8] < 0.1f);      /* outside */
	apple_portrait_blur(p, &f, 1.0f);
	/* the person is untouched */
	assert(ybuf[(H * 3 / 4) * W + W / 2] == 100);
	/* the background is smooth: far from the person the checkerboard is gone */
	int lo = 255, hi = 0;

	for (int y = 4; y < 20; y++)
		for (int x = 4; x < 40; x++) {
			int v = ybuf[y * W + x];

			lo = v < lo ? v : lo;
			hi = v > hi ? v : hi;
		}
	assert(hi - lo < 30 && lo > 90 && hi < 150);
	/* Studio Light: the face brighter, the background dimmer */
	apple_masks_subject(&m, &s, 4.0f / 3);
	assert(s.present);
	int face_before = ybuf[(int)((s.head_top + s.head_h / 3) * H) * W + (int)(s.head_x * W)];
	int bg_before = ybuf[10 * W + 10];

	apple_portrait_light(p, &f, &s, 1.0f);
	int face_after = ybuf[(int)((s.head_top + s.head_h / 3) * H) * W + (int)(s.head_x * W)];
	int bg_after = ybuf[10 * W + 10];

	/* subtle: the face lifted towards mid grey, the background at most ~20 % dimmer */
	assert(face_after > face_before + 12 && face_after < 140);
	assert(bg_after < bg_before && bg_after > bg_before * 0.78f);
	apple_portrait_free(p);
	printf("portrait ok\n");
}

static void test_rgb(void)
{
	struct cdn_frame f = frame();
	static float r[64 * 48], g[64 * 48], b[64 * 48];

	/* video-range white and BT.709 pure-ish red */
	memset(ybuf, 235, sizeof(ybuf));
	memset(uvbuf, 128, sizeof(uvbuf));
	apple_nv12_to_rgb(&f, 709, 0, r, g, b, 64, 48);
	assert(fabsf(r[100] - 1) < 0.01f && fabsf(g[100] - 1) < 0.01f && fabsf(b[100] - 1) < 0.01f);
	memset(ybuf, 63, sizeof(ybuf));
	for (int i = 0; i < W * H / 2; i += 2) {
		uvbuf[i] = 102;
		uvbuf[i + 1] = 240;
	}
	apple_nv12_to_rgb(&f, 709, 0, r, g, b, 64, 48);
	assert(r[500] > 0.95f && g[500] < 0.05f && b[500] < 0.05f);
	printf("rgb ok\n");
}

/* --- a made-up HWX: two segments, their names, their tensor commands ------- */

static uint8_t hwx[0x4000];
static size_t pos;
static unsigned ncmds;

static void put32(size_t at, uint32_t v) { memcpy(hwx + at, &v, 4); }
static void put64(size_t at, uint64_t v) { memcpy(hwx + at, &v, 8); }

static void segment(uint64_t vm, uint32_t prot)
{
	put32(pos, 0x19);
	put32(pos + 4, 72);
	memcpy(hwx + pos + 8, "__FVMLIB", 8);
	put64(pos + 24, vm);
	put32(pos + 60, prot);
	pos += 72;
	ncmds++;
}

static void ref(uint64_t vm, const char *name)
{
	uint32_t len = (0x18 + (uint32_t)strlen(name) + 1 + 7) & ~7u;

	put32(pos, 0x40);
	put32(pos + 4, len);
	put64(pos + 0x10, vm);
	strcpy((char *)hwx + pos + 0x18, name);
	pos += len;
	ncmds++;
}

static void tensor(const char *name, uint32_t c, uint32_t h, uint32_t w)
{
	uint64_t row = ((2ull * w + 63) / 64) * 64, plane = row * h;

	put32(pos, 4);
	put32(pos + 4, 0xd80);
	put32(pos + 8, 3);
	put32(pos + 0x28, 1);
	put32(pos + 0x2c, c);
	put32(pos + 0x30, h);
	put32(pos + 0x34, w);
	put64(pos + 0x50, plane * c);
	put64(pos + 0x58, plane);
	put64(pos + 0x60, row);
	put32(pos + 0x7c, 0xd40);
	strcpy((char *)hwx + pos + 0xd40, name);
	pos += 0xd80;
	ncmds++;
}

static void test_hwx(void)
{
	struct apple_tensor t[APPLE_PROG_SURFACES];
	int n;

	memset(hwx, 0, sizeof(hwx));
	put32(0, 0xbeefface);
	pos = 32;
	segment(0x1000, 2);
	segment(0x2000, 1);
	segment(0x3000, 5); /* not a surface */
	ref(0x1000, "293bdc1f04ed53222ad81ce2f31e4b37");
	ref(0x2000, "image__Placeholder__0");
	tensor("image__Placeholder__0", 3, 192, 256);
	tensor("293bdc1f04ed53222ad81ce2f31e4b37", 1, 192, 256);
	put32(16, ncmds);
	n = apple_hwx_tensors(hwx, pos, t, APPLE_PROG_SURFACES);
	assert(n == 2);
	assert(!strcmp(t[0].name, "293bdc1f04ed53222ad81ce2f31e4b37") && t[0].direction == 2);
	assert(t[0].c == 1 && t[0].h == 192 && t[0].w == 256 && t[0].row == 512);
	assert(!strcmp(t[1].name, "image__Placeholder__0") && t[1].direction == 1);
	assert(t[1].c == 3 && t[1].plane == 512 * 192 && t[1].batch == 3 * 512 * 192);
	/* too many surfaces for the caller, a bad magic, a truncated file */
	assert(apple_hwx_tensors(hwx, pos, t, 1) == -1);
	assert(apple_hwx_tensors(hwx, 100, t, APPLE_PROG_SURFACES) == -1);
	put32(0, 0xfeedface);
	assert(apple_hwx_tensors(hwx, pos, t, APPLE_PROG_SURFACES) == -1);
	printf("hwx ok\n");
}

int main(void)
{
	test_rgb();
	test_subject();
	test_framing();
	test_portrait();
	test_hwx();
	return 0;
}
