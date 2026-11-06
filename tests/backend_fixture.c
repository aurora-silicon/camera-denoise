// SPDX-License-Identifier: MIT
#include <errno.h>
#include "../src/backend.h"
static int token;
static void *create(int w, int h, int t) { (void)w; (void)h; (void)t; return &token; }
static int process(void *s, const struct cdn_frame *i, struct cdn_frame *o,
		   const struct cdn_frame_info *n, float strength)
{ (void)s; (void)i; (void)o; (void)n; (void)strength; return 0; }
static void destroy(void *s) { (void)s; }
static const struct cdn_backend_ops ops = {
#ifdef BAD_ABI
	.abi = 0,
#else
	.abi = CDN_BACKEND_ABI,
#endif
	.name="fixture", .create=create, .process=process, .destroy=destroy };
const struct cdn_backend_ops *cdn_backend_entry(void) { return &ops; }
#ifndef LEGACY
static int dmabuf(void *s, const struct cdn_dmabuf *i, struct cdn_frame *o,
		  const struct cdn_frame_info *n, float strength)
{ (void)s; (void)i; (void)o; (void)n; (void)strength; return -ENOTSUP; }
static const struct cdn_backend_dmabuf_ops ext = {
	.abi=CDN_DMABUF_ABI, .flags=CDN_BACKEND_SINGLE_UPLOAD, .process=dmabuf };
const struct cdn_backend_dmabuf_ops *cdn_backend_dmabuf_entry(void) { return &ext; }
#endif
