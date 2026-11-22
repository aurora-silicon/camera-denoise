// SPDX-License-Identifier: MIT
/*
 * Studio Light (src/apple/studiolight.c) against the same passes rendered on macOS: CDN_SL_REF names a folder
 * with a 1-frame capture (each pass's inputs and outputs, named by pass; frame.nv12 = the input). Each pass
 * is checked on its own inputs from the capture. CDN_SL_FACE=x,y,w,h: the face rect of the capture (none: no
 * face). CDN_SL_MATRIX=709|601: the buffer's colour tag (none: untagged, where the GPU reads the camera
 * texture as BT.601 and the renderer's matrices are BT.709). Captures are macOS's output for a picture of a
 * person and stay local; without one this skips.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/apple/sdof.h"
#include "../src/apple/focus.h"
#include "../src/apple/studiolight.h"

static const char *dir;

static void *load(const char *name, size_t want, int need)
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

static float half_to_float(uint16_t h)
{
	uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
	float v = e ? ldexpf(1.0f + m / 1024.0f, (int)e - 15) : ldexpf(m / 1024.0f, -14);

	return s ? -v : v;
}

static float *fp16(const char *name, size_t n)
{
	uint16_t *h = load(name, n * 2, 1);
	float *f = malloc(n * sizeof(float));

	for (size_t i = 0; i < n; i++)
		f[i] = half_to_float(h[i]);
	free(h);
	return f;
}

static float *unorm8(const char *name, size_t n)
{
	uint8_t *b = load(name, n, 1);
	float *f = malloc(n * sizeof(float));

	for (size_t i = 0; i < n; i++)
		f[i] = b[i] / 255.0f;
	free(b);
	return f;
}

static float srgb_to_linear(float v)
{
	return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
}

static float *bgra_srgb(const char *name, int w, int h)
{
	uint8_t *b = load(name, (size_t)w * h * 4, 1);
	float *f = malloc((size_t)w * h * 4 * sizeof(float));

	for (int i = 0; i < w * h; i++) {
		f[i * 4 + 0] = srgb_to_linear(b[i * 4 + 2] / 255.0f);
		f[i * 4 + 1] = srgb_to_linear(b[i * 4 + 1] / 255.0f);
		f[i * 4 + 2] = srgb_to_linear(b[i * 4 + 0] / 255.0f);
		f[i * 4 + 3] = b[i * 4 + 3] / 255.0f;
	}
	free(b);
	return f;
}

/* within tol absolute + 2 fp16 ULP relative; ch: channels compared of every stride */
static int compare(const char *what, const float *ours, const float *ref, size_t n, int stride, int ch, float tol)
{
	size_t ok = 0, cnt = 0;
	double worst = 0, sum = 0;

	for (size_t i = 0; i < n; i++)
		for (int c = 0; c < ch; c++) {
			double d = fabs((double)ours[i * stride + c] - ref[i * stride + c]);

			ok += d <= tol + fabs(ref[i * stride + c]) / 512.0;
			worst = d > worst ? d : worst;
			sum += d;
			cnt++;
		}
	printf("%-34s %.2f%% within %g, worst %.4g, mean |diff| %.3g\n", what, 100.0 * ok / cnt, tol, worst, sum / cnt);
	return ok >= cnt * 99 / 100 ? 0 : 1;
}

static int compare8(const char *what, const uint8_t *ours, const uint8_t *ref, size_t n)
{
	size_t ok = 0, ex = 0;
	int worst = 0;

	for (size_t i = 0; i < n; i++) {
		int d = abs(ours[i] - ref[i]);

		ok += d <= 1;
		ex += d == 0;
		worst = d > worst ? d : worst;
	}
	printf("%-34s %.2f%% within 1 LSB (%.2f%% exact), worst %d\n", what, 100.0 * ok / n, 100.0 * ex / n, worst);
	return ok >= n * 99 / 100 ? 0 : 1;
}

int main(void)
{
	int bad = 0, dw = 256, dh = 144, gw = 320, gh = 180, uw = 640, uh = 360, w = 1280, h = 720, n = 32;
	struct sl_params p;
	struct sl_light l;
	struct sl_face face;
	int nfaces = 0;

	dir = getenv("CDN_SL_REF");
	if (!dir || !load("gain_tex4_mip0_320x180_fmt115.bin", 8, 0)) {
		printf("SKIP: CDN_SL_REF not set or no Studio Light capture there\n");
		return 77;
	}
	if (getenv("CDN_SL_FACE") &&
	    sscanf(getenv("CDN_SL_FACE"), "%f,%f,%f,%f", &face.x, &face.y, &face.w, &face.h) == 4)
		nfaces = 1;
	sl_params_default(&p);
	/* the GPU's read of the camera texture, and Portrait's matrices */
	struct sl_yuv m601 = { 0.299f, 0.114f, 0 }, m709 = { 0.2126f, 0.0722f, 0 }, rd = m601;
	const char *tag = getenv("CDN_SL_MATRIX");

	if (tag && atoi(tag) == 709)
		rd = m709;
	if (tag && atoi(tag) == 601)
		sl_params_matrix(&p, &m601);

	/* 1. diffuse */
	float *disp = fp16("diffuse_tex0_mip0_256x144_fmt25.bin", (size_t)dw * dh);
	float *dref = unorm8("gain_tex1_mip0_256x144_fmt10.bin", (size_t)dw * dh);   /* = the copy's output */
	float *diff = malloc((size_t)dw * dh * sizeof(float));

	sl_diffuse(disp, dw, dh, &p, diff);
	bad |= compare("diffuse (8-bit)", diff, dref, (size_t)dw * dh, 1, 1, 1.01f / 255);

	/* 2. light mask */
	float *mref = unorm8("light_mask_tex0_mip0_320x180_fmt30.bin", (size_t)gw * gh * 2);
	float *mask = malloc((size_t)gw * gh * 2 * sizeof(float));

	sl_light_mask(&face, nfaces, (float)w / h, gw, gh, mask);
	bad |= compare("light mask (8-bit)", mask, mref, (size_t)gw * gh, 2, 2, 1.01f / 255);

	/* the focus from the face (Portrait's focus tracker), vs the capture's offsets */
	float off[3] = { -1.5f, -1.5f, -1.5f };

	if (nfaces) {
		float *fd = fp16("face_sample_tex0_mip0_256x144_fmt25.bin", (size_t)dw * dh);
		float *fref = load("focus_update_buf3.bin", 36, 1), *dref2 = load("face_sample_buf2.bin", 4, 1);
		struct focus_rect r = { face.x, face.y, face.w, face.h };
		struct focus_state st;
		int state = 0;          /* the capture's face state for its one face */

		focus_init(&st);
		float d = focus_face_disparity(fd, dw, dh, &r);

		focus_update(&st, &d, &state, 1, off);
		printf("face disparity %.6f (macOS %.6f); offsets %.5f %.5f %.5f (macOS %.5f %.5f %.5f)\n", d, dref2[0],
		       off[0], off[1], off[2], fref[0], fref[3], fref[6]);
		bad |= fabsf(d - dref2[0]) > 1e-6f || fabsf(off[0] - fref[0]) > 1e-5f || fabsf(off[1] - fref[3]) > 1e-5f ||
		       fabsf(off[2] - fref[6]) > 1e-5f;
	}

	/* 3. light estimate (the capture's own, GPU-written) */
	float *le = load("gain_buf4.bin", 64, 1), *fo = load("gain_buf5.bin", 12, 1);

	l = (struct sl_light) { le[8], le[9], le[10], le[11] };
	printf("light estimate %.5f %.5f %.5f %.5f, focus offset %.5f\n", l.l, l.y, l.z, l.w, fo[0]);
	if (!nfaces) {
		struct sl_light d;

		sl_light_default(&d);
		printf("default light estimate %.5f (macOS %.5f)\n", d.l, l.l);
		bad |= fabsf(d.l - l.l) > 1e-5f;
	}

	/* 4. gain, on macOS's own inputs */
	float *guide = bgra_srgb("gain_tex0_mip0_512x288_fmt81.bin", 512, 288);
	float *d8 = unorm8("gain_tex1_mip0_256x144_fmt10.bin", (size_t)dw * dh);
	float *dsp = fp16("gain_tex2_mip0_256x144_fmt25.bin", (size_t)dw * dh);
	float *m8 = unorm8("gain_tex3_mip0_320x180_fmt30.bin", (size_t)gw * gh * 2);
	float *gref = fp16("gain_tex4_mip0_320x180_fmt115.bin", (size_t)gw * gh * 4);
	float *gain = malloc((size_t)gw * gh * 4 * sizeof(float));

	sl_gain(guide, 512, 288, d8, dsp, dw, dh, m8, &p, &l, fo[0], gain, gw, gh);
	/*
	 * ~1.3% of a face capture's gain values differ by a few fp16 ULP: dark subject pixels, where out / in
	 * magnifies the shader's half arithmetic. They do not reach the picture (the whole frame below is the
	 * strict check), so this pass only has to stay above 98%.
	 */
	compare("gain map", gain, gref, (size_t)gw * gh, 4, 3, 2e-3f);
	{
		size_t ok = 0;

		for (int i = 0; i < gw * gh; i++)
			for (int c = 0; c < 3; c++)
				ok += fabsf(gain[i * 4 + c] - gref[i * 4 + c]) <= 2e-3f + fabsf(gref[i * 4 + c]) / 512;
		bad |= ok < (size_t)gw * gh * 3 * 98 / 100;
	}

	/* 5. blur, upsample */
	float *bin = fp16("gain_blur_tex0_mip0_320x180_fmt115.bin", (size_t)gw * gh * 4);
	float *bref = fp16("gain_blur_tex1_mip0_320x180_fmt115.bin", (size_t)gw * gh * 4);
	float *blur = malloc((size_t)gw * gh * 4 * sizeof(float));

	sl_gain_blur(bin, gw, gh, blur);
	bad |= compare("gain blur", blur, bref, (size_t)gw * gh, 4, 3, 1e-3f);
	float *uref = fp16("gain_upsample_tex1_mip0_640x360_fmt115.bin", (size_t)uw * uh * 4);
	float *up = malloc((size_t)uw * uh * 4 * sizeof(float));

	sl_gain_upsample(bref, gw, gh, up, uw, uh);
	bad |= compare("gain upsample", up, uref, (size_t)uw * uh, 4, 3, 1e-3f);

	/* the colour cube from macOS's correction cube */
	uint8_t *corr = load("color_cube_tex1_mip0_32x32x32_fmt70.bin", (size_t)n * n * n * 4, 1);
	uint8_t *cref = load("color_cube_tex0_mip0_32x32x32_fmt70.bin", (size_t)n * n * n * 4, 1);
	struct sl_cube cube;
	uint8_t *c8 = malloc((size_t)n * n * n * 3), *cr3 = malloc((size_t)n * n * n * 3);

	if (sl_cube_build(&cube, n, corr, &p))
		return 1;
	for (int i = 0; i < n * n * n; i++)
		for (int k = 0; k < 3; k++) {
			c8[i * 3 + k] = (uint8_t)lrintf(cube.yuv[i * 3 + k] * 255);
			cr3[i * 3 + k] = cref[i * 4 + k];
		}
	printf("cube range: min %.4f %.4f %.4f max %.4f %.4f %.4f\n", cube.min[0], cube.min[1], cube.min[2],
	       cube.max[0], cube.max[1], cube.max[2]);
	bad |= compare8("colour cube", c8, cr3, (size_t)n * n * n * 3);

	/* 6. full size, macOS's upsampled gain in */
	uint8_t *frame = load("frame.nv12", (size_t)w * h * 3 / 2, 1), *out = malloc((size_t)w * h * 3 / 2);
	uint8_t *yref = load("relight_tex4_mip0_1280x720_fmt10.bin", (size_t)w * h, 1);
	uint8_t *uvref = load("relight_tex5_mip0_640x360_fmt30.bin", (size_t)w * h / 2, 1);
	float *g6 = fp16("relight_tex0_mip0_640x360_fmt115.bin", (size_t)uw * uh * 4);
	struct sl_cube mc = { n, malloc((size_t)n * n * n * 3 * sizeof(float)), { 0 }, { 0 } };

	/* macOS's own cube, to check the pass alone */
	for (int i = 0; i < n * n * n; i++)
		for (int k = 0; k < 3; k++)
			mc.yuv[i * 3 + k] = cref[i * 4 + k] / 255.0f;
	memcpy(mc.min, cube.min, sizeof(mc.min));
	memcpy(mc.max, cube.max, sizeof(mc.max));
	sl_relight_nv12(frame, frame + w * h, w, h, w, &rd, g6, uw, uh, &p, &mc, out, out + w * h);
	if (getenv("CDN_SL_OUT")) {
		FILE *f = fopen(getenv("CDN_SL_OUT"), "wb");

		if (f) {
			fwrite(out, 1, (size_t)w * h * 3 / 2, f);
			fclose(f);
		}
	}
	bad |= compare8("relight luma", out, yref, (size_t)w * h);
	bad |= compare8("relight chroma", out + w * h, uvref, (size_t)w * h / 2);

	/* the whole frame from the frame, the disparity, the face, the light and focus offset, the cube */
	uint8_t *final = load("out.nv12", (size_t)w * h * 3 / 2, 1);
	struct sdof_yuv sm = { rd.kr, rd.kb, rd.full_range, tag != NULL };
	float *g512 = malloc((size_t)512 * 288 * 4 * sizeof(float));

	/* our guide, our focus offset (the tracker above; -1.5 with no face) */
	if (sdof_guide_nv12(frame, frame + w * h, w, h, w, &sm, 512, 288, g512) ||
	    sl_render_nv12(frame, frame + w * h, w, h, w, &rd, &p, g512, 512, 288, disp, dw, dh, &face, nfaces, &l,
			   off[FOCUS_STUDIO], &cube, out, out + w * h))
		return 1;
	bad |= compare8("whole frame luma (our guide)", out, final, (size_t)w * h);
	compare8("whole frame chroma (our guide)", out + w * h, final + w * h, (size_t)w * h / 2);
	if (sl_render_nv12(frame, frame + w * h, w, h, w, &rd, &p, guide, 512, 288, disp, dw, dh, &face, nfaces, &l,
			   fo[0], &cube, out, out + w * h))
		return 1;
	compare8("whole frame luma (macOS's guide)", out, final, (size_t)w * h);

	/* what can ship: no correction cube (identity) - how far from macOS, overall and on the subject */
	struct sl_cube idc;

	if (sl_cube_build(&idc, n, NULL, &p) ||
	    sl_render_nv12(frame, frame + w * h, w, h, w, &rd, &p, g512, 512, 288, disp, dw, dh, &face, nfaces, &l,
			   off[FOCUS_STUDIO], &idc, out, out + w * h))
		return 1;
	{
		double sum = 0, ssum = 0;
		long sn = 0;
		int worst = 0;

		for (int j = 0; j < h; j++)
			for (int i = 0; i < w; i++) {
				int d = abs(out[j * w + i] - final[j * w + i]);
				/* the subject: the light mask's body channel, at 320x180 */
				float b = m8[((j / 4) * gw + i / 4) * 2 + 1];

				sum += d;
				worst = d > worst ? d : worst;
				if (b > 0.5f) {
					ssum += d;
					sn++;
				}
			}
		printf("no correction cube: luma mean |diff| %.2f (subject %.2f), worst %d\n", sum / (w * h),
		       sn ? ssum / sn : 0.0, worst);
		compare8("whole frame luma (identity cube)", out, final, (size_t)w * h);
		compare8("whole frame chroma (identity cube)", out + w * h, final + w * h, (size_t)w * h / 2);
	}
	sl_cube_free(&idc);
	return bad;
}
