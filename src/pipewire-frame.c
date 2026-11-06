// SPDX-License-Identifier: MIT
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <string.h>
#include <spa/param/format.h>
#include <spa/param/video/format.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/pod/parser.h>
#include <spa/utils/defs.h>
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

static bool pod_offers_nv12(const struct spa_pod *p)
{
	const struct spa_pod_prop *prop = spa_pod_find_prop(p, NULL, SPA_FORMAT_VIDEO_format);
	uint32_t n, choice;
	const struct spa_pod *v;

	if (!prop)
		return false;
	v = spa_pod_get_values(&prop->value, &n, &choice);
	if (v->type != SPA_TYPE_Id)
		return false;
	for (uint32_t i = 0; i < n; i++)
		if (((const uint32_t *)SPA_POD_BODY_CONST(v))[i] == SPA_VIDEO_FORMAT_NV12)
			return true;
	return false;
}

struct spa_pod *cdn_output_format(const struct spa_pod *camera)
{
	uint8_t buf[4096];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	struct spa_pod_frame f;
	const struct spa_pod_object *o = (const struct spa_pod_object *)camera;
	struct spa_pod_prop *prop;
	struct spa_pod *out;

	if (!spa_pod_is_object_type(camera, SPA_TYPE_OBJECT_Format) || !pod_offers_nv12(camera))
		return NULL;
	spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
	SPA_POD_OBJECT_FOREACH(o, prop) {
		if (prop->key == SPA_FORMAT_VIDEO_modifier)
			continue;
		if (prop->key == SPA_FORMAT_VIDEO_format) {
			spa_pod_builder_prop(&b, SPA_FORMAT_VIDEO_format, 0);
			spa_pod_builder_id(&b, SPA_VIDEO_FORMAT_NV12);
		} else {
			spa_pod_builder_raw_padded(&b, prop, SPA_POD_PROP_SIZE(prop));
		}
	}
	out = spa_pod_builder_pop(&b, &f);
	if (!out || b.state.offset > sizeof(buf))
		return NULL;
	return spa_pod_copy(out);
}

bool cdn_format_equal(const struct spa_pod *a, const struct spa_pod *b)
{
	return SPA_POD_SIZE(a) == SPA_POD_SIZE(b) && !memcmp(a, b, SPA_POD_SIZE(a));
}
