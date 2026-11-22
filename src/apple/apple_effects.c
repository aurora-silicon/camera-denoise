// SPDX-License-Identifier: MIT
/*
 * The camera effects on the Neural Engine (camera_effects.h ABI 1): Portrait
 * (background blur), Studio Light and Center Stage.
 *
 * The person and skin segmentation network is the user's own: compiled for
 * this Mac's Neural Engine by the user's macOS install and copied from it by
 * install-apple-effects (never shipped with this program) to
 * <models>/segmentation/ with the sections the Linux driver loads with it:
 *   model.hwx, generic.bin, operation.bin, procedure.bin, text-properties.bin
 * Center Stage's tuning comes from the same install, <models>/tuning/
 * framing.conf (cf_framing.h); without it Center Stage stays off.
 * <models> is CDN_APPLE_DIR (the build's appledir).
 *
 * Per frame: segmentation (every frame up to 30 a second: at 15 the mask
 * trailed a moving person), masks refined to the frame, then blur and light
 * on the whole frame, then Center Stage: the framing session (cf_framing.c)
 * on a face and a body per person, rendered as its virtual camera rotated to
 * the viewport (warp.c, sub-pixel). The faces and bodies are the ISP
 * firmware's own detections when the camera driver runs its detector (sysfs
 * .../apple-isp/<dev>/detect/detections), else from the masks; the lens is
 * the calibration the ISP firmware returns when the driver reads it
 * (.../detect/lens_distortion), else a pinhole. The ISP preset that gives
 * the frame size sets the crop.
 *
 * Debugging (environment of the filter's service; nothing here changes what
 * a user gets without it):
 *   CDN_EFFECTS_DEBUG=mask   show the refined person mask as the picture
 *   CDN_EFFECTS_DEBUG=stats  every 5 s: milliseconds per stage and the
 *                            Center Stage viewport (to stderr)
 *   CDN_EFFECTS_DEBUG=faces  Center Stage's per-frame faces and bodies to
 *                            <CDN_EFFECTS_DUMP or /tmp>/cf-faces-<time>.in
 *   CDN_EFFECTS_DUMP=<dir>   every 5 s, up to 8 times per stream: the frame
 *                            (NV12) and the person and skin masks (8-bit,
 *                            256 x 192) to <dir>. Pictures of the user:
 *                            keep them local.
 */
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../camera_effects.h"
#include "../zoom.h"
#include "ane_prog.h"
#include "cf_framing.h"
#include "tuning.h"
#include "gpu_fx.h"
#include "portrait.h"
#include "warp.h"

#ifndef CDN_APPLE_DIR
#define CDN_APPLE_DIR "/var/lib/aurora-camera-denoise/apple"
#endif

/* no more often than this: every frame at 30 fps (a little slack for jitter) */
#define SEG_PERIOD_NS (1000000000ull / 40)
/* weight of a new network run where the mask holds still (moving parts: 1) */
#define SEG_SMOOTH 0.5f
#define STATS_PERIOD_NS 5000000000ull
#define DUMP_MAX 8
/* blur strength for camera.background = blur: a sigma of about 10 pixels at
 * 1280 wide (0.85, a sigma of ~22, looked far too blurry on the J815) */
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
	struct cdn_zoom *zoom;
	/* Center Stage (NULL: off) */
	struct cf_framing *cf;
	uint8_t *warp_buf;
	struct {
		float cx, cy, rx, ry, rw, rh; /* body centre; the face relative to the body */
		int id, face_age;
	} ppl[APPLE_MAX_PEOPLE];
	int nppl, next_id, cf_faces, cf_bodies;
	/* the ISP firmware's detections (-1: no detect node) */
	int isp_det_fd;
	unsigned long long isp_det_seq;
	int isp_det_frames, isp_det_fallbacks, isp_det_stale;
	int64_t cf_last_t;
	struct cf_rect cf_view;
	FILE *facelog;
	double t_cs;
	uint64_t last_pts, last_seg;
	int seg_errors;
	bool bgr, show_mask, stats;
	/* stats: summed milliseconds per stage since the last report */
	double t_seg, t_refine, t_blur, t_light, t_comp, t_frame;
	unsigned n_frames, n_seg;
	uint64_t last_stats, last_dump;
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
	return CDN_APPLE_DIR;
}

static void fx_reset(void *opaque)
{
	struct fx *s = opaque;

	s->masks->valid = 0;
	s->last_pts = s->last_seg = 0;
	if (s->cf)
		cf_reset(s->cf);
	s->nppl = 0;
	s->cf_last_t = INT64_MIN;
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
	free(s->cf);
	free(s->warp_buf);
	if (s->facelog)
		fclose(s->facelog);
	if (s->isp_det_fd >= 0)
		close(s->isp_det_fd);
	free(s->masks);
	free(s->rgb);
	free(s->person);
	free(s->skin);
	free(s);
}


/*
 * The front camera as Center Stage sees it: the focal length from the tuning (principal point at the centre),
 * through the ISP preset that gives this frame size (the board's ISP presets in the device tree: a crop of the
 * binned 1520 x 1440 input, scaled to the output) as the camera driver fits it into the lens's image circle
 * (apple,lens-circle = 470 per mille of the input width: 1520x856+0+292 -> 1242x700+138+370, as the driver
 * logs it). Other sizes: a centred crop, fitted the same way.
 */
#define LENS_CIRCLE 470

static int crop_in_circle(int64_t ox, int64_t oy, int64_t w, int64_t h, int64_t r)
{
	int64_t dx = llabs(ox) + w / 2, dy = llabs(oy) + h / 2;

	return dx * dx + dy * dy <= r * r;
}

static void fit_lens(int *x, int *y, int *cw, int *ch)
{
	const int64_t in_w = 1520, in_h = 1440, r = (int64_t)LENS_CIRCLE * in_w / 1000;
	int64_t w = *cw, h = *ch, lo = 0, hi = 1024;
	int64_t ox = 2 * (int64_t)*x + w - in_w, oy = 2 * (int64_t)*y + h - in_h;

	if (crop_in_circle(ox, oy, 2 * w, 2 * h, 2 * r))
		return;
	while (hi - lo > 1) {
		int64_t s = (lo + hi) / 2;

		if (crop_in_circle(ox, oy, 2 * w * s / 1024, 2 * h * s / 1024, 2 * r))
			lo = s;
		else
			hi = s;
	}
	int64_t nw = (w * lo / 1024) & ~1, nh = (h * lo / 1024) & ~1;

	if (nw < 64 || nh < 64)
		return;
	*x = (int)((*x + (w - nw) / 2) & ~1);
	*y = (int)((*y + (h - nh) / 2) & ~1);
	*cw = (int)nw;
	*ch = (int)nh;
}

/*
 * The lens calibration the ISP firmware returns for its distortion-data request (the camera driver's
 * .../apple-isp/<dev>/detect/lens_distortion), as seen in its replies: version 0xc306; the inverse distortion
 * polynomial = the 8 doubles at 0xc4 and the forward one at 0x44 (the lens-position terms at 0x104 / 0x84),
 * % over mm^2; the valid radius (mm) is the double at 0x14 and the sensor's pixel size (um) the double at 0x1bc.
 * The block at 0x880 describes the sensor config the reply is for: its size (u32 0x880 / 0x884) and the
 * distortion centre in its pixels (float 0x890 / 0x894); in the replies seen, that centre in 3040x2880 equals
 * the doubles at 0x24 / 0x2c (mm from the centre of the 3040x3024 sensor) - 1. The principal point is taken
 * on the distortion centre.
 */
struct isp_lens {
	float inv[8], fwd[8];
	double rmax_mm, pix_mm;
	double dcx, dcy;         /* distortion centre in the binned 1520x1440 input, pixels */
	const char *centre;      /* where the centre came from */
};

static double le_f64(const uint8_t *p)
{
	double v;

	memcpy(&v, p, sizeof(v));
	return v;
}

static const char *isp_lens_read(struct isp_lens *l)
{
	static uint8_t b[0x1118];
	glob_t g;
	int fd = -1;
	ssize_t n = 0;
	uint32_t ver, cw, ch;
	float cx, cy;

	if (!glob("/sys/bus/platform/drivers/apple-isp/*/detect/lens_distortion", 0, NULL, &g) && g.gl_pathc)
		fd = open(g.gl_pathv[0], O_RDONLY | O_CLOEXEC);
	globfree(&g);
	if (fd < 0)
		return "no lens_distortion node (old camera module)";
	n = pread(fd, b, sizeof(b), 0);
	close(fd);
	if (n != (ssize_t)sizeof(b))
		return "no calibration read (apple_isp lens_calib=0?)";
	memcpy(&ver, b + 0x10, sizeof(ver));
	if (ver != 0xc306)
		return "unknown calibration version";
	for (int i = 0; i < 8; i++) {
		l->inv[i] = (float)le_f64(b + 0xc4 + 8 * i);
		l->fwd[i] = (float)le_f64(b + 0x44 + 8 * i);
		if (le_f64(b + 0x104 + 8 * i) != 0 || le_f64(b + 0x84 + 8 * i) != 0)
			return "lens-position dependent calibration (not handled)";
	}
	l->pix_mm = le_f64(b + 0x1bc) / 1000;
	l->rmax_mm = le_f64(b + 0x14);
	if (!(l->pix_mm > 0.0002 && l->pix_mm < 0.005) || !(l->rmax_mm > 0.1 && l->rmax_mm < 10))
		return "calibration out of range";
	memcpy(&cw, b + 0x880, sizeof(cw));
	memcpy(&ch, b + 0x884, sizeof(ch));
	memcpy(&cx, b + 0x890, sizeof(cx));
	memcpy(&cy, b + 0x894, sizeof(cy));
	if (cw == 3040 && ch == 2880 && cx > 0 && cx < 3040 && cy > 0 && cy < 2880) {
		l->dcx = cx / 2;
		l->dcy = cy / 2;
		l->centre = "the reply's 3040x2880 config";
	} else if (cw == 1520 && ch == 1440 && cx > 0 && cx < 1520 && cy > 0 && cy < 1440) {
		l->dcx = cx;
		l->dcy = cy;
		l->centre = "the reply's 1520x1440 config";
	} else {
		/* the relation between the two in the replies seen: the 3040x3024 sensor's centre + offset - 1 */
		double ox = le_f64(b + 0x24) / l->pix_mm, oy = le_f64(b + 0x2c) / l->pix_mm;

		if (fabs(ox) > 400 || fabs(oy) > 400)
			return "calibration centre out of range";
		l->dcx = (1520 + ox - 1) / 2;
		l->dcy = (1512 + oy - 1) / 2;
		l->centre = "the offset from the sensor centre (config block unknown)";
	}
	return NULL;
}

static const char *stream_camera(int w, int h, float focal, struct cf_camera *c, const struct isp_lens *lens)
{
	static const struct {
		int w, h, x, y, cw, ch;
	} presets[] = {
		{ 1280, 720, 0, 292, 1520, 856 },    { 1920, 1080, 0, 292, 1520, 856 },
		{ 1440, 1080, 40, 180, 1440, 1080 }, { 1080, 1080, 220, 180, 1080, 1080 },
		{ 1552, 1552, 220, 180, 1080, 1080 }, { 1080, 1920, 458, 184, 604, 1072 },
		{ 1760, 1328, 44, 180, 1432, 1080 }, { 1328, 1760, 386, 224, 748, 992 },
		{ 960, 1280, 356, 180, 808, 1080 },
	};
	const double F = focal / 2.0, CX = 760, CY = 720;   /* binned: half the full-resolution focal length */
	int x, y, cw, ch;
	const char *how = "ISP preset in the lens circle";
	size_t i;

	for (i = 0; i < sizeof(presets) / sizeof(presets[0]); i++)
		if (presets[i].w == w && presets[i].h == h)
			break;
	if (i < sizeof(presets) / sizeof(presets[0])) {
		x = presets[i].x;
		y = presets[i].y;
		cw = presets[i].cw;
		ch = presets[i].ch;
	} else {
		how = "no ISP preset for this size: a centred crop in the lens circle";
		if ((double)w / h >= 1520.0 / 1440) {
			cw = 1520;
			ch = (int)(1520.0 * h / w) & ~1;
		} else {
			ch = 1440;
			cw = (int)(1440.0 * w / h) & ~1;
		}
		x = (1520 - cw) / 2 & ~1;
		y = (1440 - ch) / 2 & ~1;
	}
	fit_lens(&x, &y, &cw, &ch);
	double sx = (double)w / cw, sy = (double)h / ch;

	memset(c, 0, sizeof(*c));
	c->pix = 0.001; /* the pinhole's: zero coefficients make it irrelevant */
	c->fx = F * sx;
	c->fy = F * sy;
	c->cx = c->dcx = (CX - x) * sx;
	c->cy = c->dcy = (CY - y) * sy;
	c->width = w;
	c->height = h;
	c->radius = (int)lround(LENS_CIRCLE * 1520 / 1000.0 * sx);
	if (lens) {
		/* the stream is the 2x2-binned sensor (1520x1440), cropped and scaled */
		memcpy(c->inv, lens->inv, sizeof(c->inv));
		memcpy(c->fwd, lens->fwd, sizeof(c->fwd));
		c->pix = lens->pix_mm * 2 / sx;
		/* principal point = distortion centre */
		c->cx = c->dcx = (lens->dcx - x) * sx;
		c->cy = c->dcy = (lens->dcy - y) * sy;
		c->radius = (int)lround(lens->rmax_mm / lens->pix_mm / 2 * sx);
		how = "the ISP's lens calibration";
	}
	c->out_w = w;
	c->out_h = h;
	c->sensor_w = 2 * atan2(CX, F);
	return how;
}

static void *fx_create(const struct ce_config *cfg)
{
	struct fx *s = calloc(1, sizeof(*s));
	char dir[1024];
	int ret;

	if (!s)
		return NULL;
	s->isp_det_fd = -1;
	s->cfg = *cfg;
	s->seg.fd = -1;
	s->show_mask = getenv("CDN_EFFECTS_DEBUG") && strstr(getenv("CDN_EFFECTS_DEBUG"), "mask");
	s->stats = getenv("CDN_EFFECTS_DEBUG") && strstr(getenv("CDN_EFFECTS_DEBUG"), "stats");
	s->dump_dir = getenv("CDN_EFFECTS_DUMP") && *getenv("CDN_EFFECTS_DUMP") ? getenv("CDN_EFFECTS_DUMP") : NULL;
	snprintf(dir, sizeof(dir), "%s/segmentation", models_dir());
	ret = apple_prog_open(&s->seg, dir);
	if (ret) {
		fprintf(stderr, "camera-effects: apple: person segmentation (%s): %s\n", dir, strerror(-ret));
		goto fail;
	}
	/* the segmentation network: one RGB input, then the person and the skin planes, all 256x192 */
	s->seg_in = apple_prog_surface(&s->seg, 1, 0);
	s->seg_person = apple_prog_surface(&s->seg, 2, 0);
	s->seg_skin = apple_prog_surface(&s->seg, 2, 1);
	if (s->seg.count != 3 || apple_prog_layout(&s->seg, s->seg_in, 3, APPLE_SEG_H, APPLE_SEG_W) ||
	    apple_prog_layout(&s->seg, s->seg_person, 1, APPLE_SEG_H, APPLE_SEG_W) ||
	    apple_prog_layout(&s->seg, s->seg_skin, 1, APPLE_SEG_H, APPLE_SEG_W)) {
		fprintf(stderr, "camera-effects: apple: %s is not the segmentation network\n", dir);
		goto fail;
	}
	s->masks = calloc(1, sizeof(*s->masks));
	s->rgb = malloc(sizeof(float) * 3 * APPLE_SEG_W * APPLE_SEG_H);
	s->person = malloc(sizeof(float) * APPLE_SEG_W * APPLE_SEG_H);
	s->skin = malloc(sizeof(float) * APPLE_SEG_W * APPLE_SEG_H);
	s->portrait = apple_portrait_new((int)cfg->width, (int)cfg->height);
	if (!s->masks || !s->rgb || !s->person || !s->skin || !s->portrait)
		goto fail;
	/* the full-size passes on the GPU when there is a usable one, else on the CPU */
	s->gpu = apple_gpu_new((int)cfg->width, (int)cfg->height, ((int)cfg->width + 3) / 4, ((int)cfg->height + 3) / 4);
	fprintf(stderr, "camera-effects: apple: full-size passes (quarter sums, composite) on the %s%s%s\n",
		s->gpu ? "GPU (" : "CPU", s->gpu ? apple_gpu_name(s->gpu) : "", s->gpu ? ")" : " (no usable GPU)");
	s->cf_last_t = INT64_MIN;
	struct cf_tuning tuning;
	char terr[256];

	snprintf(dir, sizeof(dir), "%s/tuning/framing.conf", models_dir());
	if (cfg->framing && cf_tuning_read(&tuning, dir, terr, sizeof(terr)))
		fprintf(stderr, "camera-effects: apple: Center Stage off: no tuning (%s)\n", terr);
	else if (cfg->framing) {
		struct cf_camera cam;
		struct isp_lens lens;
		const char *lens_err = isp_lens_read(&lens);
		bool use_lens = !lens_err;
		const char *how = stream_camera((int)cfg->width, (int)cfg->height, tuning.focal_length, &cam,
						use_lens ? &lens : NULL);

		fprintf(stderr, "camera-effects: apple: Center Stage lens: %s", use_lens ? "ISP calibration" : lens_err);
		if (use_lens)
			fprintf(stderr, " (pixel %.6f mm, valid r %.4f mm, centre %.2f,%.2f binned from %s, inv c1 %.4f, fwd c1 %.4f)",
				lens.pix_mm, lens.rmax_mm, lens.dcx, lens.dcy, lens.centre, lens.inv[1], lens.fwd[1]);
		fprintf(stderr, "\n");
		const char *dbg = getenv("CDN_EFFECTS_DEBUG");

		s->cf = malloc(sizeof(*s->cf));
		s->warp_buf = malloc((size_t)cfg->width * cfg->height * 3 / 2);
		if (!s->cf || !s->warp_buf)
			goto fail;
		cf_init(s->cf, &cam, &tuning);
		{
			glob_t g;

			/* the ISP firmware's detector when the camera driver runs it; else (or while it is off) the masks */
			if (!glob("/sys/bus/platform/drivers/apple-isp/*/detect/detections", 0, NULL, &g) &&
			    g.gl_pathc)
				s->isp_det_fd = open(g.gl_pathv[0], O_RDONLY | O_CLOEXEC);
			fprintf(stderr, "camera-effects: apple: Center Stage detections: %s\n",
				s->isp_det_fd >= 0 ? "the ISP firmware's, when its detector runs" : "from the masks");
			globfree(&g);
		}
		fprintf(stderr, "camera-effects: apple: Center Stage: %ux%u (%s): f %.1f/%.1f c %.1f,%.1f lens r %d; "
			"frame %.3fx%.3f rad, home view %.3fx%.3f (%.2f of the sensor's %.3f)\n",
			cfg->width, cfg->height, how, cam.fx, cam.fy, cam.cx, cam.cy, cam.radius, s->cf->b[2], s->cf->b[3],
			s->cf->cur.w, s->cf->cur.h, tuning.default_fov, cam.sensor_w);
		if (dbg && strstr(dbg, "faces")) {
			char path[1100];
			struct timespec ts;

			clock_gettime(CLOCK_REALTIME, &ts);
			snprintf(path, sizeof(path), "%s/cf-faces-%lld.in", s->dump_dir ? s->dump_dir : "/tmp",
				 (long long)ts.tv_sec);
			s->facelog = fopen(path, "w");
			if (s->facelog)
				fprintf(s->facelog, "# camera %.4f %.4f %.4f %.4f %.0f %.0f %.4f %.4f %d %d %d %.6f\n", cam.fx, cam.fy,
					cam.cx, cam.cy, cam.width, cam.height, cam.dcx, cam.dcy, cam.radius, cam.out_w, cam.out_h,
					cam.sensor_w);
			/* the replay's CF_PIX / CF_ILENS / CF_LENS */
			if (s->facelog && use_lens) {
				fprintf(s->facelog, "# lens %.9g", cam.pix);
				for (int i = 0; i < 8; i++)
					fprintf(s->facelog, "%c%.9g", i ? ',' : ' ', cam.inv[i]);
				for (int i = 0; i < 8; i++)
					fprintf(s->facelog, "%c%.9g", i ? ',' : ' ', cam.fwd[i]);
				fprintf(s->facelog, "\n");
			}
			fprintf(stderr, "camera-effects: apple: Center Stage faces to %s%s\n", path, s->facelog ? "" : " (failed)");
		}
	}
	if (cfg->zoom > 1.0f)
		s->zoom = cdn_zoom_new((int)cfg->width, (int)cfg->height, CDN_ZOOM_AUTO);
	fprintf(stderr, "camera-effects: engine: apple (segmentation on the ANE): "
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
	float *planes[3] = { r, g, b };     /* the network takes R, G, B planes */
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
		"people %d\n",
		s->n_frames, s->n_seg, s->t_seg / n, s->t_refine / n, s->t_blur / n, s->t_light / n,
		s->t_comp / n, s->gpu ? "gpu" : "cpu", s->t_frame / n, sub->present ? "yes" : "no", sub->x0, sub->x1, sub->head_x, sub->head_top,
		sub->head_h, sub->people);
	if (s->cf)
		fprintf(stderr, "camera-effects: apple: center stage %.2f ms/frame: state %d, faces %d, bodies %d, "
			"viewport %.3f,%.3f %.3fx%.3f rad%s\n", s->t_cs / n, cf_state(s->cf), s->cf_faces, s->cf_bodies,
			s->cf_view.x, s->cf_view.y, s->cf_view.w, s->cf_view.h,
			s->isp_det_fd < 0 ? "" : s->isp_det_frames ? " (ISP detector)" : " (masks: ISP detector off)");
	s->isp_det_frames = 0;
	s->t_seg = s->t_refine = s->t_blur = s->t_light = s->t_comp = s->t_frame = s->t_cs = 0;
	s->n_frames = s->n_seg = 0;
	s->last_stats = pts;
}

/* Center Stage's detections: a body per person, a face when the mask shows one, ids kept by nearest body */
static int people(struct fx *s, int w, int h, struct cf_face *faces, int *nf, struct cf_face *bodies)
{
	struct apple_person pp[APPLE_MAX_PEOPLE];
	int n = apple_masks_people(s->masks, (float)w / h, pp, APPLE_MAX_PEOPLE);
	int used[APPLE_MAX_PEOPLE] = { 0 }, match[APPLE_MAX_PEOPLE];
	__typeof__(s->ppl[0]) next[APPLE_MAX_PEOPLE];

	memset(next, 0, sizeof(next));
	/* greedy nearest pairs, closest first, within a quarter of the frame */
	for (int i = 0; i < n; i++)
		match[i] = -1;
	for (;;) {
		float best = 0.25f * 0.25f;
		int bi = -1, bj = -1;

		for (int i = 0; i < n; i++) {
			float cx = pp[i].bx + pp[i].bw / 2, cy = pp[i].by + pp[i].bh / 2;

			for (int j = 0; j < s->nppl; j++) {
				float d = (cx - s->ppl[j].cx) * (cx - s->ppl[j].cx) + (cy - s->ppl[j].cy) * (cy - s->ppl[j].cy);

				if (match[i] < 0 && !used[j] && d < best) {
					best = d;
					bi = i;
					bj = j;
				}
			}
		}
		if (bi < 0)
			break;
		match[bi] = bj;
		used[bj] = 1;
	}
	*nf = 0;
	for (int i = 0; i < n; i++) {
		struct apple_person *p = &pp[i];

		next[i].cx = p->bx + p->bw / 2;
		next[i].cy = p->by + p->bh / 2;
		next[i].id = match[i] >= 0 ? s->ppl[match[i]].id : s->next_id++;
		next[i].face_age = match[i] >= 0 ? s->ppl[match[i]].face_age + 1 : 1000;
		if (match[i] >= 0) {
			next[i].rx = s->ppl[match[i]].rx;
			next[i].ry = s->ppl[match[i]].ry;
			next[i].rw = s->ppl[match[i]].rw;
			next[i].rh = s->ppl[match[i]].rh;
		}
		if (p->face) {
			next[i].face_age = 0;
			next[i].rx = (p->fx - p->bx) / p->bw;
			next[i].ry = (p->fy - p->by) / p->bh;
			next[i].rw = p->fw / p->bw;
			next[i].rh = p->fh / p->bh;
		}
		bodies[i] = (struct cf_face){ next[i].id, p->bx, p->by, p->bw, p->bh };
		/*
		 * A face the gate missed for a few frames follows its body: a face detector holds a face that
		 * steadily, and the framing session restarts a face's filter on every missing frame.
		 */
		if (next[i].face_age <= 10)
			faces[(*nf)++] = (struct cf_face){ next[i].id, p->bx + next[i].rx * p->bw, p->by + next[i].ry * p->bh,
							   next[i].rw * p->bw, next[i].rh * p->bh };
	}
	memcpy(s->ppl, next, sizeof(next[0]) * n);
	s->nppl = n;
	return n;
}

/*
 * The ISP firmware's faces and bodies (isp-detect.c's detect/detections): a header line, then one line per
 * object, rect in pixels of the ISP's output frame and in 1/1000 of it.  0: none this frame, -1: no
 * running detector, so the caller uses the masks.
 */
static int isp_people(struct fx *s, struct cf_face *faces, int *nf, struct cf_face *bodies, int *nb)
{
	char buf[4096], kind[8];
	unsigned long long seq;
	unsigned int level, fw, fh, count, raw;
	int running, err, off = 0, used;
	ssize_t len = pread(s->isp_det_fd, buf, sizeof(buf) - 1, 0);

	*nf = *nb = 0;
	if (len <= 0)
		return -1;
	buf[len] = 0;
	if (sscanf(buf, "seq %llu level %u running %d err %d frame %ux%u count %u raw_count %u%n", &seq, &level,
		   &running, &err, &fw, &fh, &count, &raw, &off) != 8 || !running)
		return -1;
	/* a count beyond the firmware's 32 = not the layout we parse; no new metadata for ~1 s = a stuck detector */
	if (raw > 32)
		return -1;
	if (seq == s->isp_det_seq) {
		if (++s->isp_det_stale > 30)
			return -1;
	} else {
		s->isp_det_stale = 0;
	}
	s->isp_det_seq = seq;
	for (char *line = strchr(buf, '\n'); line && *++line; line = strchr(line, '\n')) {
		int id, x, y;
		unsigned int conf, w, h, pm[4];
		struct cf_face o;

		if (sscanf(line, "%7s id %d conf %u rect %d %d %u %u pm %u %u %u %u%n", kind, &id, &conf, &x, &y, &w,
			   &h, &pm[0], &pm[1], &pm[2], &pm[3], &used) != 11)
			continue;
		/* pixels of the frame the ISP renders, which is this stream's frame; else the 1/1000 rect */
		if (fw && fh && w && h)
			o = (struct cf_face){ id < 0 ? 0 : id, (double)x / fw, (double)y / fh, (double)w / fw,
					      (double)h / fh };
		else
			o = (struct cf_face){ id < 0 ? 0 : id, pm[0] / 1000.0, pm[1] / 1000.0, pm[2] / 1000.0,
					      pm[3] / 1000.0 };
		if (o.w <= 0 || o.h <= 0 || o.w > 1.5 || o.h > 1.5)
			continue;
		if (!strcmp(kind, "face") && *nf < APPLE_MAX_PEOPLE)
			faces[(*nf)++] = o;
		else if (!strcmp(kind, "body") && *nb < APPLE_MAX_PEOPLE)
			bodies[(*nb)++] = o;
	}
	return *nf + *nb;
}

static void center_stage(struct fx *s, const uint8_t *in, struct cdn_frame *f, uint64_t pts, bool changed)
{
	int w = f->width, h = f->height, nf = 0, nb;
	struct cf_face faces[APPLE_MAX_PEOPLE], bodies[APPLE_MAX_PEOPLE];
	int64_t t = (int64_t)(pts / 1000);

	/* a stream restart or a long gap starts over, as a new session would */
	if (s->cf_last_t != INT64_MIN && (t < s->cf_last_t || t - s->cf_last_t > 2000000)) {
		cf_reset(s->cf);
		s->nppl = 0;
	}
	s->cf_last_t = t;
	if (s->isp_det_fd >= 0 && isp_people(s, faces, &nf, bodies, &nb) >= 0) {
		s->isp_det_frames++;
	} else {
		if (s->isp_det_fd >= 0 && !s->isp_det_fallbacks++)
			fprintf(stderr, "camera-effects: apple: ISP detector not running, stuck or unparsed: Center Stage on the masks\n");
		nb = people(s, w, h, faces, &nf, bodies);
	}
	s->cf_faces = nf;
	s->cf_bodies = nb;
	s->cf_view = cf_step(s->cf, t, faces, nf, bodies, nb);
	if (s->facelog) {
		fprintf(s->facelog, "%.6f %d", t / 1e6, nf);
		for (int i = 0; i < nf; i++)
			fprintf(s->facelog, " %.5f %.5f %.5f %.5f", faces[i].x, faces[i].y, faces[i].w, faces[i].h);
		fprintf(s->facelog, " ids");
		for (int i = 0; i < nf; i++)
			fprintf(s->facelog, " %d", faces[i].id);
		for (int i = 0; i < nb; i++)
			fprintf(s->facelog, " body %.5f %.5f %.5f %.5f %d", bodies[i].x, bodies[i].y, bodies[i].w, bodies[i].h,
				bodies[i].id);
		fprintf(s->facelog, "\n");
	}
	double Hm[9];
	struct cdn_frame src = { (uint8_t *)in, (uint8_t *)in + (size_t)w * h, w, h, w, w };

	cf_viewport_homography(s->cf, s->cf_view, w, h, Hm);
	if (changed || in == f->y) {
		memcpy(s->warp_buf, f->y, (size_t)w * h * 3 / 2);
		src.y = s->warp_buf;
		src.uv = s->warp_buf + (size_t)w * h;
	}
	apple_warp_nv12(&src, f, Hm, 0);
}

static int fx_process(void *opaque, const uint8_t *in, uint8_t *out, uint64_t pts)
{
	struct fx *s = opaque;
	int w = (int)s->cfg.width, h = (int)s->cfg.height;
	struct cdn_frame f = { out, out + (size_t)w * h, w, h, w, w };
	bool blur = s->cfg.background == 1, light = s->cfg.studio_light > 0;
	bool frame = s->cfg.framing && s->cf, need_masks = blur || light || frame;
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
	if (frame && s->cf) {
		center_stage(s, in, &f, pts, blur || lit);
		if (s->stats) {
			double t = now_ms();

			s->t_cs += t - t1;
			t1 = t;
		}
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
