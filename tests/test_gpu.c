// SPDX-License-Identifier: MIT
/* Exercise the shipped plugin against the CPU oracle, including padding,
 * scene cuts, signed saturation, noise changes, strength ramps and resets. */
#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/backend.h"

static uint32_t rng = 12345;
static unsigned random_byte(void)
{
	rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
	return rng & 255;
}

static int run(const struct cdn_backend_ops *gpu, const struct cdn_backend_dmabuf_ops *dma, int w, int h)
{
	const struct cdn_backend_ops *cpu = cdn_cpu_backend();
	void *a = cpu->create(w, h, 1), *b = gpu->create(w, h, 0);
	if (!b) { cpu->destroy(a); return 77; }
	int stride = w+17, fails = 0;
	size_t n = (size_t)stride*h, fs = n*3/2;
	uint8_t *in = malloc(fs), *oa = malloc(fs), *ob = malloc(fs);
	if (!a || !in || !oa || !ob) exit(1);
	struct cdn_frame fi = { in, in+n, w, h, stride, stride };
	struct cdn_frame fa = { oa, oa+n, w, h, stride, stride };
	struct cdn_frame fb = { ob, ob+n, w, h, stride, stride };
	struct cdn_frame_info info = {0};
	double sse = 0; size_t bytes = 0; unsigned maxerr = 0;
	for (int frame = 0; frame < 64; frame++) {
		for (size_t j = 0; j < fs; j++) in[j] = 0xa5;
		for (int plane = 0; plane < 2; plane++) for (int y = 0; y < (plane?h/2:h); y++)
			for (int x = 0; x < w; x++) {
				int v = plane ? 128+(x&1)*15 : 40+x*140/w + ((y/25)&1)*20;
				if (!plane && x>frame*3 && x<frame*3+w/5 && y>h/3 && y<2*h/3) v = 25;
				int noise = frame < 24 ? 32 : 8;
				v += (int)(random_byte() % (unsigned)noise)-noise/2;
				if (frame >= 46 && frame <= 49) v = (int)random_byte();
				if (frame == 50) v = (x&1) ? 255 : 0;
				in[(plane?n:0)+(size_t)y*stride+x] = (uint8_t)(v<0?0:v>255?255:v);
			}
		memset(oa,0xcd,fs); memset(ob,0xcd,fs);
		if (frame == 32) { cpu->reset(a); gpu->reset(b); }
		float strength = frame < 16 ? 1 : frame < 24 ? 0 : frame < 32 ? 0.375f : 1;
		if (frame == 33) strength = -0.5f;
		if (frame == 34) strength = 1.5f;
		if (frame == 16) {
			struct cdn_dmabuf unavailable = { w,h,{-1,0,(uint32_t)w},{-1,0,(uint32_t)w} };
			if (!dma || dma->abi != CDN_DMABUF_ABI ||
			    dma->process(b,&unavailable,&fb,&info,strength) != -ENOTSUP) {
				fprintf(stderr,"unsupported import did not request upload fallback\n"); fails++;
			}
			struct cdn_frame bad = fi; bad.width += 2;
			if (gpu->process(b,&bad,&fb,&info,strength) != -EINVAL) {
				fprintf(stderr,"invalid geometry accepted\n"); fails++;
			}
		}
		if (cpu->process(a,&fi,&fa,&info,strength) || gpu->process(b,&fi,&fb,&info,strength)) {
			fprintf(stderr,"process failed at frame %d\n",frame); fails++; break;
		}
		// Padding must remain untouched, and all pixels must be bit-exact.
		for (size_t j = 0; j < fs; j++) {
			unsigned d = (unsigned)abs((int)oa[j]-ob[j]);
			sse += d*d; bytes++;
			if (d > maxerr) maxerr = d;
			if (d && !fails) {
				fprintf(stderr,"%dx%d frame %d byte %zu: cpu=%u gpu=%u\n", w,h,frame,j,oa[j],ob[j]); fails++;
			}
		}
		if (fabsf(cpu->noise_sigma(a)-gpu->noise_sigma(b)) > 0.0001f) {
			fprintf(stderr,"noise mismatch frame %d: %.4f != %.4f\n",frame,cpu->noise_sigma(a),gpu->noise_sigma(b)); fails++;
		}
	}
	printf("%dx%d CPU/GPU PSNR %.2f dB, max error %u\n",w,h,sse?10*log10(65025.0*bytes/sse):INFINITY,maxerr);
	gpu->destroy(b); cpu->destroy(a);
	free(in); free(oa); free(ob);
	return fails ? 1 : 0;
}
int main(int argc, char **argv)
{
	if (argc != 2) return 1;
	void *dl = dlopen(argv[1], RTLD_NOW|RTLD_LOCAL);
	if (!dl) { fprintf(stderr,"%s\n",dlerror()); return 1; }
	cdn_backend_entry_fn entry = (cdn_backend_entry_fn)dlsym(dl,CDN_BACKEND_ENTRY);
	const struct cdn_backend_ops *gpu = entry ? entry() : NULL;
	if (!gpu || gpu->abi != CDN_BACKEND_ABI) return 1;
	cdn_backend_dmabuf_entry_fn de = (cdn_backend_dmabuf_entry_fn)dlsym(dl,CDN_DMABUF_ENTRY);
	const struct cdn_backend_dmabuf_ops *dma = de ? de() : NULL;
	int sizes[][2] = {{320,180},{318,182},{18,18},{16,16},{1920,1080}};
	for (unsigned i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
		int rc = run(gpu,dma,sizes[i][0],sizes[i][1]);
		if (rc) {
			dlclose(dl);
			return rc == 77 && getenv("CDN_TEST_REQUIRE_GPU") ? 1 : rc;
		}
	}
	dlclose(dl);
	return 0;
}
