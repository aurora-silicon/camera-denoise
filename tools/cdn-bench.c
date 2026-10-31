// SPDX-License-Identifier: MIT
/*
 * Offline benchmark: run a backend over a raw NV12 file (frames back to back)
 * and report per-frame latency and CPU time. Optionally write the output.
 *
 *   cdn-bench [-b backend] [-t threads] [-s strength] [-l loops] [-o out.nv12]
 *             WIDTH HEIGHT in.nv12
 */
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

#include "../src/backend.h"

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static double cpu_ms(void)
{
	struct rusage ru;
	getrusage(RUSAGE_SELF, &ru);
	return (ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1e3 +
	       (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1e3;
}

static int cmp(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
	const char *bname = "cpu", *outp = NULL;
	int threads = 0, loops = 1, c;
	float strength = 1.0f;

	while ((c = getopt(argc, argv, "b:t:s:l:o:")) != -1) {
		switch (c) {
		case 'b': bname = optarg; break;
		case 't': threads = atoi(optarg); break;
		case 's': strength = (float)atof(optarg); break;
		case 'l': loops = atoi(optarg); break;
		case 'o': outp = optarg; break;
		default: return 2;
		}
	}
	if (argc - optind != 3) {
		fprintf(stderr, "usage: cdn-bench [-b backend] [-t threads] [-s strength] [-l loops] [-o out] W H in.nv12\n");
		return 2;
	}
	int w = atoi(argv[optind]), h = atoi(argv[optind + 1]);
	size_t fs = (size_t)w * h * 3 / 2;
	FILE *f = fopen(argv[optind + 2], "rb");
	if (!f) { perror("open"); return 1; }
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	int n = (int)(sz / (long)fs);
	uint8_t *in = malloc(fs * n), *out = malloc(fs);
	if (!in || !out || fread(in, fs, n, f) != (size_t)n) { fprintf(stderr, "read\n"); return 1; }
	fclose(f);
	FILE *fo = outp ? fopen(outp, "wb") : NULL;

	const struct cdn_backend_ops *ops = cdn_backend_find(bname, bname);
	if (!ops) { fprintf(stderr, "no backend %s\n", bname); return 1; }
	void *st = ops->create(w, h, threads);
	if (!st) { fprintf(stderr, "backend %s cannot run %dx%d\n", ops->name, w, h); return 1; }

	int total = n * loops;
	double *lat = malloc(sizeof(double) * total);
	double c0 = cpu_ms(), t0 = now_ms();
	for (int i = 0; i < total; i++) {
		uint8_t *src = in + fs * (size_t)(i % n);
		struct cdn_frame fi = { src, src + (size_t)w * h, w, h, w, w };
		struct cdn_frame fo_ = { out, out + (size_t)w * h, w, h, w, w };
		struct cdn_frame_info info = { 0 };
		double a = now_ms();
		if (ops->process(st, &fi, &fo_, &info, strength)) { fprintf(stderr, "process failed\n"); return 1; }
		lat[i] = now_ms() - a;
		if (fo && i < n)
			fwrite(out, fs, 1, fo);
	}
	double wall = now_ms() - t0, cpu = cpu_ms() - c0;
	/* skip the seeding frame */
	qsort(lat + 1, total - 1, sizeof(double), cmp);
	int m = total - 1;
	printf("backend=%s %dx%d frames=%d threads=%d strength=%.2f\n", ops->name, w, h, total,
	       threads, strength);
	printf("latency_ms p50=%.2f p95=%.2f max=%.2f\n", lat[1 + m / 2], lat[1 + m * 95 / 100],
	       lat[total - 1]);
	printf("cpu_ms_per_frame=%.2f wall_ms_per_frame=%.2f cpu_pct_at_30fps=%.1f sigma=%.2f\n",
	       cpu / total, wall / total, cpu / total * 30 / 10, ops->noise_sigma ? ops->noise_sigma(st) : -1);
	ops->destroy(st);
	if (fo)
		fclose(fo);
	return 0;
}
