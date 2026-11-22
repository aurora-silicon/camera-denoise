// SPDX-License-Identifier: MIT
/*
 * Studio Light's face exposure statistics (src/apple/faceexp.c) against the same passes rendered on macOS:
 * CDN_FEXP_REF names a folder with a 1-frame capture with faces (its passes' inputs and outputs, named
 * exposure_*, light_estimate_*; buf/ the buffers; tuning/face-exposure.conf the tuning it ran with;
 * frame.nv12 the input). Inputs (the half-size luma, the segmentation's skin and person planes, the faces and
 * their tone probabilities) and outputs (global and face stats, coefficients, the light estimate) all come
 * from the capture. Captures are macOS's output for a picture of a person and stay local; skips without one.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/apple/faceexp.h"
#include "../src/apple/studiolight.h"

static const char *dir;

static void *load(const char *name, size_t want)
{
	char path[4096];
	FILE *f;
	void *b = malloc(want);

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	f = fopen(path, "rb");
	if (!f || fread(b, 1, want, f) != want) {
		fprintf(stderr, "read %s failed\n", path);
		exit(1);
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

static int bad;

static void check(const char *what, double ours, double ref)
{
	int ok = fabs(ours - ref) <= 1e-5 + fabs(ref) * 1e-4;

	printf("%-22s %.6f  (macOS %.6f)%s\n", what, ours, ref, ok ? "" : "  MISMATCH");
	bad |= !ok;
}

int main(void)
{
	const char *K = "exposure_coefs";
	char n[512];
	int lw = 640, lh = 360, mw = 256, mh = 192;

	dir = getenv("CDN_FEXP_REF");
	if (!dir) {
		printf("SKIP: CDN_FEXP_REF not set\n");
		return 77;
	}
	snprintf(n, sizeof(n), "%s_tex1_mip0_640x360_fmt10.bin", K);
	uint8_t *l8 = load(n, (size_t)lw * lh);
	float *luma = malloc((size_t)lw * lh * sizeof(float)), *skin = malloc((size_t)mw * mh * sizeof(float));
	float *person = malloc((size_t)mw * mh * sizeof(float));

	/* the half-size luma from the frame (frame.nv12, 2 lw x 2 lh), checked against macOS's, then used below */
	char fn[4096];
	FILE *fr;
	uint8_t *fy = malloc((size_t)lw * lh * 4);

	snprintf(fn, sizeof(fn), "%s/frame.nv12", dir);
	fr = fopen(fn, "rb");
	if (fr && fread(fy, 1, (size_t)lw * lh * 4, fr) == (size_t)lw * lh * 4) {
		int lbad = 0;

		fexp_luma_half(fy, 2 * lw, 2 * lh, 2 * lw, luma);
		for (int i = 0; i < lw * lh; i++)
			lbad += (int)lroundf(luma[i] * 255) != l8[i];
		printf("half-size luma         %d of %d differ\n", lbad, lw * lh);
		bad |= lbad != 0;
	} else {
		printf("half-size luma         no frame.nv12: macOS's used\n");
		for (int i = 0; i < lw * lh; i++)
			luma[i] = l8[i] / 255.0f;
	}
	if (fr)
		fclose(fr);
	free(fy);
	snprintf(n, sizeof(n), "%s_tex5_mip0_256x192_fmt25.bin", K);
	uint16_t *h5 = load(n, (size_t)mw * mh * 2);
	snprintf(n, sizeof(n), "%s_tex6_mip0_256x192_fmt25.bin", K);
	uint16_t *h6 = load(n, (size_t)mw * mh * 2);

	for (int i = 0; i < mw * mh; i++) {
		skin[i] = half_to_float(h5[i]);
		person[i] = half_to_float(h6[i]);
	}
	/* the faces as Portrait passes them (48 B each): bottom-left origin, then 6 tone probabilities */
	int nf = 0;
	char path[4096];
	FILE *ff;

	snprintf(path, sizeof(path), "%s/buf/exposure_face_bytes1.bin", dir);
	ff = fopen(path, "rb");
	if (ff) {
		fseek(ff, 0, SEEK_END);
		nf = (int)(ftell(ff) / 48);
		fclose(ff);
	}
	if (nf < 1 || nf > 4) {
		fprintf(stderr, "faces: %d\n", nf);
		return 1;
	}
	float *fp = load("buf/exposure_face_bytes1.bin", (size_t)nf * 48);
	struct fexp_params p;
	struct fexp_global g;
	struct fexp_face_stats st[4];
	struct fexp_coeffs c;

	char terr[256];

	snprintf(path, sizeof(path), "%s/tuning/face-exposure.conf", dir);
	if (fexp_params_read(&p, path, terr, sizeof(terr))) {
		printf("SKIP: %s\n", terr);
		return 77;
	}
	fexp_global(luma, lw, lh, &g);
	uint8_t *gb = load("exposure_coefs_buf2.bin", 8460);
	float gref[2];

	memcpy(gref, gb + 4, 8);
	check("global median", g.median, gref[0]);
	check("global p99", g.p99, gref[1]);
	uint8_t *sb = load("exposure_coefs_buf3.bin", (size_t)16980 * nf);

	for (int i = 0; i < nf; i++) {
		const float *q = fp + i * 12;
		struct fexp_face face = { q[0], 1 - (q[1] + q[3]), q[2], q[3], { q[4], q[5], q[6], q[7], q[8], q[9] } };

		fexp_face_stats(luma, lw, lh, skin, person, mw, mh, &face, &p, &st[i]);
	}
	fexp_coefficients(&p, &g, st, nf, 0, 0, &c);
	for (int i = 0; i < nf; i++) {
		const uint8_t *r = sb + (size_t)16980 * i;
		float sf[21];

		memcpy(sf, r, sizeof(sf));
		printf("face %d: valid %d (macOS %d)\n", i, st[i].valid, r[0]);
		bad |= st[i].valid != r[0];
		check("  face size", st[i].size, sf[9]);
		if (!st[i].valid)
			continue;
		check("  skin p5", st[i].skin_lo, sf[10]);
		check("  skin median", st[i].skin_median, sf[11]);
		check("  skin p95", st[i].skin_hi, sf[12]);
		check("  face p1", st[i].face_lo, sf[14]);
		check("  face p95", st[i].face_hi, sf[15]);
		check("  target", st[i].target, sf[16]);
		check("  target adjusted", st[i].target_adj, sf[17]);
		check("  max boost", st[i].boost_max, sf[18]);
		check("  skin mean", st[i].mean, sf[20]);
	}
	uint8_t *cb = load("exposure_coefs_buf4.bin", 100);
	unsigned cf[2];
	float cv[15];

	memcpy(cf, cb, 8);
	memcpy(cv, cb + 8, sizeof(cv));
	printf("flags %u %u (macOS %u %u)\n", c.flags, c.flags2, cf[0], cf[1]);
	bad |= c.flags != cf[0] || c.flags2 != cf[1];
	check("c2 p99", c.p99, cv[0]);
	for (int k = 0; k < 5; k++) {
		char w[32];

		snprintf(w, sizeof(w), "boost[%d]", k);
		check(w, c.boost[k], cv[1 + k]);
	}
	check("gain[4]", c.gain[4], cv[10]);
	check("c5", c.c5, cv[11]);
	check("c6", c.c6, cv[12]);
	check("c7", c.c7, cv[13]);
	check("c8", c.c8, cv[14]);
	check("c13", c.c13, ((float *)(cb + 96))[0]);

	/* the light estimate from these */
	struct sl_light l;
	float *le = load("light_estimate_buf1.bin", 64);

	sl_light_from_fexp(&l, c.c6, c.c7, c.c8, c.c13);
	check("light L", l.l, le[8]);
	check("light y", l.y, le[9]);
	check("light z", l.z, le[10]);
	check("light w", l.w, le[11]);
	return bad;
}
