/* SPDX-License-Identifier: MIT */
#ifndef CDN_ZOOM_H
#define CDN_ZOOM_H
/*
 * Digital zoom without the effects plugin: crop a rectangle of an NV12 frame
 * and scale it back to the frame's size, in place.
 *
 * The crop and the scaling run on the SoC's scaler when there is one (a V4L2
 * memory-to-memory device whose driver is "apple-scaler", which takes NV12 and
 * an OUTPUT crop selection: the M5's MSR23), with its polyphase filters; the
 * CPU does only two frame copies. Without it, or after any error from it, a
 * CPU bilinear pass does the same, so the stream never stops for the zoom.
 */
#include <stdbool.h>

#include "backend.h"

enum cdn_zoom_engine {
	CDN_ZOOM_AUTO,   /* the scaler when present, else the CPU */
	CDN_ZOOM_CPU,    /* never the scaler */
	CDN_ZOOM_SCALER, /* the scaler only: without it the frame is left as it is */
};

struct cdn_rect {
	int x, y, w, h;
};

struct cdn_zoom;

struct cdn_zoom *cdn_zoom_new(int width, int height, enum cdn_zoom_engine engine);
void cdn_zoom_free(struct cdn_zoom *z);
/* In place. 0, or a negative errno (the frame is then as it was). */
int cdn_zoom_apply(struct cdn_zoom *z, struct cdn_frame *f, struct cdn_rect crop);
/* "scaler", "cpu" or "none": what the last cdn_zoom_apply used */
const char *cdn_zoom_used(const struct cdn_zoom *z);
/* The centred crop for a zoom factor (>= 1), even-aligned, at least 64 x 64. */
struct cdn_rect cdn_zoom_rect(int width, int height, float zoom);
/* CPU bilinear crop-and-scale from src to dst (both NV12, may not overlap). */
void cdn_zoom_cpu(const struct cdn_frame *src, struct cdn_rect crop, struct cdn_frame *dst);

#endif
