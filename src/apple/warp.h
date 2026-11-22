/* SPDX-License-Identifier: MIT */
#ifndef CDN_APPLE_WARP_H
#define CDN_APPLE_WARP_H
/*
 * Projective resampling of an NV12 frame: dst(u, v) = src(H (u, v, 1)), bilinear, at sub-pixel
 * positions (an integer crop would move a slow pan in 2-pixel steps).  Continuous coordinates: pixel i
 * covers [i, i + 1) in both frames; the chroma planes follow at half resolution.  Rows are split over a
 * few threads; along a row the mapping is evaluated exactly every 16 pixels and linearly in between
 * (error < 0.01 px for camera-like homographies).
 */
#include "../backend.h"

/* Hm: row-major 3x3, output pixel -> input pixel. src and dst must not overlap. threads <= 0: default. */
void apple_warp_nv12(const struct cdn_frame *src, struct cdn_frame *dst, const double Hm[9], int threads);

#endif
