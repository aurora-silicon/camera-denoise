// SPDX-License-Identifier: MIT
/* Paired output parity (each frame against the matching CPU frame).
 * cdn-metrics instead averages the reference, which is useful for static
 * scene noise measurements but cannot measure backend parity. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
	if (argc != 5) {
		fprintf(stderr, "usage: cdn-compare W H cpu.nv12 gpu.nv12\n"); return 2;
	}
	char *end;
	long w = strtol(argv[1], &end, 10);
	if (*end || w < 16 || w > 16384 || (w&1)) return 2;
	long h = strtol(argv[2], &end, 10);
	if (*end || h < 16 || h > 16384 || (h&1)) return 2;
	size_t n = (size_t)w*h, fs = n*3/2;
	FILE *a = fopen(argv[3], "rb"), *b = fopen(argv[4], "rb");
	if (!a || !b) { perror("open"); return 1; }
	uint8_t *fa = malloc(fs), *fb = malloc(fs);
	if (!fa || !fb) return 1;
	size_t frames = 0;
	double total[2] = {0}, worst[2] = {INFINITY, INFINITY};
	unsigned max_error = 0;
	for (;;) {
		size_t na = fread(fa, 1, fs, a), nb = fread(fb, 1, fs, b);
		if (!na && !nb) break;
		if (na != fs || nb != fs) { fprintf(stderr, "unequal or partial frames\n"); return 1; }
		uint64_t error[2] = {0};
		for (size_t j = 0; j < fs; j++) {
			unsigned d = (unsigned)abs((int)fa[j]-fb[j]);
			if (d > max_error) max_error = d;
			error[j>=n] += d*d;
		}
		for (int p = 0; p < 2; p++) {
			double db = error[p] ? 10*log10(65025.0*(p?n/2:n)/error[p]) : INFINITY;
			if (db < worst[p]) worst[p] = db;
			total[p] += error[p];
		}
		frames++;
	}
	if (!frames || ferror(a) || ferror(b)) { fprintf(stderr,"no complete frames or read error\n"); return 1; }
	printf("paired_frames=%zu max_error=%u psnr_y=%.3f psnr_uv=%.3f worst_frame_y=%.3f worst_frame_uv=%.3f dB\n",
		frames, max_error, total[0]?10*log10(65025.0*n*frames/total[0]):INFINITY,
		total[1]?10*log10(65025.0*(n/2)*frames/total[1]):INFINITY, worst[0],worst[1]);
	free(fa); free(fb); fclose(a); fclose(b);
	return worst[0] >= 40 && worst[1] >= 40 ? 0 : 1;
}
