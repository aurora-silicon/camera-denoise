/* SPDX-License-Identifier: MIT */
#ifndef CDN_PIPEWIRE_FRAME_H
#define CDN_PIPEWIRE_FRAME_H
#include <stdbool.h>
#include <spa/buffer/buffer.h>
#include <spa/param/video/raw.h>
#include "backend.h"
bool cdn_frame_from_buffer(struct spa_buffer *b, const struct spa_video_info_raw *f,
			   struct cdn_frame *fr);
bool cdn_frame_dmabuf(struct spa_buffer *b, const struct cdn_frame *f,
		     struct cdn_dmabuf *dma);
#endif
