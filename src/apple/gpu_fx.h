/* SPDX-License-Identifier: MIT */
#ifndef CDN_APPLE_GPU_FX_H
#define CDN_APPLE_GPU_FX_H

#include <stdbool.h>

#include "portrait.h"

/*
 * The Apple effects' full-size work on a Vulkan GPU (gpu_fx.c): the frame's
 * quarter-size sums (quarter.comp; the same numbers as
 * apple_portrait_quarter(), valid until the next call) and the composite
 * (fx.comp). apple_gpu_apply(..., loaded): the frame is the one
 * apple_gpu_quarter() uploaded, unchanged since, so it is not sent again.
 * NULL from apple_gpu_new() = no usable GPU (or a build without Vulkan): the
 * CPU loops in portrait.c do the same work.
 */
struct apple_gpu;
#ifdef APPLE_GPU
struct apple_gpu *apple_gpu_new(int width, int height, int qw, int qh);
void apple_gpu_free(struct apple_gpu *g);
const char *apple_gpu_name(const struct apple_gpu *g);
int apple_gpu_quarter(struct apple_gpu *g, const struct cdn_frame *f, struct apple_quarter *q);
int apple_gpu_apply(struct apple_gpu *g, struct cdn_frame *f, const struct apple_fx_maps *m,
		    bool blur, bool light, bool loaded);
#else
static inline struct apple_gpu *apple_gpu_new(int w, int h, int qw, int qh)
{
	(void)w; (void)h; (void)qw; (void)qh;
	return NULL;
}
static inline void apple_gpu_free(struct apple_gpu *g) { (void)g; }
static inline const char *apple_gpu_name(const struct apple_gpu *g) { (void)g; return "none"; }
static inline int apple_gpu_quarter(struct apple_gpu *g, const struct cdn_frame *f,
				    struct apple_quarter *q)
{
	(void)g; (void)f; (void)q;
	return -1;
}
static inline int apple_gpu_apply(struct apple_gpu *g, struct cdn_frame *f, const struct apple_fx_maps *m,
				  bool blur, bool light, bool loaded)
{
	(void)g; (void)f; (void)m; (void)blur; (void)light; (void)loaded;
	return -1;
}
#endif

#endif
