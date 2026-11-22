/* SPDX-License-Identifier: MIT */
#include "zoom.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/videodev2.h>
#define CDN_HAVE_V4L2 1
#endif

#define MIN_SIDE 64
#define TIMEOUT_MS 200

struct plane_map {
	void *addr;
	size_t len;
};

struct cdn_zoom {
	int width, height;
	enum cdn_zoom_engine engine;
	const char *used;
	/* scaler */
	int fd;              /* -1: none (not found, refused, or failed once) */
	bool streaming;
	struct cdn_rect crop; /* the crop the scaler is set to */
	struct plane_map out, cap;
	uint32_t out_bpl, cap_bpl;
	/* CPU path: a copy of the source, so the frame can be rewritten in place */
	uint8_t *scratch;
	size_t scratch_len;
};

static int clampi(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

struct cdn_rect cdn_zoom_rect(int width, int height, float zoom)
{
	struct cdn_rect r;

	if (!(zoom >= 1.0f))
		zoom = 1.0f;
	r.w = clampi((int)(width / zoom) & ~1, MIN_SIDE, width & ~1);
	r.h = clampi((int)(height / zoom) & ~1, MIN_SIDE, height & ~1);
	r.x = ((width - r.w) / 2) & ~1;
	r.y = ((height - r.h) / 2) & ~1;
	return r;
}

static struct cdn_rect sane(const struct cdn_zoom *z, struct cdn_rect c)
{
	c.w = clampi(c.w & ~1, MIN_SIDE, z->width & ~1);
	c.h = clampi(c.h & ~1, MIN_SIDE, z->height & ~1);
	c.x = clampi(c.x & ~1, 0, (z->width - c.w) & ~1);
	c.y = clampi(c.y & ~1, 0, (z->height - c.h) & ~1);
	return c;
}

/* --- CPU: bilinear, 16.16 fixed point, sample centres aligned ------------- */

static void scale_plane(const uint8_t *src, int sstride, int sx, int sy, int sw, int sh,
			uint8_t *dst, int dstride, int dw, int dh, int comps)
{
	/* step in 16.16; centre-aligned: src = (dst + 0.5) * step - 0.5 */
	int64_t stepx = ((int64_t)sw << 16) / dw, stepy = ((int64_t)sh << 16) / dh;

	for (int y = 0; y < dh; y++) {
		int64_t fy = (y * stepy) + (stepy >> 1) - (1 << 15);
		int y0 = (int)(fy >> 16), wy = (int)(fy & 0xffff);
		if (fy < 0) {
			y0 = 0;
			wy = 0;
		}
		int y1 = y0 + 1 < sh ? y0 + 1 : sh - 1;
		y0 = y0 < sh ? y0 : sh - 1;
		const uint8_t *r0 = src + (size_t)(sy + y0) * sstride + (size_t)sx * comps;
		const uint8_t *r1 = src + (size_t)(sy + y1) * sstride + (size_t)sx * comps;
		uint8_t *d = dst + (size_t)y * dstride;

		for (int x = 0; x < dw; x++) {
			int64_t fx = (x * stepx) + (stepx >> 1) - (1 << 15);
			int x0 = (int)(fx >> 16), wx = (int)(fx & 0xffff);
			if (fx < 0) {
				x0 = 0;
				wx = 0;
			}
			int x1 = x0 + 1 < sw ? x0 + 1 : sw - 1;
			x0 = x0 < sw ? x0 : sw - 1;
			for (int c = 0; c < comps; c++) {
				int a = r0[x0 * comps + c], b = r0[x1 * comps + c];
				int e = r1[x0 * comps + c], f = r1[x1 * comps + c];
				int64_t top = a * (65536 - wx) + (int64_t)b * wx;
				int64_t bot = e * (65536 - wx) + (int64_t)f * wx;
				int64_t v = (top * (65536 - wy) + bot * wy + ((int64_t)1 << 31)) >> 32;
				d[x * comps + c] = (uint8_t)(v > 255 ? 255 : v);
			}
		}
	}
}

void cdn_zoom_cpu(const struct cdn_frame *src, struct cdn_rect c, struct cdn_frame *dst)
{
	scale_plane(src->y, src->stride_y, c.x, c.y, c.w, c.h,
		    dst->y, dst->stride_y, dst->width, dst->height, 1);
	scale_plane(src->uv, src->stride_uv, c.x / 2, c.y / 2, c.w / 2, c.h / 2,
		    dst->uv, dst->stride_uv, dst->width / 2, dst->height / 2, 2);
}

static int zoom_cpu(struct cdn_zoom *z, struct cdn_frame *f, struct cdn_rect c)
{
	size_t ylen = (size_t)f->stride_y * f->height, uvlen = (size_t)f->stride_uv * (f->height / 2);

	if (z->scratch_len < ylen + uvlen) {
		uint8_t *p = realloc(z->scratch, ylen + uvlen);
		if (!p)
			return -ENOMEM;
		z->scratch = p;
		z->scratch_len = ylen + uvlen;
	}
	memcpy(z->scratch, f->y, ylen);
	memcpy(z->scratch + ylen, f->uv, uvlen);
	struct cdn_frame src = *f;
	src.y = z->scratch;
	src.uv = z->scratch + ylen;
	cdn_zoom_cpu(&src, c, f);
	z->used = "cpu";
	return 0;
}

/* --- the scaler (V4L2 mem2mem, multi-planar API, one NV12 plane) ----------- */

#ifdef CDN_HAVE_V4L2
static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;

	do
		r = ioctl(fd, req, arg);
	while (r < 0 && errno == EINTR);
	return r < 0 ? -errno : 0;
}

/* The first /dev/video* whose driver is the Apple scaler. */
static int find_scaler(void)
{
	char path[64];

	for (int i = 0; i < 64; i++) {
		struct v4l2_capability cap;
		int fd;

		snprintf(path, sizeof(path), "/dev/video%d", i);
		fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		memset(&cap, 0, sizeof(cap));
		if (!xioctl(fd, VIDIOC_QUERYCAP, &cap) && !strcmp((char *)cap.driver, "apple-scaler") &&
		    ((cap.device_caps ? cap.device_caps : cap.capabilities) & V4L2_CAP_VIDEO_M2M_MPLANE))
			return fd;
		close(fd);
	}
	return -ENODEV;
}

static int set_fmt(int fd, uint32_t type, int w, int h, uint32_t *bpl)
{
	struct v4l2_format f;
	int r;

	memset(&f, 0, sizeof(f));
	f.type = type;
	f.fmt.pix_mp.width = (uint32_t)w;
	f.fmt.pix_mp.height = (uint32_t)h;
	f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
	f.fmt.pix_mp.field = V4L2_FIELD_NONE;
	f.fmt.pix_mp.num_planes = 1;
	f.fmt.pix_mp.colorspace = V4L2_COLORSPACE_REC709;
	f.fmt.pix_mp.ycbcr_enc = V4L2_YCBCR_ENC_709;
	f.fmt.pix_mp.quantization = V4L2_QUANTIZATION_LIM_RANGE;
	r = xioctl(fd, VIDIOC_S_FMT, &f);
	if (r)
		return r;
	if (f.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12 || f.fmt.pix_mp.num_planes != 1 ||
	    f.fmt.pix_mp.width != (uint32_t)w || f.fmt.pix_mp.height != (uint32_t)h)
		return -EINVAL;
	*bpl = f.fmt.pix_mp.plane_fmt[0].bytesperline;
	return 0;
}

static int map_buf(int fd, uint32_t type, struct plane_map *m)
{
	struct v4l2_requestbuffers rb;
	struct v4l2_buffer b;
	struct v4l2_plane p;
	int r;

	memset(&rb, 0, sizeof(rb));
	rb.count = 1;
	rb.type = type;
	rb.memory = V4L2_MEMORY_MMAP;
	r = xioctl(fd, VIDIOC_REQBUFS, &rb);
	if (r || rb.count < 1)
		return r ? r : -ENOMEM;
	memset(&b, 0, sizeof(b));
	memset(&p, 0, sizeof(p));
	b.type = type;
	b.memory = V4L2_MEMORY_MMAP;
	b.index = 0;
	b.m.planes = &p;
	b.length = 1;
	r = xioctl(fd, VIDIOC_QUERYBUF, &b);
	if (r)
		return r;
	m->len = p.length;
	m->addr = mmap(NULL, m->len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, p.m.mem_offset);
	if (m->addr == MAP_FAILED) {
		m->addr = NULL;
		return -errno;
	}
	return 0;
}

static void scaler_close(struct cdn_zoom *z)
{
	if (z->fd < 0)
		return;
	if (z->streaming) {
		uint32_t t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
		xioctl(z->fd, VIDIOC_STREAMOFF, &t);
		t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		xioctl(z->fd, VIDIOC_STREAMOFF, &t);
	}
	if (z->out.addr)
		munmap(z->out.addr, z->out.len);
	if (z->cap.addr)
		munmap(z->cap.addr, z->cap.len);
	close(z->fd);
	z->fd = -1;
	z->streaming = false;
	z->out.addr = z->cap.addr = NULL;
}

static int scaler_open(struct cdn_zoom *z)
{
	int r;

	z->fd = find_scaler();
	if (z->fd < 0) {
		r = z->fd;
		z->fd = -1;
		return r;
	}
	r = set_fmt(z->fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, z->width, z->height, &z->out_bpl);
	if (!r)
		r = set_fmt(z->fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, z->width, z->height, &z->cap_bpl);
	if (!r)
		r = map_buf(z->fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, &z->out);
	if (!r)
		r = map_buf(z->fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, &z->cap);
	if (!r && (z->out.len < (size_t)z->out_bpl * z->height * 3 / 2 ||
		   z->cap.len < (size_t)z->cap_bpl * z->height * 3 / 2))
		r = -EINVAL;
	if (r)
		scaler_close(z);
	return r;
}

static int set_crop(struct cdn_zoom *z, struct cdn_rect c)
{
	struct v4l2_selection s;
	uint32_t t;
	int r;

	if (z->streaming) {
		t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
		xioctl(z->fd, VIDIOC_STREAMOFF, &t);
		t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		xioctl(z->fd, VIDIOC_STREAMOFF, &t);
		z->streaming = false;
	}
	memset(&s, 0, sizeof(s));
	s.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	s.target = V4L2_SEL_TGT_CROP;
	s.r.left = c.x;
	s.r.top = c.y;
	s.r.width = (uint32_t)c.w;
	s.r.height = (uint32_t)c.h;
	r = xioctl(z->fd, VIDIOC_S_SELECTION, &s);
	if (r)
		return r;
	/* the driver rounds to even sides; anything else is not the crop asked for */
	if (s.r.left != c.x || s.r.top != c.y || s.r.width != (uint32_t)c.w || s.r.height != (uint32_t)c.h)
		return -ERANGE;
	t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	r = xioctl(z->fd, VIDIOC_STREAMON, &t);
	if (!r) {
		t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		r = xioctl(z->fd, VIDIOC_STREAMON, &t);
	}
	if (r)
		return r;
	z->streaming = true;
	z->crop = c;
	return 0;
}

static void copy_in(uint8_t *dst, uint32_t dbpl, const struct cdn_frame *f)
{
	for (int y = 0; y < f->height; y++)
		memcpy(dst + (size_t)y * dbpl, f->y + (size_t)y * f->stride_y, (size_t)f->width);
	dst += (size_t)dbpl * f->height;
	for (int y = 0; y < f->height / 2; y++)
		memcpy(dst + (size_t)y * dbpl, f->uv + (size_t)y * f->stride_uv, (size_t)f->width);
}

static void copy_out(struct cdn_frame *f, const uint8_t *src, uint32_t sbpl)
{
	for (int y = 0; y < f->height; y++)
		memcpy(f->y + (size_t)y * f->stride_y, src + (size_t)y * sbpl, (size_t)f->width);
	src += (size_t)sbpl * f->height;
	for (int y = 0; y < f->height / 2; y++)
		memcpy(f->uv + (size_t)y * f->stride_uv, src + (size_t)y * sbpl, (size_t)f->width);
}

static int queue(int fd, uint32_t type, size_t used)
{
	struct v4l2_buffer b;
	struct v4l2_plane p;

	memset(&b, 0, sizeof(b));
	memset(&p, 0, sizeof(p));
	b.type = type;
	b.memory = V4L2_MEMORY_MMAP;
	b.index = 0;
	b.m.planes = &p;
	b.length = 1;
	p.bytesused = (uint32_t)used;
	return xioctl(fd, VIDIOC_QBUF, &b);
}

static int dequeue(int fd, uint32_t type, uint32_t *flags)
{
	struct v4l2_buffer b;
	struct v4l2_plane p;
	int r;

	memset(&b, 0, sizeof(b));
	memset(&p, 0, sizeof(p));
	b.type = type;
	b.memory = V4L2_MEMORY_MMAP;
	b.m.planes = &p;
	b.length = 1;
	r = xioctl(fd, VIDIOC_DQBUF, &b);
	if (!r && flags)
		*flags = b.flags;
	return r;
}

static int zoom_scaler(struct cdn_zoom *z, struct cdn_frame *f, struct cdn_rect c)
{
	uint32_t flags = 0;
	int r;

	if (z->fd < 0)
		return -ENODEV;
	if (!z->streaming || memcmp(&c, &z->crop, sizeof(c))) {
		r = set_crop(z, c);
		if (r)
			return r;
	}
	copy_in(z->out.addr, z->out_bpl, f);
	r = queue(z->fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, 0);
	if (!r)
		r = queue(z->fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, (size_t)z->out_bpl * z->height * 3 / 2);
	if (r)
		return r;
	/* both buffers come back before the next frame, whatever happens */
	for (int done = 0; done != 3;) {
		struct pollfd pfd = { .fd = z->fd, .events = POLLIN | POLLOUT | POLLERR };
		int n = poll(&pfd, 1, TIMEOUT_MS);

		if (n <= 0)
			return n ? -errno : -ETIMEDOUT;
		if (pfd.revents & POLLERR)
			return -EIO;
		if (!(done & 1) && !dequeue(z->fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, NULL))
			done |= 1;
		if (!(done & 2) && !dequeue(z->fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, &flags))
			done |= 2;
	}
	if (flags & V4L2_BUF_FLAG_ERROR)
		return -EIO;
	copy_out(f, z->cap.addr, z->cap_bpl);
	z->used = "scaler";
	return 0;
}
#else
static void scaler_close(struct cdn_zoom *z)
{
	z->fd = -1;
}

static int scaler_open(struct cdn_zoom *z)
{
	(void)z;
	return -ENODEV;
}

static int zoom_scaler(struct cdn_zoom *z, struct cdn_frame *f, struct cdn_rect c)
{
	(void)z;
	(void)f;
	(void)c;
	return -ENODEV;
}
#endif

struct cdn_zoom *cdn_zoom_new(int width, int height, enum cdn_zoom_engine engine)
{
	struct cdn_zoom *z;

	if (width < MIN_SIDE || height < MIN_SIDE || (width | height) & 1)
		return NULL;
	z = calloc(1, sizeof(*z));
	if (!z)
		return NULL;
	z->width = width;
	z->height = height;
	z->engine = engine;
	z->fd = -1;
	z->used = "none";
	if (engine != CDN_ZOOM_CPU) {
		int r = scaler_open(z);
		if (r)
			fprintf(stderr, "camera-denoise: zoom: no scaler (%s)%s\n", strerror(-r),
				engine == CDN_ZOOM_SCALER ? ", zoom off" : ", using the CPU");
		else
			fprintf(stderr, "camera-denoise: zoom: on the scaler\n");
	}
	return z;
}

void cdn_zoom_free(struct cdn_zoom *z)
{
	if (!z)
		return;
	scaler_close(z);
	free(z->scratch);
	free(z);
}

int cdn_zoom_apply(struct cdn_zoom *z, struct cdn_frame *f, struct cdn_rect c)
{
	int r;

	if (!z || f->width != z->width || f->height != z->height)
		return -EINVAL;
	c = sane(z, c);
	if (c.w == z->width && c.h == z->height) {
		z->used = "none";
		return 0;
	}
	if (z->fd >= 0) {
		r = zoom_scaler(z, f, c);
		if (!r)
			return 0;
		/* one failure and the scaler is left alone for this stream */
		fprintf(stderr, "camera-denoise: zoom: the scaler failed (%s)%s\n", strerror(-r),
			z->engine == CDN_ZOOM_SCALER ? ", zoom off" : ", using the CPU");
		scaler_close(z);
	}
	if (z->engine == CDN_ZOOM_SCALER) {
		z->used = "none";
		return -ENODEV;
	}
	return zoom_cpu(z, f, c);
}

const char *cdn_zoom_used(const struct cdn_zoom *z)
{
	return z ? z->used : "none";
}
