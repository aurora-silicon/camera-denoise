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
 * Per frame: segmentation (at most 15 times a second, as macOS's "lowfps"
 * variant), masks refined to the frame, then blur and light on the whole
 * frame, then Center Stage crops and scales (the SoC scaler when there is
 * one) with CinematicFraming's tunables.
 *
 * Bring-up switches (environment of the filter's service):
 *   CDN_APPLE_SEG_BGR=1   feed the network B, G, R instead of R, G, B
 *   CDN_APPLE_DEBUG=mask  show the refined person mask as the picture
 */
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../camera_effects.h"
#include "../zoom.h"
#include "ane_prog.h"
#include "framing.h"
#include "portrait.h"

#ifndef CDN_APPLE_DIR
#define CDN_APPLE_DIR "/var/lib/aurora-camera-denoise/apple"
#endif

/* The outputs are named by the first half of sha256 of their CoreML names. */
#define SEG_PERSON "293bdc1f04ed53222ad81ce2f31e4b37" /* person:0 */
#define SEG_SKIN "abc6c1daccb4856d5e9a5e9a120e6055"   /* skin:0 */
#define SEG_IMAGE "image__Placeholder__0"
/* macOS runs the low-fps variant: no more often than this */
#define SEG_PERIOD_NS (1000000000ull / 15)
/* mask smoothing per network run (0..1, 1 = no smoothing) */
#define SEG_SMOOTH 0.65f
/* blur strength for camera.background = blur */
#define BLUR_STRENGTH 0.85f

struct fx {
	struct ce_config cfg;
	struct apple_prog seg;
	int seg_in, seg_person, seg_skin;
	struct apple_masks *masks;
	float *rgb, *person, *skin;
	struct apple_portrait *portrait;
	struct apple_framing framing;
	struct cdn_zoom *zoom;
	uint64_t last_pts, last_seg;
	int seg_errors;
	bool bgr, show_mask;
};

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
	s->show_mask = getenv("CDN_APPLE_DEBUG") && !strcmp(getenv("CDN_APPLE_DEBUG"), "mask");
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

static int fx_process(void *opaque, const uint8_t *in, uint8_t *out, uint64_t pts)
{
	struct fx *s = opaque;
	int w = (int)s->cfg.width, h = (int)s->cfg.height;
	struct cdn_frame f = { out, out + (size_t)w * h, w, h, w, w };
	bool blur = s->cfg.background == 1, light = s->cfg.studio_light > 0;
	bool frame = s->cfg.framing, need_masks = blur || light || frame;
	float dt = s->last_pts && pts > s->last_pts ? (float)((pts - s->last_pts) * 1e-9) : 1.0f / 30;

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
	}
	struct apple_subject subject;

	apple_masks_subject(s->masks, &subject);
	if (blur || light || s->show_mask)
		apple_portrait_refine(s->portrait, s->masks, &f);
	if (s->show_mask) {
		int qw, qh;
		const float *a = apple_portrait_alpha(s->portrait, &qw, &qh);

		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++)
				out[(size_t)y * w + x] = (uint8_t)(a[(size_t)(y / 4) * qw + x / 4] * 255);
		memset(out + (size_t)w * h, 128, (size_t)w * h / 2);
		return 0;
	}
	if (blur)
		apple_portrait_blur(s->portrait, &f, BLUR_STRENGTH);
	if (light)
		apple_portrait_light(s->portrait, &f, &subject, s->cfg.studio_light);
	if (frame) {
		struct apple_viewport v = apple_framing_step(&s->framing, &subject, dt);
		struct cdn_rect r;

		apple_framing_crop(&v, w, h, &r.x, &r.y, &r.w, &r.h);
		if (s->zoom && (r.w < w || r.h < h))
			cdn_zoom_apply(s->zoom, &f, r);
	} else if (s->cfg.zoom > 1.0f && s->zoom) {
		cdn_zoom_apply(s->zoom, &f, cdn_zoom_rect(w, h, s->cfg.zoom));
	}
	return 0;
}

__attribute__((visibility("default"))) const struct ce_ops *camera_effects_entry(void)
{
	static const struct ce_ops ops = { CE_ABI, fx_create, fx_process, fx_reset, fx_destroy };

	return &ops;
}
