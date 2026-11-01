// SPDX-License-Identifier: MIT
/*
 * Motion artefacts of the CPU backend on a real luma-only capture.
 *
 *   cdn-yeval [-f filtered.y] [-o out.y] W H raw.y
 *
 * Runs the CPU backend over raw 8-bit luma frames (chroma flat 128), or
 * with -f takes an already filtered capture of the same frames, and
 * reports, against masks found from the raw frames alone:
 *
 *   moving:  8x8 blocks whose mean changed by more than 4 robust std of
 *            the block-mean noise since the previous frame
 *   halo:    pixels within 16 px of a moving block, now or in the last 8
 *            frames (the wake), not in a moving block
 *   speckle: share of halo (and static) pixels further than half the raw
 *            per-pixel noise std from their 3x3 median: isolated dots
 *            where neighbouring pixels restarted averaging and others not
 *   grain:   robust std of out - 7x7 mean inside moving blocks
 *   static:  median per-pixel temporal std over the 60-frame window with
 *            the least motion, on pixels 16 px or more from any motion in it
 *   ms:      CPU time per frame on one thread
 */
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../src/backend.h"
#include "../src/cpu/denoise_cpu.h"

#define BS 8
#define WIN 60

static int cmpf(const void *a, const void *b)
{
	float x = *(const float *)a, y = *(const float *)b;
	return x < y ? -1 : x > y;
}

static const uint8_t *map(const char *p, size_t *len)
{
	int fd = open(p, O_RDONLY);
	struct stat st;
	if (fd < 0 || fstat(fd, &st))
		return NULL;
	*len = (size_t)st.st_size;
	void *m = mmap(NULL, *len, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	return m == MAP_FAILED ? NULL : m;
}

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static inline int med9(const uint8_t *p, int s)
{
	int v[9] = { p[-s - 1], p[-s], p[-s + 1], p[-1], p[0], p[1], p[s - 1], p[s], p[s + 1] };
	for (int i = 1; i < 9; i++)
		for (int j = i; j > 0 && v[j] < v[j - 1]; j--) {
			int t = v[j]; v[j] = v[j - 1]; v[j - 1] = t;
		}
	return v[4];
}

int main(int argc, char **argv)
{
	const char *filt = NULL, *outp = NULL;
	int c;
	while ((c = getopt(argc, argv, "f:o:")) != -1) {
		if (c == 'f') filt = optarg;
		else if (c == 'o') outp = optarg;
		else return 2;
	}
	if (argc - optind != 3) {
		fprintf(stderr, "usage: cdn-yeval [-f filtered.y] [-o out.y] W H raw.y\n");
		return 2;
	}
	int W = atoi(argv[optind]), H = atoi(argv[optind + 1]);
	size_t fs = (size_t)W * H, len, flen = 0;
	const uint8_t *raw = map(argv[optind + 2], &len), *fin = filt ? map(filt, &flen) : NULL;
	if (!raw || (filt && !fin)) {
		fprintf(stderr, "cannot map input\n");
		return 1;
	}
	int n = (int)(len / fs);
	if (filt && (int)(flen / fs) < n)
		n = (int)(flen / fs);
	int bw = W / BS, bh = H / BS, nb = bw * bh;

	/* block means of the raw frames, and the moving-block masks */
	float *bm = malloc(sizeof(float) * nb * n);
	for (int t = 0; t < n; t++)
		for (int by = 0; by < bh; by++)
			for (int bx = 0; bx < bw; bx++) {
				int s = 0;
				for (int y = 0; y < BS; y++)
					for (int x = 0; x < BS; x++)
						s += raw[t * fs + (size_t)(by * BS + y) * W + bx * BS + x];
				bm[(size_t)t * nb + by * bw + bx] = s / (float)(BS * BS);
			}
	float *tmp = malloc(sizeof(float) * nb * (n - 1)), *tp = malloc(sizeof(float) * (size_t)W * H / 4);
	for (int t = 1; t < n; t++)
		for (int i = 0; i < nb; i++)
			tmp[(size_t)(t - 1) * nb + i] = fabsf(bm[(size_t)t * nb + i] - bm[(size_t)(t - 1) * nb + i]);
	/* sample for the median */
	int ns = 0;
	for (size_t i = 0; i < (size_t)nb * (n - 1) && ns < W * H / 4; i += 7)
		tp[ns++] = tmp[i];
	qsort(tp, ns, sizeof(float), cmpf);
	float sb = 1.4826f * tp[ns / 2], tm = 4 * sb > 2 ? 4 * sb : 2;
	uint8_t *mv = calloc((size_t)nb * n, 1), *dl = calloc((size_t)nb * n, 1);
	for (int t = 1; t < n; t++)
		for (int i = 0; i < nb; i++)
			mv[(size_t)t * nb + i] = tmp[(size_t)(t - 1) * nb + i] > tm;
	for (int t = 0; t < n; t++)
		for (int by = 0; by < bh; by++)
			for (int bx = 0; bx < bw; bx++) {
				int d = 0;
				for (int dy = -2; dy <= 2 && !d; dy++)
					for (int dx = -2; dx <= 2 && !d; dx++) {
						int yy = by + dy, xx = bx + dx;
						if (yy >= 0 && yy < bh && xx >= 0 && xx < bw)
							d = mv[(size_t)t * nb + yy * bw + xx];
					}
				dl[(size_t)t * nb + by * bw + bx] = d;
			}
	/* wake: near motion now, or moved in the last 8 frames */
	uint8_t *wk = calloc((size_t)nb * n, 1);
	for (int t = 0; t < n; t++)
		for (int i = 0; i < nb; i++) {
			int d = 0;
			for (int u = t - 8; u <= t && !d; u++)
				if (u >= 0)
					d = dl[(size_t)u * nb + i];
			wk[(size_t)t * nb + i] = d;
		}
	/* raw per-pixel noise: successive differences on blocks far from motion */
	ns = 0;
	for (int t = 1; t < n && ns < W * H / 4; t += 3)
		for (int i = 0; i < nb && ns < W * H / 4; i += 3) {
			if (dl[(size_t)t * nb + i] || dl[(size_t)(t - 1) * nb + i])
				continue;
			size_t p = (size_t)(i / bw * BS + 3) * W + i % bw * BS + 3;
			tp[ns++] = fabsf((float)raw[t * fs + p] - raw[(t - 1) * fs + p]);
		}
	qsort(tp, ns, sizeof(float), cmpf);
	float sraw = 1.4826f * tp[ns / 2] / sqrtf(2);
	/* the quietest window */
	int best = 1, bestc = 1 << 30;
	for (int a = 1; a + WIN <= n; a += 10) {
		int cnt = 0;
		for (int t = a; t < a + WIN; t++)
			for (int i = 0; i < nb; i++)
				cnt += mv[(size_t)t * nb + i];
		if (cnt < bestc) { bestc = cnt; best = a; }
	}
	uint8_t *stat_ok = malloc(nb);
	int nstat = 0;
	for (int i = 0; i < nb; i++) {
		int d = 0;
		for (int t = best; t < best + WIN; t++)
			d |= dl[(size_t)t * nb + i];
		stat_ok[i] = !d;
		nstat += !d;
	}

	void *st = filt ? NULL : cdn_cpu_create(W, H, 1);
	uint8_t *in = malloc(fs * 3 / 2), *out = malloc(fs * 3 / 2);
	memset(in + fs, 128, fs / 2);
	FILE *fo = outp ? fopen(outp, "wb") : NULL;
	double ms = 0, *s1 = calloc(fs, sizeof(double)), *s2 = calloc(fs, sizeof(double));
	long halo_n = 0, halo_sp = 0, st_n = 0, st_sp = 0, raw_halo_sp = 0;
	float *hp = malloc(sizeof(float) * 4000000), *hpr = malloc(sizeof(float) * 4000000);
	long nhp = 0;
	for (int t = 0; t < n; t++) {
		const uint8_t *r = raw + t * fs;
		const uint8_t *o;
		if (filt) {
			o = fin + t * fs;
		} else {
			memcpy(in, r, fs);
			struct cdn_frame fi = { in, in + fs, W, H, W, W }, fo_ = { out, out + fs, W, H, W, W };
			double a = now_ms();
			cdn_cpu_process_ex(st, &fi, &fo_, 1.0f, true);
			if (t)
				ms += now_ms() - a;
			o = out;
		}
		if (fo)
			fwrite(o, fs, 1, fo);
		if (t >= best && t < best + WIN)
			for (int i = 0; i < nb; i++) {
				if (!stat_ok[i])
					continue;
				for (int y = 0; y < BS; y += 2)
					for (int x = 0; x < BS; x += 2) {
						size_t p = (size_t)(i / bw * BS + y) * W + i % bw * BS + x;
						s1[p] += o[p];
						s2[p] += (double)o[p] * o[p];
					}
			}
		if (t < 10)
			continue;	/* let the history settle */
		for (int by = 1; by < bh - 1; by++)
			for (int bx = 1; bx < bw - 1; bx++) {
				size_t bi = (size_t)t * nb + by * bw + bx;
				int m = mv[bi], halo = wk[bi] && !m, quiet = !wk[bi] && (t % 5 == 0);
				if (!m && !halo && !quiet)
					continue;
				for (int y = 0; y < BS; y++)
					for (int x = 0; x < BS; x++) {
						size_t p = (size_t)(by * BS + y) * W + bx * BS + x;
						if (m) {
							if ((x | y) & 1 || nhp >= 4000000)
								continue;
							if (by * BS + y < 3 || by * BS + y >= H - 3 || bx * BS + x < 3 || bx * BS + x >= W - 3)
								continue;
							int sm = 0, sr = 0;
							for (int dy = -3; dy <= 3; dy++)
								for (int dx = -3; dx <= 3; dx++) {
									sm += o[p + dy * W + dx];
									sr += r[p + dy * W + dx];
								}
							hp[nhp] = fabsf(o[p] - sm / 49.0f);
							hpr[nhp++] = fabsf(r[p] - sr / 49.0f);
							continue;
						}
						int sp = abs(o[p] - med9(o + p, W)) > sraw / 2;
						if (halo) {
							halo_n++;
							halo_sp += sp;
							raw_halo_sp += abs(r[p] - med9(r + p, W)) > sraw / 2;
						} else {
							st_n++;
							st_sp += sp;
						}
					}
			}
	}
	ns = 0;
	for (size_t p = 0; p < fs; p++)
		if (s1[p] > 0 || s2[p] > 0) {
			double m = s1[p] / WIN;
			tp[ns++] = (float)sqrt(fmax(s2[p] / WIN - m * m, 0));
			if (ns >= W * H / 4)
				break;
		}
	qsort(tp, ns, sizeof(float), cmpf);
	float sstat = ns ? tp[ns / 2] : -1;
	qsort(hp, nhp, sizeof(float), cmpf);
	qsort(hpr, nhp, sizeof(float), cmpf);
	long mvn = 0;
	for (size_t i = 0; i < (size_t)nb * n; i++)
		mvn += mv[i];
	printf("frames %d, raw noise std %.2f, block threshold %.2f, moving %.2f%% of blocks, "
	       "static window %d-%d (%d blocks)\n", n, sraw, tm, 100.0 * mvn / ((double)nb * n), best,
	       best + WIN, nstat);
	printf("speckle halo %.2f%% (raw %.2f%%), speckle static %.2f%%\n", 100.0 * halo_sp / (halo_n ? halo_n : 1),
	       100.0 * raw_halo_sp / (halo_n ? halo_n : 1), 100.0 * st_sp / (st_n ? st_n : 1));
	printf("grain in motion %.2f (raw %.2f), static temporal std %.2f, ms/frame %.2f\n",
	       nhp ? 1.4826 * hp[nhp / 2] : -1, nhp ? 1.4826 * hpr[nhp / 2] : -1, sstat,
	       filt ? 0 : ms / (n - 1));
	if (fo)
		fclose(fo);
	return 0;
}
