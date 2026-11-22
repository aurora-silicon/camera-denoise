// SPDX-License-Identifier: MIT
/*
 * Portrait's passes (src/apple/sdof.c) against the same passes rendered on macOS: CDN_SDOF_REF names a folder
 * with their inputs and outputs, captured and named by pass. Each pass whose files are there is checked:
 *  - the ray pass: the 640x360 pyramid (4 levels, sRGB RGBA8), the 512x288 focus disparity (fp16) and the
 *    640x360 output (sRGB RGB + alpha);
 *  - the guided filter's three passes: guides (BGRA8 sRGB, 256x144 and 512x288), the 256x144 disparity, the
 *    coefficients (RGBA16F) and the 512x288 focus disparity; each pass alone and the three chained;
 *  - the pyramid: each of the ray pass's levels from the one above it; with the frame as frame.nv12
 *    (1280x720, the capture's input) also level 0 from the frame;
 *  - the composite: with frame.nv12 and out.nv12 (the renderer's output for the same last frame), the ray
 *    pass's output and the noise table (composite_tex1) give the output frame;
 *  - the temporal filter: flow, history and new disparity in (a multi-frame capture), the filtered disparity;
 *  - the whole first frame (a 1-frame capture, disparity_scale_tex0 = the network's disparity): frame + disparity
 *    in, output frame out (CDN_SDOF_FOCUS_OFFSET, default -1.5 = no face).
 * Captures are macOS's output for a picture of a person and stay local, so without the folder this skips. CDN_SDOF_F0 / _F1X / _F1Y /
 * _F2X / _F2Y: the ray pass's parameters as traced (default: aperture 0.5).
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/apple/sdof.h"

static void *load(const char *dir, const char *name, size_t want, int need)
{
	char path[4096];
	FILE *f;
	void *b = malloc(want);

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	f = fopen(path, "rb");
	if (!f || fread(b, 1, want, f) != want) {
		if (f)
			fclose(f);
		free(b);
		if (need) {
			fprintf(stderr, "read %s failed\n", path);
			exit(1);
		}
		return NULL;
	}
	fclose(f);
	return b;
}

static void *slurp(const char *dir, const char *name, size_t want)
{
	return load(dir, name, want, 1);
}

static float half_to_float(uint16_t h)
{
	uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
	float v = e ? ldexpf(1.0f + m / 1024.0f, (int)e - 15) : ldexpf(m / 1024.0f, -14);

	return s ? -v : v;
}

static float env(const char *n, float d)
{
	return getenv(n) ? (float)atof(getenv(n)) : d;
}

static int access_ok(const char *dir, const char *name)
{
	char path[4096];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	f = fopen(path, "rb");
	if (!f)
		return -1;
	fclose(f);
	return 0;
}

static float *fp16_file(const char *dir, const char *name, size_t n)
{
	uint16_t *h = slurp(dir, name, n * 2);
	float *f = malloc(n * sizeof(float));

	for (size_t i = 0; i < n; i++)
		f[i] = half_to_float(h[i]);
	free(h);
	return f;
}

/* A BGRA8 sRGB texture as linear RGBA. */
static float *bgra_srgb_file(const char *dir, const char *name, int w, int h)
{
	uint8_t *b = slurp(dir, name, (size_t)w * h * 4);
	float *f = malloc((size_t)w * h * 4 * sizeof(float));

	for (int i = 0; i < w * h; i++) {
		f[i * 4 + 0] = sdof_srgb_to_linear(b[i * 4 + 2] / 255.0f);
		f[i * 4 + 1] = sdof_srgb_to_linear(b[i * 4 + 1] / 255.0f);
		f[i * 4 + 2] = sdof_srgb_to_linear(b[i * 4 + 0] / 255.0f);
		f[i * 4 + 3] = b[i * 4 + 3] / 255.0f;
	}
	free(b);
	return f;
}

/* fp16 maps: the share of values within tol (absolute, plus 1/512 relative = 2 fp16 ULP), and the worst. */
static int compare(const char *what, const float *ours, const float *ref, size_t n, float tol)
{
	size_t ok = 0;
	double worst = 0, sum = 0;

	for (size_t i = 0; i < n; i++) {
		double d = fabs((double)ours[i] - ref[i]);

		ok += d <= tol + fabs(ref[i]) / 512.0;
		worst = d > worst ? d : worst;
		sum += d;
	}
	printf("%s vs macOS: %.2f%% within %g (+2 ULP), worst %.4g, mean |diff| %.3g\n", what, 100.0 * ok / n, tol,
	       worst, sum / n);
	return ok >= n * 99 / 100 ? 0 : 1;
}

static int check_ray(const char *dir)
{
	struct sdof_level lv[4];
	struct sdof_params p;
	int ow = 640, oh = 360, dw = 512, dh = 288, ok1 = 0, ok2 = 0, worst = 0, aok = 0;
	double err = 0;

	if (access_ok(dir, "ray_tex3_mip0_640x360_fmt71.bin") < 0)
		return -1;
	for (int l = 0; l < 4; l++) {
		char name[128];
		int w = ow >> l, h = oh >> l;
		uint8_t *b;
		float *f = malloc((size_t)w * h * 4 * sizeof(float));

		snprintf(name, sizeof(name), "ray_tex0_mip%d_%dx%d_fmt71.bin", l, w, h);
		b = slurp(dir, name, (size_t)w * h * 4);
		for (int i = 0; i < w * h * 4; i++)
			f[i] = i % 4 == 3 ? b[i] / 255.0f : sdof_srgb_to_linear(b[i] / 255.0f);
		lv[l] = (struct sdof_level) { w, h, f };
		free(b);
	}
	uint16_t *dh16 = slurp(dir, "ray_tex1_mip0_512x288_fmt25.bin", (size_t)dw * dh * 2);
	float *disp = malloc((size_t)dw * dh * sizeof(float));

	for (int i = 0; i < dw * dh; i++)
		disp[i] = half_to_float(dh16[i]);
	uint8_t *ref = slurp(dir, "ray_tex3_mip0_640x360_fmt71.bin", (size_t)ow * oh * 4);
	float *out = malloc((size_t)ow * oh * 4 * sizeof(float));

	sdof_params_for(&p, 0.5f, 1280);
	p.f0 = env("CDN_SDOF_F0", 0.00664647f);
	p.f1x = env("CDN_SDOF_F1X", 0.00553873f);
	p.f1y = env("CDN_SDOF_F1Y", 0.0141791f);
	p.f2x = env("CDN_SDOF_F2X", -0.375309f);
	p.f2y = env("CDN_SDOF_F2Y", -0.211111f);
	sdof_raytrace(lv, 4, disp, dw, dh, &p, out, ow, oh);
	if (getenv("CDN_SDOF_OUT")) {
		FILE *f = fopen(getenv("CDN_SDOF_OUT"), "wb");

		if (f) {
			fwrite(out, sizeof(float), (size_t)ow * oh * 4, f);
			fclose(f);
		}
	}
	for (int i = 0; i < ow * oh; i++) {
		int pw = 0;

		for (int c = 0; c < 3; c++) {
			int v = (int)lrintf(sdof_linear_to_srgb(out[i * 4 + c]) * 255.0f), d = abs(v - ref[i * 4 + c]);

			pw = d > pw ? d : pw;
			err += d;
		}
		int a = (int)lrintf(out[i * 4 + 3] * 255.0f);

		aok += abs(a - ref[i * 4 + 3]) <= 1;
		ok1 += pw <= 1;
		ok2 += pw <= 2;
		worst = pw > worst ? pw : worst;
	}
	printf("sdof vs macOS: %.2f%% of pixels within 1 LSB, %.2f%% within 2, worst %d, mean |diff| %.3f; "
	       "alpha within 1: %.2f%%\n", 100.0 * ok1 / (ow * oh), 100.0 * ok2 / (ow * oh), worst,
	       err / (3.0 * ow * oh), 100.0 * aok / (ow * oh));
	return ok2 >= ow * oh * 99 / 100 && aok >= ow * oh * 99 / 100 ? 0 : 1;
}

static int check_guided(const char *dir)
{
	const char *C = "guide_coefs", *A = "guide_average",
		   *P = "guide_apply";
	char n[256];
	int lw = 256, lh = 144, w = 512, hh = 288, bad = 0;

	snprintf(n, sizeof(n), "%s_tex2_mip0_%dx%d_fmt115.bin", C, lw, lh);
	if (access_ok(dir, n) < 0)
		return -1;
	snprintf(n, sizeof(n), "%s_tex1_mip0_%dx%d_fmt115.bin", A, lw, lh);
	if (access_ok(dir, n) < 0)
		return -1;
	snprintf(n, sizeof(n), "%s_tex0_mip0_%dx%d_fmt81.bin", C, lw, lh);
	float *glo = bgra_srgb_file(dir, n, lw, lh);
	snprintf(n, sizeof(n), "%s_tex1_mip0_%dx%d_fmt25.bin", C, lw, lh);
	float *p = fp16_file(dir, n, (size_t)lw * lh);
	snprintf(n, sizeof(n), "%s_tex2_mip0_%dx%d_fmt115.bin", C, lw, lh);
	float *coef_ref = fp16_file(dir, n, (size_t)lw * lh * 4);
	snprintf(n, sizeof(n), "%s_tex1_mip0_%dx%d_fmt115.bin", A, lw, lh);
	float *avg_ref = fp16_file(dir, n, (size_t)lw * lh * 4);
	snprintf(n, sizeof(n), "%s_tex0_mip0_%dx%d_fmt81.bin", P, w, hh);
	float *ghi = bgra_srgb_file(dir, n, w, hh);
	snprintf(n, sizeof(n), "%s_tex2_mip0_%dx%d_fmt25.bin", P, w, hh);
	float *out_ref = fp16_file(dir, n, (size_t)w * hh);
	float *coef = malloc((size_t)lw * lh * 4 * sizeof(float)), *avg = malloc((size_t)lw * lh * 4 * sizeof(float));
	float *out = malloc((size_t)w * hh * sizeof(float));
	float off = env("CDN_SDOF_OFFSET", -1.5f);

	sdof_guided_coefs(glo, p, lw, lh, 0.05f, coef);
	bad |= compare("guided coefficients", coef, coef_ref, (size_t)lw * lh * 4, 1e-3f);
	sdof_guided_average(coef_ref, lw, lh, avg);
	bad |= compare("guided average (macOS's coefficients in)", avg, avg_ref, (size_t)lw * lh * 4, 1e-3f);
	sdof_guided_apply(avg_ref, lw, lh, ghi, w, hh, off, -3.4028235e38f, 0, out);
	bad |= compare("guided apply (macOS's average in)", out, out_ref, (size_t)w * hh, 1e-3f);
	sdof_guided_average(coef, lw, lh, avg);
	sdof_guided_apply(avg, lw, lh, ghi, w, hh, off, -3.4028235e38f, 0, out);
	bad |= compare("guided filter chained (guides + disparity in)", out, out_ref, (size_t)w * hh, 2e-3f);
	free(glo); free(p); free(coef_ref); free(avg_ref); free(ghi); free(out_ref); free(coef); free(avg); free(out);
	return bad;
}

/* sRGB RGBA8 levels: the share of pixels within 1 LSB in every channel, the worst. */
static int compare8(const char *what, const float *ours, const uint8_t *ref, int n)
{
	int ok = 0, worst = 0;

	for (int i = 0; i < n; i++) {
		int pw = 0;

		for (int c = 0; c < 3; c++) {
			int d = abs((int)lrintf(sdof_linear_to_srgb(ours[i * 4 + c]) * 255.0f) - ref[i * 4 + c]);

			pw = d > pw ? d : pw;
		}
		ok += pw <= 1;
		worst = pw > worst ? pw : worst;
	}
	printf("%s vs macOS: %.2f%% of pixels within 1 LSB, worst %d\n", what, 100.0 * ok / n, worst);
	return ok >= n * 99 / 100 ? 0 : 1;
}

static int check_pyramid(const char *dir)
{
	char n[256];
	int bad = 0, w = 640, h = 360;
	uint8_t *lv[4];

	snprintf(n, sizeof(n), "ray_tex0_mip3_80x45_fmt71.bin");
	if (access_ok(dir, n) < 0)
		return -1;
	for (int l = 0; l < 4; l++) {
		snprintf(n, sizeof(n), "ray_tex0_mip%d_%dx%d_fmt71.bin", l, w >> l, h >> l);
		lv[l] = slurp(dir, n, (size_t)(w >> l) * (h >> l) * 4);
	}
	uint8_t *nv12 = load(dir, "frame.nv12", 1280 * 720 * 3 / 2, 0);

	if (nv12) {
		struct sdof_yuv m = { 0.299f, 0.114f, 0, 0 };
		float *o = malloc((size_t)w * h * 4 * sizeof(float));

		sdof_level0_nv12(nv12, nv12 + 1280 * 720, 1280, 720, 1280, &m, o);
		bad |= compare8("pyramid level 0 (from the frame)", o, lv[0], w * h);
		free(o);
		free(nv12);
	}
	for (int l = 0; l < 3; l++) {
		int sw = w >> l, sh = h >> l, dw = w >> (l + 1), dh = h >> (l + 1);
		float *s = malloc((size_t)sw * sh * 4 * sizeof(float)), *o = malloc((size_t)dw * dh * 4 * sizeof(float));

		for (int i = 0; i < sw * sh * 4; i++)
			s[i] = i % 4 == 3 ? 1 : sdof_srgb_to_linear(lv[l][i] / 255.0f);
		sdof_downscale_gauss3(s, sw, sh, o, dw, dh);
		snprintf(n, sizeof(n), "pyramid level %d (from macOS's level %d)", l + 1, l);
		bad |= compare8(n, o, lv[l + 1], dw * dh);
		free(s);
		free(o);
	}
	for (int l = 0; l < 4; l++)
		free(lv[l]);
	return bad;
}

static int check_composite(const char *dir)
{
	int w = 1280, h = 720, rw = 640, rh = 360, n = w * h * 3 / 2, ok1 = 0, okc = 0, worst = 0;
	uint8_t *in = load(dir, "frame.nv12", (size_t)n, 0), *ref = load(dir, "out.nv12", (size_t)n, 0);
	const char *R = "composite_tex0_mip0_640x360_fmt71.bin";

	if (!in || !ref || access_ok(dir, R) < 0) {
		free(in);
		free(ref);
		return -1;
	}
	uint8_t *r8 = slurp(dir, R, (size_t)rw * rh * 4), *out = malloc((size_t)n);
	float *ray = malloc((size_t)rw * rh * 4 * sizeof(float));
	float *lut = fp16_file(dir, "composite_tex1_mip0_256x256_fmt25.bin", 256 * 256);
	struct sdof_yuv m = { 0.299f, 0.114f, 0, 0 };
	struct sdof_grain g = { lut, 256, 3.0f, 8.188798e-11f, 3.05274e-06f, 2732, 10799 };

	for (int i = 0; i < rw * rh * 4; i++)
		ray[i] = i % 4 == 3 ? r8[i] / 255.0f : sdof_srgb_to_linear(r8[i] / 255.0f);
	sdof_composite_nv12(ray, rw, rh, in, in + w * h, w, h, w, &m, &g, out, out + w * h);
	for (int i = 0; i < w * h; i++) {
		int d = abs(out[i] - ref[i]);

		ok1 += d <= 1;
		worst = d > worst ? d : worst;
	}
	for (int i = w * h; i < n; i++)
		okc += abs(out[i] - ref[i]) <= 1;
	printf("composite vs macOS: luma %.2f%% within 1 LSB (worst %d), chroma %.2f%% within 1\n", 100.0 * ok1 / (w * h),
	       worst, 100.0 * okc / (w * h / 2));
	if (getenv("CDN_SDOF_OUT_NV12")) {
		FILE *f = fopen(getenv("CDN_SDOF_OUT_NV12"), "wb");

		if (f) {
			fwrite(out, 1, (size_t)n, f);
			fclose(f);
		}
	}
	free(in); free(ref); free(r8); free(out); free(ray); free(lut);
	return ok1 >= w * h * 99 / 100 && okc >= w * h / 2 * 98 / 100 ? 0 : 1;
}

static int check_temporal(const char *dir)
{
	const char *T = "temporal";
	char n[256];
	int w = 256, h = 144, fw = 128, fh = 72, bad = 0;

	snprintf(n, sizeof(n), "%s_tex0_mip0_%dx%d_fmt65.bin", T, fw, fh);
	if (access_ok(dir, n) < 0)
		return -1;
	float *flow = fp16_file(dir, n, (size_t)fw * fh * 2);
	snprintf(n, sizeof(n), "%s_tex1_mip0_%dx%d_fmt25.bin", T, w, h);
	float *prev = fp16_file(dir, n, (size_t)w * h);
	snprintf(n, sizeof(n), "%s_tex2_mip0_%dx%d_fmt25.bin", T, w, h);
	float *cur = fp16_file(dir, n, (size_t)w * h);
	snprintf(n, sizeof(n), "%s_tex3_mip0_%dx%d_fmt25.bin", T, w, h);
	float *ref = fp16_file(dir, n, (size_t)w * h), *out = malloc((size_t)w * h * sizeof(float));
	double fm = 0;

	for (int i = 0; i < fw * fh; i++)
		fm += sqrt(flow[2 * i] * flow[2 * i] + flow[2 * i + 1] * flow[2 * i + 1]);
	printf("temporal: mean |flow| %.3f flow pixels\n", fm / (fw * fh));
	sdof_temporal_ema(flow, fw, fh, prev, cur, w, h, out);
	bad |= compare("temporal filter", out, ref, (size_t)w * h, 1e-3f);
	free(flow); free(prev); free(cur); free(ref); free(out);
	return bad;
}

static int check_render(const char *dir)
{
	int w = 1280, h = 720, n = w * h * 3 / 2, ok1 = 0, okc = 0, worst = 0, ok3 = 0;
	uint8_t *in = load(dir, "frame.nv12", (size_t)n, 0), *ref = load(dir, "out.nv12", (size_t)n, 0);
	const char *D = "disparity_scale_tex0_mip0_256x144_fmt25.bin";

	if (!in || !ref || access_ok(dir, D) < 0 ||
	    access_ok(dir, "temporal_tex1_mip0_256x144_fmt25.bin") == 0) {
		/* a multi-frame capture: its last frame went through the temporal filter */
		free(in);
		free(ref);
		return -1;
	}
	float *disp = fp16_file(dir, D, 256 * 144);
	float *lut = fp16_file(dir, "composite_tex1_mip0_256x256_fmt25.bin", 256 * 256);
	uint8_t *out = malloc((size_t)n);

	if (getenv("CDN_SDOF_OWN_GRAIN"))       /* our table instead of macOS's */
		sdof_grain_table(lut, 256, 1);
	struct sdof_yuv m = { 0.299f, 0.114f, 0, 0 };
	struct sdof_grain g = { lut, 256, 3.0f, 8.188798e-11f, 3.05274e-06f, 2732, 10799 };

	if (sdof_render_nv12(in, in + w * h, w, h, w, disp, 256, 144, 0.5f, env("CDN_SDOF_FOCUS_OFFSET", -1.5f), &m, &g,
			     out, out + w * h))
		return 1;
	for (int i = 0; i < w * h; i++) {
		int d = abs(out[i] - ref[i]);

		ok1 += d <= 1;
		ok3 += d <= 3;
		worst = d > worst ? d : worst;
	}
	for (int i = w * h; i < n; i++)
		okc += abs(out[i] - ref[i]) <= 1;
	printf("whole frame vs macOS: luma %.2f%% within 1 LSB, %.2f%% within 3 (worst %d), chroma %.2f%% within 1\n",
	       100.0 * ok1 / (w * h), 100.0 * ok3 / (w * h), worst, 100.0 * okc / (w * h / 2));
	if (getenv("CDN_SDOF_OUT_NV12")) {
		FILE *f = fopen(getenv("CDN_SDOF_OUT_NV12"), "wb");

		if (f) {
			fwrite(out, 1, (size_t)n, f);
			fclose(f);
		}
	}
	free(in); free(ref); free(disp); free(lut); free(out);
	return ok3 >= w * h * 99 / 100 ? 0 : 1;
}

int main(void)
{
	const char *dir = getenv("CDN_SDOF_REF");
	int r, bad = 0, ran = 0;

	if (!dir) {
		printf("SKIP: CDN_SDOF_REF not set\n");
		return 77;
	}
	if ((r = check_guided(dir)) >= 0) { bad |= r; ran++; }
	if ((r = check_pyramid(dir)) >= 0) { bad |= r; ran++; }
	if ((r = check_temporal(dir)) >= 0) { bad |= r; ran++; }
	if ((r = check_render(dir)) >= 0) { bad |= r; ran++; }
	if ((r = check_composite(dir)) >= 0) { bad |= r; ran++; }
	if ((r = check_ray(dir)) >= 0) { bad |= r; ran++; }
	if (!ran) {
		printf("SKIP: no captured pass in %s\n", dir);
		return 77;
	}
	return bad;
}
