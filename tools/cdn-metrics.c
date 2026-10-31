// SPDX-License-Identifier: MIT
/*
 * Noise metrics for a static scene, from raw NV12 files.
 *
 *   cdn-metrics W H ref.nv12 test.nv12 [first [count]]
 *
 * The reference is the average of every frame in ref.nv12 (a long static
 * capture: averaging N frames is a long exposure with 1/sqrt(N) the noise).
 * For frames [first, first + count) of test.nv12 it reports:
 *   - temporal std: per-pixel standard deviation across those frames,
 *     averaged over the image (luma, and Cb/Cr)
 *   - PSNR of each frame against the reference (luma, chroma), mean
 *   - SSIM of luma against the reference (8x8 windows, stride 4), mean
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static long frames_in(FILE *f, size_t fs)
{
	fseek(f, 0, SEEK_END);
	long n = ftell(f) / (long)fs;
	fseek(f, 0, SEEK_SET);
	return n;
}

static double ssim_y(const float *ref, const uint8_t *img, int w, int h)
{
	const double c1 = 6.5025, c2 = 58.5225; /* (0.01*255)^2, (0.03*255)^2 */
	double acc = 0;
	long n = 0;

	for (int y = 0; y + 8 <= h; y += 4)
		for (int x = 0; x + 8 <= w; x += 4) {
			double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
			for (int j = 0; j < 8; j++)
				for (int i = 0; i < 8; i++) {
					double a = ref[(size_t)(y + j) * w + x + i];
					double b = img[(size_t)(y + j) * w + x + i];
					sa += a; sb += b; saa += a * a; sbb += b * b; sab += a * b;
				}
			double ma = sa / 64, mb = sb / 64;
			double va = saa / 64 - ma * ma, vb = sbb / 64 - mb * mb, cov = sab / 64 - ma * mb;
			acc += ((2 * ma * mb + c1) * (2 * cov + c2)) /
			       ((ma * ma + mb * mb + c1) * (va + vb + c2));
			n++;
		}
	return acc / n;
}

int main(int argc, char **argv)
{
	if (argc < 5) {
		fprintf(stderr, "usage: cdn-metrics W H ref.nv12 test.nv12 [first [count]]\n");
		return 2;
	}
	int w = atoi(argv[1]), h = atoi(argv[2]);
	size_t ny = (size_t)w * h, fs = ny * 3 / 2;
	FILE *fr = fopen(argv[3], "rb"), *ft = fopen(argv[4], "rb");
	if (!fr || !ft) { perror("open"); return 1; }
	long nr = frames_in(fr, fs), nt = frames_in(ft, fs);
	long first = argc > 5 ? atol(argv[5]) : 0;
	long count = argc > 6 ? atol(argv[6]) : nt - first;
	if (first + count > nt) count = nt - first;
	if (count < 2 || nr < 1) { fprintf(stderr, "not enough frames\n"); return 1; }

	uint8_t *buf = malloc(fs);
	float *ref = calloc(fs, sizeof(float));
	double *sum = calloc(fs, sizeof(double)), *sq = calloc(fs, sizeof(double));
	for (long i = 0; i < nr; i++) {
		if (fread(buf, fs, 1, fr) != 1) return 1;
		for (size_t k = 0; k < fs; k++) ref[k] += buf[k];
	}
	for (size_t k = 0; k < fs; k++) ref[k] /= (float)nr;

	double psnr_y = 0, psnr_c = 0, ssim = 0;
	fseek(ft, (long)fs * first, SEEK_SET);
	for (long i = 0; i < count; i++) {
		if (fread(buf, fs, 1, ft) != 1) return 1;
		double ey = 0, ec = 0;
		for (size_t k = 0; k < fs; k++) {
			double v = buf[k], d = v - ref[k];
			sum[k] += v; sq[k] += v * v;
			if (k < ny) ey += d * d; else ec += d * d;
		}
		psnr_y += 10 * log10(255.0 * 255.0 / (ey / ny));
		psnr_c += 10 * log10(255.0 * 255.0 / (ec / (fs - ny)));
		ssim += ssim_y(ref, buf, w, h);
	}
	double tsy = 0, tsu = 0, tsv = 0;
	for (size_t k = 0; k < fs; k++) {
		double m = sum[k] / count, var = sq[k] / count - m * m;
		double sd = sqrt(var > 0 ? var * count / (count - 1) : 0);
		if (k < ny) tsy += sd;
		else if (((k - ny) & 1) == 0) tsu += sd;
		else tsv += sd;
	}
	double meany = 0;
	for (size_t k = 0; k < ny; k++) meany += ref[k];
	printf("frames=%ld ref_frames=%ld mean_luma=%.1f\n", count, nr, meany / ny);
	printf("temporal_std y=%.3f cb=%.3f cr=%.3f\n", tsy / ny, tsu / ((fs - ny) / 2), tsv / ((fs - ny) / 2));
	printf("psnr_y=%.2f psnr_c=%.2f ssim_y=%.4f\n", psnr_y / count, psnr_c / count, ssim / count);
	return 0;
}
