/* SPDX-License-Identifier: MIT */
/* Host tests of the effects' image and framing halves, the surface layout and
 * the tuning reader, with made-up masks and made-up tuning (no vendor files). */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/apple/ane_prog.h"
#include "../src/apple/cf_framing.h"
#include "../src/apple/portrait.h"
#include "../src/apple/tuning.h"
#include "../src/apple/portrait.h"
#include "../src/apple/warp.h"

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

/* Surface layouts from the sizes the driver reports: dense, padded rows, and sizes that fit no layout */
static void test_layout(void)
{
	struct apple_tensor t;

	assert(!apple_tensor_layout(&t, 3 * 192 * 512, 3, 192, 256) && t.row == 512 && t.plane == 192 * 512);
	assert(!apple_tensor_layout(&t, 192 * 576, 1, 192, 256) && t.row == 576 && t.plane == 192 * 576);
	assert(apple_tensor_layout(&t, 192 * 256, 1, 192, 256));        /* rows shorter than 256 fp16 */
	assert(apple_tensor_layout(&t, 3 * 192 * 512 + 2, 3, 192, 256)); /* not whole planes */
	assert(apple_tensor_layout(&t, 3 * 192 * 512, 0, 192, 256));
	printf("layout ok\n");
}

/* The tuning reader: every key, comments and spaces; a missing key or a bad value refuses the file */
static const char *write_tmp(const char *text)
{
	static char path[] = "/tmp/cdn-tuning-XXXXXX";
	int fd;
	FILE *f;

	strcpy(path, "/tmp/cdn-tuning-XXXXXX");
	fd = mkstemp(path);
	assert(fd >= 0);
	f = fdopen(fd, "w");
	fputs(text, f);
	fclose(f);
	return path;
}

static void test_tuning(void)
{
	float a = 0, b = 0;
	const struct apple_tuning_key k[] = { { "one.a", &a }, { "two-b", &b } };
	char err[256];
	const char *p;

	p = write_tmp("# made up\n one.a = 0.25  # comment\n\ntwo-b=-3e-2\nunused = 7\n");
	assert(!apple_tuning_read(p, k, 2, err, sizeof(err)) && a == 0.25f && fabsf(b + 0.03f) < 1e-7f);
	unlink(p);
	p = write_tmp("one.a = 1\n");
	assert(apple_tuning_read(p, k, 2, err, sizeof(err)) && strstr(err, "two-b"));
	unlink(p);
	p = write_tmp("one.a = 1\ntwo-b = x\n");
	assert(apple_tuning_read(p, k, 2, err, sizeof(err)) && strstr(err, "not a number"));
	unlink(p);
	assert(apple_tuning_read("/nonexistent/tuning.conf", k, 2, err, sizeof(err)));
	printf("tuning ok\n");
}

/* Center Stage's detections from the masks: a body per person, a face only when the skin looks like one */
static void test_people(void)
{
	static struct apple_masks m;
	struct apple_person p[APPLE_MAX_PEOPLE];
	int n;

	person_masks(&m, 0.25f, 0.2f, 0.55f, 1.0f);
	n = apple_masks_people(&m, 4.0f / 3, p, APPLE_MAX_PEOPLE);
	assert(n == 1);
	assert(fabsf(p[0].bx - 0.25f) < 0.02f && fabsf(p[0].by - 0.2f) < 0.02f);
	assert(fabsf(p[0].bw - 0.30f) < 0.03f && p[0].by + p[0].bh > 0.98f);
	/* the made-up skin is a 0.1 x 0.2 block: a face about 1.2 of its width, square in frame pixels */
	assert(p[0].face);
	assert(fabsf(p[0].fx + p[0].fw / 2 - 0.40f) < 0.02f);
	assert(fabsf(p[0].fw - 0.12f) < 0.02f && fabsf(p[0].fh - p[0].fw * 4 / 3) < 0.02f);
	assert(p[0].fy > 0.2f && p[0].fy < 0.26f);
	/* skin down the whole top half (a bare torso): no face, the body stays */
	for (int y = 0; y < APPLE_SEG_H / 2; y++)
		for (int x = 64; x < 140; x++)
			m.skin[y * APPLE_SEG_W + x] = m.person[y * APPLE_SEG_W + x];
	n = apple_masks_people(&m, 4.0f / 3, p, APPLE_MAX_PEOPLE);
	assert(n == 1 && !p[0].face);
	/* two people apart */
	person_masks(&m, 0.05f, 0.3f, 0.35f, 1.0f);
	for (int y = 80; y < APPLE_SEG_H; y++)
		for (int x = 180; x < 230; x++)
			m.person[y * APPLE_SEG_W + x] = 1;
	n = apple_masks_people(&m, 4.0f / 3, p, APPLE_MAX_PEOPLE);
	assert(n == 2 && p[0].bx < 0.2f && p[1].bx > 0.65f && p[0].face && !p[1].face);
	printf("people ok\n");
}

/* Center Stage's framing session, with a made-up tuning (not any shipped one). */
static const char TEST_FRAMING[] =
	"single.deadband-width = 0.55\nsingle.deadband-height = 0.3\nsingle.top-offset = 0.12\n"
	"single.deadband-width-min = 0.8\nsingle.deadband-width-max = 1.25\nsingle.padding-width = 1\n"
	"single.padding-height = 1\n"
	"multi.deadband-width = 0.75\nmulti.deadband-height = 0.45\nmulti.top-offset = 0.12\n"
	"multi.deadband-width-min = 0.8\nmulti.deadband-width-max = 1.25\nmulti.padding-width = 1\n"
	"multi.padding-height = 1\n"
	"speed.horizontal = 2.2\nspeed.vertical = 2.4\nspeed.zoom-in = 1\nspeed.zoom-out = 2\n"
	"acceleration.horizontal = 1.4\nacceleration.vertical = 1.4\nacceleration.zoom = 1\n"
	"view.default = 0.7\nview.minimum = 0.35\nhead.tiny-min = 0.01\nhead.tiny-max = 0.02\n"
	"subject.movement-tolerance = 0.25\nrecenter.duration = 1.2\nrecenter.idle = 4\nrecenter.delay = 2.5\n"
	"home.delay = 5\nhome.duration = 5\ndrift.enabled = 1\ndrift.duration = 1\ndrift.offset = 0.3\n"
	"camera.focal-length = 1500\n";

static void test_cf_framing(void)
{
	/* the 1920x1080 ISP preset: the binned 1520x1440 input's 0,292 1520x856 crop, scaled */
	struct cf_camera cam = { 751.66665 * 1920 / 1520, 751.66665 * 1080 / 856, 760.0 * 1920 / 1520,
				 428.0 * 1080 / 856, 1920, 1080, 960, 540, 1326, 1920, 1080, 0, { 0 }, 0.001, { 0 } };
	static struct cf_framing f;
	struct cf_rect v, prev;
	int64_t t = 1000000;
	double maxstep = 0;

	struct cf_tuning tu;
	char err[256];
	const char *tp = write_tmp(TEST_FRAMING);

	assert(!cf_tuning_read(&tu, tp, err, sizeof(err)));
	unlink(tp);
	cf_init(&f, &cam, &tu);
	/* nobody: the default viewport, centred */
	for (int i = 0; i < 90; i++, t += 33333)
		v = cf_step(&f, t, NULL, 0, NULL, 0);
	assert(fabs(v.x + v.w / 2) < 1e-3 && fabs(v.y + v.h / 2) < 0.05);
	double home_w = v.w;
	assert(home_w > 0.9 && home_w < 1.2);
	/* someone on the right: the viewport follows smoothly and settles */
	struct cf_face face = { 1, 0.66, 0.30, 0.07, 0.124 }, body = { 1, 0.58, 0.25, 0.23, 0.75 };
	prev = v;
	for (int i = 0; i < 300; i++, t += 33333) {
		v = cf_step(&f, t, &face, 1, &body, 1);
		double d = fabs(v.x - prev.x) + fabs(v.y - prev.y) + fabs(v.w - prev.w);
		maxstep = d > maxstep ? d : maxstep;
		prev = v;
	}
	assert(v.x + v.w / 2 > 0.15);
	assert(maxstep < 0.05);
	struct cf_rect a = v;
	for (int i = 0; i < 30; i++, t += 33333)
		v = cf_step(&f, t, &face, 1, &body, 1);
	assert(fabs(v.x - a.x) < 2e-3 && fabs(v.w - a.w) < 2e-3);
	/* the face turns away: the body keeps the framing */
	for (int i = 0; i < 60; i++, t += 33333)
		v = cf_step(&f, t, NULL, 0, &body, 1);
	assert(fabs(v.x - a.x) < 0.05);
	/* nobody: wait 5 s, then home over 5 s */
	for (int i = 0; i < 120; i++, t += 33333)
		v = cf_step(&f, t, NULL, 0, NULL, 0);
	assert(v.x + v.w / 2 > 0.1);
	for (int i = 0; i < 300; i++, t += 33333)
		v = cf_step(&f, t, NULL, 0, NULL, 0);
	assert(fabs(v.x + v.w / 2) < 0.02 && fabs(v.w - home_w) < 0.02);
	/* the homography of the centred viewport maps the output centre to the principal point */
	double Hm[9];
	cf_viewport_homography(&f, (struct cf_rect){ -0.5, -0.3, 1.0, 0.6 }, 1920, 1080, Hm);
	double z = Hm[6] * 960 + Hm[7] * 540 + Hm[8];
	assert(fabs((Hm[0] * 960 + Hm[1] * 540 + Hm[2]) / z - (cam.cx + cam.fx * tan(0.0))) < 1e-6);
	printf("cf framing ok (largest step %.4f rad/frame)\n", maxstep);
}

static void test_warp(void)
{
	static uint8_t sy[W * H], suv[W * H / 2], dy[W * H], duv[W * H / 2];
	struct cdn_frame src = { sy, suv, W, H, W, W }, dst = { dy, duv, W, H, W, W };
	const double id[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 }, half[9] = { 1, 0, 0.5, 0, 1, 0, 0, 0, 1 };

	for (int i = 0; i < W * H; i++)
		sy[i] = (uint8_t)(i * 7 + (i / W) * 13);
	for (int i = 0; i < W * H / 2; i++)
		suv[i] = (uint8_t)(i * 5 + 3);
	apple_warp_nv12(&src, &dst, id, 3);
	assert(!memcmp(sy, dy, sizeof(sy)) && !memcmp(suv, duv, sizeof(suv)));
	/* half a pixel to the right: the mean of the two neighbours */
	for (int i = 0; i < W * H; i++)
		sy[i] = (uint8_t)((i % W) * 2);
	apple_warp_nv12(&src, &dst, half, 2);
	for (int y = 0; y < H; y++)
		for (int x = 0; x < 126; x++) /* below the ramp's wrap at 256 */
			assert(abs(dy[y * W + x] - (2 * x + 1)) <= 1);
	printf("warp ok\n");
}

int main(void)
{
	test_rgb();
	test_subject();
	test_people();
	test_cf_framing();
	test_warp();
	test_portrait();
	test_layout();
	test_tuning();
	return 0;
}
