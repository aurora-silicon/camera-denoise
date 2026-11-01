// SPDX-License-Identifier: MIT
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include "pipewire-frame.h"

static bool plane(struct spa_data *d, int w, int rows, uint8_t **ptr, int *stride)
{
	if (!d->data || !d->chunk || d->chunk->stride < 0) return false;
	*stride = d->chunk->stride ? d->chunk->stride : w;
	if (*stride < w) return false;
	uint64_t need = (uint64_t)*stride*(rows-1)+w;
	if ((d->chunk->size && d->chunk->size < need) ||
	    (uint64_t)d->chunk->offset+need > d->maxsize) return false;
	*ptr = (uint8_t *)d->data + d->chunk->offset;
	return true;
}

bool cdn_frame_from_buffer(struct spa_buffer *b, const struct spa_video_info_raw *f,
			   struct cdn_frame *fr)
{
	if (!b || !f || !fr || !b->datas || !b->n_datas || b->n_datas > 2 ||
	    !f->size.width || !f->size.height || (f->size.width&1) || (f->size.height&1) ||
	    f->size.width > INT_MAX || f->size.height > INT_MAX) return false;
	int w = (int)f->size.width, h = (int)f->size.height;
	struct spa_data *d = &b->datas[0];
	fr->width = w; fr->height = h;
	if (!plane(d,w,h,&fr->y,&fr->stride_y)) return false;
	if (b->n_datas == 2) {
		if (!plane(&b->datas[1],w,h/2,&fr->uv,&fr->stride_uv)) return false;
	} else {
		uint64_t uv_offset = (uint64_t)fr->stride_y*h;
		uint64_t need = uv_offset + (uint64_t)fr->stride_y*(h/2-1)+w;
		if ((d->chunk->size && d->chunk->size < need) ||
		    (uint64_t)d->chunk->offset+need > d->maxsize) return false;
		fr->uv = fr->y + uv_offset; fr->stride_uv = fr->stride_y;
	}
	/* The camera normally dequeues only completed frames. Honor any
	 * implicit writer fence before the CPU noise trigger reads mapped data. */
	for (uint32_t i = 0; i < b->n_datas; i++) {
		if (b->datas[i].type != SPA_DATA_DmaBuf) continue;
		if (b->datas[i].fd < 0 || b->datas[i].fd > INT_MAX) return false;
		struct pollfd p = { .fd = (int)b->datas[i].fd, .events = POLLIN };
		int rc;
		do { rc = poll(&p,1,1000); } while (rc < 0 && errno == EINTR);
		if (rc <= 0 || !(p.revents&POLLIN) || (p.revents&(POLLERR|POLLNVAL))) return false;
	}
	return true;
}

bool cdn_frame_dmabuf(struct spa_buffer *b, const struct cdn_frame *f,
		     struct cdn_dmabuf *dma)
{
	if (!b || !f || !dma || !b->datas || !b->n_datas || b->n_datas > 2) return false;
	struct spa_data *y = &b->datas[0], *uv = b->n_datas == 2 ? &b->datas[1] : y;
	if (y->type != SPA_DATA_DmaBuf || uv->type != SPA_DATA_DmaBuf ||
	    !y->chunk || !uv->chunk || y->fd < 0 || uv->fd < 0 ||
	    y->fd > INT_MAX || uv->fd > INT_MAX) return false;
	uint64_t yo = (uint64_t)y->mapoffset + y->chunk->offset;
	uint64_t uo = b->n_datas == 2 ? (uint64_t)uv->mapoffset + uv->chunk->offset :
		yo + (uint64_t)f->stride_y*f->height;
	if (yo > UINT32_MAX || uo > UINT32_MAX) return false;
	*dma = (struct cdn_dmabuf){ f->width, f->height,
		{ (int)y->fd, (uint32_t)yo, (uint32_t)f->stride_y },
		{ (int)uv->fd, (uint32_t)uo, (uint32_t)f->stride_uv } };
	return true;
}
