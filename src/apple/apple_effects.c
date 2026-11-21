// SPDX-License-Identifier: MIT
/*
 * The camera effects with Apple's own networks on the Neural Engine
 * (camera_effects.h ABI 1): Portrait (background blur), Studio Light and
 * Center Stage, the way macOS does them on this Mac.
 *
 * The network is the one macOS runs for them, compiled by macOS for this
 * Mac's Neural Engine: personsemantics (Portrait.framework's person
 * segmentation, from the aned cache of the user's own macOS install; it is
 * never shipped with this program). install-apple-effects copies it with the
 * sections the Linux driver needs to <models>/personsemantics/:
 *   model.hwx, generic.bin, operation.bin, procedure.bin, text-properties.bin
 * <models> is $CDN_APPLE_EFFECTS_DIR, else CDN_APPLE_DIR.
 *
 * Per frame: segmentation (every frame up to 30 a second: at 15 the mask
 * trailed a moving person), masks refined to the frame, then blur and light
 * on the whole frame, then Center Stage crops and scales (the SoC scaler when
 * there is one) with CinematicFraming's tunables.
 *
 * Bring-up switches (environment of the filter's service):
 *   CDN_APPLE_SEG_BGR=1     feed the network B, G, R instead of R, G, B
 *   CDN_APPLE_DEBUG=mask    show the refined person mask as the picture
 *   CDN_APPLE_DEBUG=stats   every 5 s: milliseconds per stage, the subject
 *                           and the Center Stage viewport (to stderr)
 *   CDN_APPLE_DUMP=<dir>    every 5 s, up to 8 times per stream: the frame
 *                           as it came in (NV12) and the network's person
 *                           and skin masks (8-bit, 256 x 192) to <dir>,
 *                           named by wall-clock time (each effect switch
 *                           makes a new plugin instance). Pictures of the
 *                           user: keep them local.
 */
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../camera_effects.h"
#include "../zoom.h"
#include "ane_prog.h"
#include "framing.h"
#include "gpu_fx.h"
#include "portrait.h"

#ifndef CDN_APPLE_DIR
#define CDN_APPLE_DIR "/var/lib/aurora-camera-denoise/apple"
#endif

/* The outputs are named by the first half of sha256 of their CoreML names. */
#define SEG_PERSON "293bdc1f04ed53222ad81ce2f31e4b37" /* person:0 */
#define SEG_SKIN "abc6c1daccb4856d5e9a5e9a120e6055"   /* skin:0 */
#define SEG_IMAGE "image__Placeholder__0"
/* no more often than this: every frame at 30 fps (a little slack for jitter) */
#define SEG_PERIOD_NS (1000000000ull / 40)
/* weight of a new network run where the mask holds still (moving parts: 1) */
#define SEG_SMOOTH 0.5f
#define STATS_PERIOD_NS 5000000000ull
#define DUMP_MAX 8
/* blur strength for camera.background = blur: a sigma of about 10 pixels at
 * 1280 wide (0.85 was ~22, about twice as blurry as macOS by eye, 2026-11-21) */
#define BLUR_STRENGTH 0.3f

struct fx {
	struct ce_config cfg;
	struct apple_prog seg;
	int seg_in, seg_person, seg_skin;
	struct apple_masks *masks;
	float *rgb, *person, *skin;
	struct apple_portrait *portrait;
	/* the full-size passes on the GPU (quarter-size sums, composite), or NULL: the CPU loops */
	struct apple_gpu *gpu;
	struct apple_framing framing;
	struct cdn_zoom *zoom;
	uint64_t last_pts, last_seg;
	int seg_errors;
	bool bgr, show_mask, stats;
	/* stats: summed milliseconds per stage since the last report */
	double t_seg, t_refine, t_blur, t_light, t_comp, t_frame;
	unsigned n_frames, n_seg;
	uint64_t last_stats, last_dump;
	struct apple_viewport last_view;
	const char *dump_dir;
	unsigned dumps;
};

static double now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec * 1e-6;
}

static void dump(struct fx *s, const uint8_t *in, uint64_t pts)
{
	char path[1200];
	FILE *fp;
	int w = (int)s->cfg.width, h = (int)s->cfg.height;
	static uint8_t m8[APPLE_SEG_W * APPLE_SEG_H];

	struct timespec ts;
	char stamp[32];

	clock_gettime(CLOCK_REALTIME, &ts);
	snprintf(stamp, sizeof(stamp), "%lld-%03ld", (long long)ts.tv_sec, ts.tv_nsec / 1000000);
	snprintf(path, sizeof(path), "%s/frame-%s-%dx%d.nv12", s->dump_dir, stamp, w, h);
	fp = fopen(path, "wb");
	if (!fp) {
		fprintf(stderr, "camera-effects: apple: dump %s: %s\n", path, strerror(errno));
		s->dump_dir = NULL;
		return;
	}
	fwrite(in, 1, (size_t)w * h * 3 / 2, fp);
	fclose(fp);
	const float *planes[2] = { s->person, s->skin };
	const char *names[2] = { "person", "skin" };

	for (int i = 0; i < 2; i++) {
		for (int j = 0; j < APPLE_SEG_W * APPLE_SEG_H; j++)
			m8[j] = (uint8_t)lroundf(fminf(fmaxf(planes[i][j], 0), 1) * 255);
		snprintf(path, sizeof(path), "%s/frame-%s-%s.pgm", s->dump_dir, stamp, names[i]);
		fp = fopen(path, "wb");
		if (!fp)
			continue;
		fprintf(fp, "P5\n%d %d\n255\n", APPLE_SEG_W, APPLE_SEG_H);
		fwrite(m8, 1, sizeof(m8), fp);
		fclose(fp);
	}
	s->dumps++;
	s->last_dump = pts;
}

static const char *models_dir(void)
{
	const char *d = getenv("CDN_APPLE_EFFECTS_DIR");

	return d && *d ? d : CDN_APPLE_DIR;
}

static void fx_reset(void *opaque)
{
	struct fx *s = opaque;

	s->masks->valid = 0;
	s->last_pts = s->last_seg = 0;
	apple_framing_reset(&s->framing);
}

static void fx_destroy(void *opaque)
{
	struct fx *s = opaque;

	if (!s)
		return;
	apple_prog_close(&s->seg);
	apple_portrait_free(s->portrait);
	apple_gpu_free(s->gpu);
	cdn_zoom_free(s->zoom);
	free(s->masks);
	free(s->rgb);
	free(s->person);
	free(s->skin);
	free(s);
}

static bool seg_shape_ok(const struct apple_prog *p, int i, unsigned c, unsigned dir)
{
	return i >= 0 && p->t[i].direction == dir && p->t[i].c == c &&
	       p->t[i].h == APPLE_SEG_H && p->t[i].w == APPLE_SEG_W;
}

static void *fx_create(const struct ce_config *cfg)
{
	struct fx *s = calloc(1, sizeof(*s));
	char dir[1024];
	int ret;

	if (!s)
		return NULL;
	s->cfg = *cfg;
	s->seg.fd = -1;
	s->bgr = getenv("CDN_APPLE_SEG_BGR") && !strcmp(getenv("CDN_APPLE_SEG_BGR"), "1");
	s->show_mask = getenv("CDN_APPLE_DEBUG") && strstr(getenv("CDN_APPLE_DEBUG"), "mask");
	s->stats = getenv("CDN_APPLE_DEBUG") && strstr(getenv("CDN_APPLE_DEBUG"), "stats");
	s->dump_dir = getenv("CDN_APPLE_DUMP") && *getenv("CDN_APPLE_DUMP") ? getenv("CDN_APPLE_DUMP") : NULL;
	snprintf(dir, sizeof(dir), "%s/personsemantics", models_dir());
	ret = apple_prog_open(&s->seg, dir);
	if (ret) {
		fprintf(stderr, "camera-effects: apple: person segmentation (%s): %s\n", dir, strerror(-ret));
		goto fail;
	}
	s->seg_in = apple_prog_find(&s->seg, SEG_IMAGE);
	s->seg_person = apple_prog_find(&s->seg, SEG_PERSON);
	s->seg_skin = apple_prog_find(&s->seg, SEG_SKIN);
	if (!seg_shape_ok(&s->seg, s->seg_in, 3, 1) || !seg_shape_ok(&s->seg, s->seg_person, 1, 2) ||
	    !seg_shape_ok(&s->seg, s->seg_skin, 1, 2)) {
		fprintf(stderr, "camera-effects: apple: %s is not the personsemantics network\n", dir);
		goto fail;
	}
	s->masks = calloc(1, sizeof(*s->masks));
	s->rgb = malloc(sizeof(float) * 3 * APPLE_SEG_W * APPLE_SEG_H);
	s->person = malloc(sizeof(float) * APPLE_SEG_W * APPLE_SEG_H);
	s->skin = malloc(sizeof(float) * APPLE_SEG_W * APPLE_SEG_H);
	s->portrait = apple_portrait_new((int)cfg->width, (int)cfg->height);
	if (!s->masks || !s->rgb || !s->person || !s->skin || !s->portrait)
		goto fail;
	/* GPU by default (CDN_APPLE_GPU=0: the CPU loops); without one the CPU does it */
	{
		const char *e = getenv("CDN_APPLE_GPU");

		if (!e || strcmp(e, "0"))
			s->gpu = apple_gpu_new((int)cfg->width, (int)cfg->height,
					       ((int)cfg->width + 3) / 4, ((int)cfg->height + 3) / 4);
		fprintf(stderr, "camera-effects: apple: full-size passes (quarter sums, composite) on the %s%s%s\n",
			s->gpu ? "GPU (" : "CPU", s->gpu ? apple_gpu_name(s->gpu) : "",
			s->gpu ? ")" : (e && !strcmp(e, "0") ? " (CDN_APPLE_GPU=0)" : " (no usable GPU)"));
	}
	apple_framing_init(&s->framing, NULL, cfg->zoom);
	if (cfg->framing || cfg->zoom > 1.0f)
		s->zoom = cdn_zoom_new((int)cfg->width, (int)cfg->height, CDN_ZOOM_AUTO);
	fprintf(stderr, "camera-effects: engine: apple (personsemantics on the ANE): "
		"background %u, studio light %.2f, framing %u, zoom %.2f\n",
		cfg->background, cfg->studio_light, cfg->framing, cfg->zoom);
	return s;
fail:
	fx_destroy(s);
	return NULL;
}

static int segment(struct fx *s, const struct cdn_frame *f)
{
	struct apple_prog *p = &s->seg;
	float *r = s->rgb, *g = r + APPLE_SEG_W * APPLE_SEG_H, *b = g + APPLE_SEG_W * APPLE_SEG_H;
	float *planes[3] = { r, g, b };

	if (s->bgr) {
		planes[0] = b;
		planes[2] = r;
	}
	int ret;

	apple_nv12_to_rgb(f, (int)s->cfg.matrix, (int)s->cfg.full_range, r, g, b,
			  APPLE_SEG_W, APPLE_SEG_H);
	for (unsigned c = 0; c < 3; c++)
		for (unsigned y = 0; y < APPLE_SEG_H; y++) {
			_Float16 *dst = apple_prog_at(p, s->seg_in, c, y, 0);
			const float *src = planes[c] + y * APPLE_SEG_W;

			for (unsigned x = 0; x < APPLE_SEG_W; x++)
				dst[x] = (_Float16)src[x];
		}
	ret = apple_prog_run(p);
	if (ret)
		return ret;
	for (unsigned y = 0; y < APPLE_SEG_H; y++) {
		const _Float16 *pp = apple_prog_at(p, s->seg_person, 0, y, 0);
		const _Float16 *ps = apple_prog_at(p, s->seg_skin, 0, y, 0);

		for (unsigned x = 0; x < APPLE_SEG_W; x++) {
			float vp = (float)pp[x], vs = (float)ps[x];

			/* NaN (a bad run) reads as background */
			s->person[y * APPLE_SEG_W + x] = vp == vp ? vp : 0;
			s->skin[y * APPLE_SEG_W + x] = vs == vs ? vs : 0;
		}
	}
	apple_masks_update(s->masks, s->person, s->skin, SEG_SMOOTH);
	return 0;
}

static void report(struct fx *s, const struct apple_subject *sub, uint64_t pts)
{
	unsigned n = s->n_frames ? s->n_frames : 1;

	fprintf(stderr, "camera-effects: apple: %u frames, %u segmentations; ms/frame: segment %.2f, "
		"refine %.2f, blur %.2f, light %.2f, composite %.2f (%s), all %.2f; subject %s x %.2f-%.2f head %.2f,%.2f h %.2f "
		"people %d; viewport %.2f,%.2f size %.2f\n",
		s->n_frames, s->n_seg, s->t_seg / n, s->t_refine / n, s->t_blur / n, s->t_light / n,
		s->t_comp / n, s->gpu ? "gpu" : "cpu", s->t_frame / n, sub->present ? "yes" : "no", sub->x0, sub->x1, sub->head_x, sub->head_top,
		sub->head_h, sub->people, s->last_view.cx, s->last_view.cy, s->last_view.size);
	s->t_seg = s->t_refine = s->t_blur = s->t_light = s->t_comp = s->t_frame = 0;
	s->n_frames = s->n_seg = 0;
	s->last_stats = pts;
}

static int fx_process(void *opaque, const uint8_t *in, uint8_t *out, uint64_t pts)
{
	struct fx *s = opaque;
	int w = (int)s->cfg.width, h = (int)s->cfg.height;
	struct cdn_frame f = { out, out + (size_t)w * h, w, h, w, w };
	bool blur = s->cfg.background == 1, light = s->cfg.studio_light > 0;
	bool frame = s->cfg.framing, need_masks = blur || light || frame;
	float dt = s->last_pts && pts > s->last_pts ? (float)((pts - s->last_pts) * 1e-9) : 1.0f / 30;
	double t0 = s->stats ? now_ms() : 0, t1 = t0;

	memcpy(out, in, (size_t)w * h * 3 / 2);
	s->last_pts = pts;
	if (need_masks && (!s->last_seg || pts < s->last_seg || pts - s->last_seg >= SEG_PERIOD_NS)) {
		int ret = segment(s, &f);

		if (ret) {
			/* frames pass through; the caller logs the first failure */
			if (s->seg_errors++ < 3)
				fprintf(stderr, "camera-effects: apple: segmentation: %s\n", strerror(-ret));
			return ret;
		}
		s->last_seg = pts;
		s->n_seg++;
		if (s->dump_dir && s->dumps < DUMP_MAX &&
		    (!s->dumps || pts < s->last_dump || pts - s->last_dump >= STATS_PERIOD_NS))
			dump(s, in, pts);
	}
	if (s->stats) {
		double t = now_ms();

		s->t_seg += t - t1;
		t1 = t;
	}
	struct apple_subject subject;

	apple_masks_subject(s->masks, &subject, (float)w / (float)h);
	/*
	 * The quarter-size sums refine and blur start from: the frame's one
	 * upload to the GPU, which keeps it for the composite below.
	 */
	struct apple_quarter q;
	bool loaded = false;

	if (blur || light || s->show_mask) {
		if (s->gpu) {
			int rc = apple_gpu_quarter(s->gpu, &f, &q);

			if (rc) {
				fprintf(stderr, "camera-effects: apple: GPU quarter pass failed (%d): CPU from now on\n", rc);
				apple_gpu_free(s->gpu);
				s->gpu = NULL;
			} else {
				loaded = true;
			}
		}
		if (!loaded)
			apple_portrait_quarter(s->portrait, &f, &q);
		apple_portrait_refine_q(s->portrait, s->masks, &q);
	}
	if (s->stats) {
		double t = now_ms();

		s->t_refine += t - t1;
		t1 = t;
	}
	if (s->show_mask) {
		int qw, qh;
		const float *a = apple_portrait_alpha(s->portrait, &qw, &qh);

		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++)
				out[(size_t)y * w + x] = (uint8_t)(a[(size_t)(y / 4) * qw + x / 4] * 255);
		memset(out + (size_t)w * h, 128, (size_t)w * h / 2);
		return 0;
	}
	/* the quarter-size maps of both effects from the frame as it came in, then one composite */
	bool lit = false;

	if (blur)
		apple_portrait_blur_prepare_q(s->portrait, &q, BLUR_STRENGTH);
	if (s->stats) {
		double t = now_ms();

		s->t_blur += t - t1;
		t1 = t;
	}
	if (light)
		lit = apple_portrait_light_prepare(s->portrait, &subject, s->cfg.studio_light);
	if (s->stats) {
		double t = now_ms();

		s->t_light += t - t1;
		t1 = t;
	}
	if (blur || lit) {
		int rc = -1;

		if (s->gpu) {
			struct apple_fx_maps m;

			apple_portrait_maps(s->portrait, &m);
			rc = apple_gpu_apply(s->gpu, &f, &m, blur, lit, loaded);
			if (rc) {
				/* f is untouched on a failure before the copy back; the CPU redoes it */
				fprintf(stderr, "camera-effects: apple: GPU composite failed (%d): CPU from now on\n", rc);
				apple_gpu_free(s->gpu);
				s->gpu = NULL;
			}
		}
		if (rc) {
			if (blur)
				apple_portrait_blur_apply(s->portrait, &f);
			if (lit)
				apple_portrait_light_apply(s->portrait, &f);
		}
	}
	if (s->stats) {
		double t = now_ms();

		s->t_comp += t - t1;
		t1 = t;
	}
	if (frame) {
		struct apple_viewport v = apple_framing_step(&s->framing, &subject, dt);
		struct cdn_rect r;

		s->last_view = v;
		apple_framing_crop(&v, w, h, &r.x, &r.y, &r.w, &r.h);
		if (s->zoom && (r.w < w || r.h < h))
			cdn_zoom_apply(s->zoom, &f, r);
	} else if (s->cfg.zoom > 1.0f && s->zoom) {
		cdn_zoom_apply(s->zoom, &f, cdn_zoom_rect(w, h, s->cfg.zoom));
	}
	if (s->stats) {
		s->t_frame += now_ms() - t0;
		s->n_frames++;
		if (!s->last_stats || pts < s->last_stats || pts - s->last_stats >= STATS_PERIOD_NS)
			report(s, &subject, pts);
	}
	return 0;
}

__attribute__((visibility("default"))) const struct ce_ops *camera_effects_entry(void)
{
	static const struct ce_ops ops = { CE_ABI, fx_create, fx_process, fx_reset, fx_destroy };

	return &ops;
}
