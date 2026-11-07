/* SPDX-License-Identifier: MIT */
#ifndef CDN_PIPEWIRE_FRAME_H
#define CDN_PIPEWIRE_FRAME_H
#include <stdbool.h>
#include <spa/buffer/buffer.h>
#include <spa/param/video/raw.h>
#include <spa/pod/pod.h>
#include "backend.h"
bool cdn_frame_from_buffer(struct spa_buffer *b, const struct spa_video_info_raw *f,
			   struct cdn_frame *fr);
bool cdn_frame_dmabuf(struct spa_buffer *b, const struct cdn_frame *f,
		     struct cdn_dmabuf *dma);
/*
 * The output format for one camera format: NV12 only, without the DMA-BUF
 * modifier, since the filter writes its frames into shared memory.  A client
 * that fixated a modifier would ask for DMA-BUF buffers we cannot give it.
 * Returns a malloc'd pod, or NULL if the camera format is not NV12.
 */
struct spa_pod *cdn_output_format(const struct spa_pod *camera);
bool cdn_format_equal(const struct spa_pod *a, const struct spa_pod *b);
#endif
