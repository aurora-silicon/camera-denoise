// SPDX-License-Identifier: MIT
/*
 * Temporal backend on the video encoder's motion-compensated temporal filter
 * (MCTF), the hardware macOS runs every built-in camera frame through.
 *
 * The AVE driver exposes the filter as a V4L2 memory-to-memory device
 * ("Apple AVE MCTF"): NV12 frames go in on the output queue and filtered
 * NV12 frames come out on the capture queue. The hardware keeps its own
 * reference history and aligns it to each new frame with motion vectors, so
 * moving objects are averaged along their motion instead of leaving trails
 * or falling back to raw noise. Stopping the queues drops the history.
 *
 * The level control follows what macOS asks for (see mctf_level.h).
 */
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/videodev2.h>

#include "../backend.h"
#include "mctf_level.h"

#define NBUF 4
#define CARD "Apple AVE MCTF"
#define LEVEL_CTRL "MCTF Level"
#define TIMEOUT_MS 200

struct qbuf {
	void *p;
	size_t len;
};

struct ave {
	int fd;
	int w, h;
	bool mplane;
	uint32_t out_type, cap_type;
	uint32_t out_bpl, cap_bpl, out_size, cap_size;
	/* chroma plane offset: the driver may pad the luma height (1080 -> 1088) */
	size_t out_uv, cap_uv;
	struct qbuf out[NBUF], cap[NBUF];
	uint32_t level_id;	/* 0 when the driver has no level control */
	int level;		/* last level set */
	struct cdn_mctf_level lv;
	bool streaming;
	uint32_t next_out;
	float sigma;
};

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;

	do
		r = ioctl(fd, req, arg);
	while (r < 0 && errno == EINTR);
	return r;
}

static bool is_mctf(int fd, bool *mplane)
{
	struct v4l2_capability cap = { 0 };
	uint32_t caps;

	if (xioctl(fd, VIDIOC_QUERYCAP, &cap))
		return false;
	caps = cap.capabilities & V4L2_CAP_DEVICE_CAPS ? cap.device_caps : cap.capabilities;
	if (strncmp((const char *)cap.card, CARD, sizeof(cap.card)))
		return false;
	if (caps & V4L2_CAP_VIDEO_M2M_MPLANE)
		*mplane = true;
	else if (caps & V4L2_CAP_VIDEO_M2M)
		*mplane = false;
	else
		return false;
	return (caps & V4L2_CAP_STREAMING) != 0;
}

static int open_mctf(bool *mplane)
{
	glob_t g;
	int fd = -1;

	if (glob("/dev/video*", 0, NULL, &g))
		return -1;
	for (size_t i = 0; i < g.gl_pathc && fd < 0; i++) {
		int f = open(g.gl_pathv[i], O_RDWR | O_NONBLOCK | O_CLOEXEC);

		if (f < 0)
			continue;
		if (is_mctf(f, mplane))
			fd = f;
		else
			close(f);
	}
	globfree(&g);
	return fd;
}

static int set_fmt(struct ave *a, uint32_t type, uint32_t *bpl, uint32_t *size)
{
	struct v4l2_format f = { .type = type };

	if (a->mplane) {
		f.fmt.pix_mp.width = (uint32_t)a->w;
		f.fmt.pix_mp.height = (uint32_t)a->h;
		f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
		f.fmt.pix_mp.num_planes = 1;
		f.fmt.pix_mp.field = V4L2_FIELD_NONE;
	} else {
		f.fmt.pix.width = (uint32_t)a->w;
		f.fmt.pix.height = (uint32_t)a->h;
		f.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
		f.fmt.pix.field = V4L2_FIELD_NONE;
	}
	if (xioctl(a->fd, VIDIOC_S_FMT, &f))
		return -errno;
	if (a->mplane) {
		if (f.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12 ||
		    f.fmt.pix_mp.width != (uint32_t)a->w || f.fmt.pix_mp.height != (uint32_t)a->h ||
		    f.fmt.pix_mp.num_planes != 1)
			return -EINVAL;
		*bpl = f.fmt.pix_mp.plane_fmt[0].bytesperline;
		*size = f.fmt.pix_mp.plane_fmt[0].sizeimage;
	} else {
		if (f.fmt.pix.pixelformat != V4L2_PIX_FMT_NV12 ||
		    f.fmt.pix.width != (uint32_t)a->w || f.fmt.pix.height != (uint32_t)a->h)
			return -EINVAL;
		*bpl = f.fmt.pix.bytesperline;
		*size = f.fmt.pix.sizeimage;
	}
	/* NV12 in one plane: chroma follows luma at the same stride */
	if (*bpl < (uint32_t)a->w || *size < *bpl * (uint32_t)a->h * 3 / 2)
		return -EINVAL;
	return 0;
}

static void fill_buf(const struct ave *a, struct v4l2_buffer *b, struct v4l2_plane *pl,
		     uint32_t type, uint32_t idx)
{
	memset(b, 0, sizeof(*b));
	b->type = type;
	b->memory = V4L2_MEMORY_MMAP;
	b->index = idx;
	if (a->mplane) {
		memset(pl, 0, sizeof(*pl));
		b->m.planes = pl;
		b->length = 1;
	}
}

static int map_bufs(struct ave *a, uint32_t type, struct qbuf *q)
{
	struct v4l2_requestbuffers rb = { .count = NBUF, .type = type,
					  .memory = V4L2_MEMORY_MMAP };

	if (xioctl(a->fd, VIDIOC_REQBUFS, &rb))
		return -errno;
	if (rb.count < NBUF)
		return -ENOMEM;
	for (uint32_t i = 0; i < NBUF; i++) {
		struct v4l2_buffer b;
		struct v4l2_plane pl;
		size_t len;
		off_t off;

		fill_buf(a, &b, &pl, type, i);
		if (xioctl(a->fd, VIDIOC_QUERYBUF, &b))
			return -errno;
		len = a->mplane ? pl.length : b.length;
		off = a->mplane ? (off_t)pl.m.mem_offset : (off_t)b.m.offset;
		q[i].p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, a->fd, off);
		if (q[i].p == MAP_FAILED) {
			q[i].p = NULL;
			return -errno;
		}
		q[i].len = len;
	}
	return 0;
}

static uint32_t find_ctrl(int fd, const char *name)
{
	struct v4l2_query_ext_ctrl qc = { .id = V4L2_CTRL_FLAG_NEXT_CTRL };

	while (!xioctl(fd, VIDIOC_QUERY_EXT_CTRL, &qc)) {
		if (!strncmp(qc.name, name, sizeof(qc.name)) &&
		    !(qc.flags & V4L2_CTRL_FLAG_DISABLED))
			return qc.id;
		qc.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
	}
	return 0;
}

static void set_level(struct ave *a, int level)
{
	struct v4l2_control c = { .id = a->level_id, .value = level };

	if (!a->level_id || level == a->level)
		return;
	if (!xioctl(a->fd, VIDIOC_S_CTRL, &c))
		a->level = level;
}

static int qcap(struct ave *a, uint32_t i)
{
	struct v4l2_buffer b;
	struct v4l2_plane pl;

	fill_buf(a, &b, &pl, a->cap_type, i);
	return xioctl(a->fd, VIDIOC_QBUF, &b) ? -errno : 0;
}

static void stream(struct ave *a, bool on)
{
	uint32_t t;

	if (on == a->streaming)
		return;
	t = a->out_type;
	xioctl(a->fd, on ? VIDIOC_STREAMON : VIDIOC_STREAMOFF, &t);
	t = a->cap_type;
	xioctl(a->fd, on ? VIDIOC_STREAMON : VIDIOC_STREAMOFF, &t);
	a->streaming = on;
	if (on)
		for (uint32_t i = 0; i < NBUF; i++)
			qcap(a, i);
	a->next_out = 0;
	cdn_mctf_level_reset(&a->lv);
}

static void ave_destroy(void *state)
{
	struct ave *a = state;

	if (!a)
		return;
	if (a->fd >= 0) {
		stream(a, false);
		for (int i = 0; i < NBUF; i++) {
			if (a->out[i].p)
				munmap(a->out[i].p, a->out[i].len);
			if (a->cap[i].p)
				munmap(a->cap[i].p, a->cap[i].len);
		}
		close(a->fd);
	}
	free(a);
}

static void *ave_create(int width, int height, int threads)
{
	struct ave *a = calloc(1, sizeof(*a));

	(void)threads;
	if (!a)
		return NULL;
	a->w = width;
	a->h = height;
	a->level = -1;
	a->sigma = -1.0f;
	/* J700 sensor gain levels (1/256 steps), as the low-light trigger uses */
	cdn_mctf_level_init(&a->lv, 2304, 1792);
	a->fd = open_mctf(&a->mplane);
	if (a->fd < 0)
		goto fail;
	a->out_type = a->mplane ? V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE : V4L2_BUF_TYPE_VIDEO_OUTPUT;
	a->cap_type = a->mplane ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (set_fmt(a, a->out_type, &a->out_bpl, &a->out_size) ||
	    set_fmt(a, a->cap_type, &a->cap_bpl, &a->cap_size) ||
	    map_bufs(a, a->out_type, a->out) || map_bufs(a, a->cap_type, a->cap))
		goto fail;
	/*
	 * One NV12 plane: luma rows (possibly padded), then chroma at half
	 * the luma height, so chroma starts at two thirds of the image.
	 */
	a->out_uv = (size_t)a->out_size / 3 * 2 / a->out_bpl * a->out_bpl;
	a->cap_uv = (size_t)a->cap_size / 3 * 2 / a->cap_bpl * a->cap_bpl;
	if (a->out_uv < (size_t)a->out_bpl * a->h || a->cap_uv < (size_t)a->cap_bpl * a->h)
		goto fail;
	a->level_id = find_ctrl(a->fd, LEVEL_CTRL);
	return a;
fail:
	ave_destroy(a);
	return NULL;
}

static void copy_in(uint8_t *dst, uint32_t bpl, size_t uv, int h, const struct cdn_frame *in)
{
	for (int y = 0; y < h; y++)
		memcpy(dst + (size_t)y * bpl, in->y + (size_t)y * in->stride_y, (size_t)in->width);
	dst += uv;
	for (int y = 0; y < h / 2; y++)
		memcpy(dst + (size_t)y * bpl, in->uv + (size_t)y * in->stride_uv, (size_t)in->width);
}

/* out = in + s * (f - in), row by row; s == 1 is a plain copy */
static void copy_out(struct cdn_frame *out, const uint8_t *src, uint32_t bpl, size_t uv,
		     const struct cdn_frame *in, float s)
{
	int w = out->width, h = out->height;
	int k = (int)(s * 256.0f + 0.5f);

	for (int p = 0; p < 2; p++) {
		int rows = p ? h / 2 : h;
		const uint8_t *f = src + (p ? uv : 0);
		uint8_t *o = p ? out->uv : out->y;
		const uint8_t *i0 = p ? in->uv : in->y;
		int so = p ? out->stride_uv : out->stride_y, si = p ? in->stride_uv : in->stride_y;

		for (int y = 0; y < rows; y++) {
			const uint8_t *fr = f + (size_t)y * bpl, *ir = i0 + (size_t)y * si;
			uint8_t *or = o + (size_t)y * so;

			if (k >= 256) {
				memcpy(or, fr, (size_t)w);
				continue;
			}
			for (int x = 0; x < w; x++)
				or[x] = (uint8_t)(ir[x] + (((fr[x] - ir[x]) * k + 128) >> 8));
		}
	}
}

static int wait_dq(struct ave *a, uint32_t type, uint32_t *idx)
{
	struct v4l2_buffer b;
	struct v4l2_plane pl;
	struct pollfd p = { .fd = a->fd, .events = POLLIN | POLLOUT };

	for (;;) {
		fill_buf(a, &b, &pl, type, 0);
		if (!xioctl(a->fd, VIDIOC_DQBUF, &b)) {
			*idx = b.index;
			return (b.flags & V4L2_BUF_FLAG_ERROR) ? -EIO : 0;
		}
		if (errno != EAGAIN)
			return -errno;
		int r = poll(&p, 1, TIMEOUT_MS);

		if (r == 0)
			return -ETIMEDOUT;
		if (r < 0 && errno != EINTR)
			return -errno;
	}
}

static int ave_process(void *state, const struct cdn_frame *in, struct cdn_frame *out,
		       const struct cdn_frame_info *info, float strength)
{
	struct ave *a = state;
	struct v4l2_buffer b;
	struct v4l2_plane pl;
	uint32_t oi, ci;
	int r;

	if (in->width != a->w || in->height != a->h)
		return -EINVAL;
	stream(a, true);
	set_level(a, cdn_mctf_level_step(&a->lv, info ? info->gain : 0));

	oi = a->next_out;
	copy_in(a->out[oi].p, a->out_bpl, a->out_uv, a->h, in);
	fill_buf(a, &b, &pl, a->out_type, oi);
	if (a->mplane)
		pl.bytesused = a->out_size;
	else
		b.bytesused = a->out_size;
	if (info && info->pts_ns) {
		b.timestamp.tv_sec = (time_t)(info->pts_ns / 1000000000u);
		b.timestamp.tv_usec = (suseconds_t)(info->pts_ns % 1000000000u / 1000u);
	}
	if (xioctl(a->fd, VIDIOC_QBUF, &b))
		return -errno;
	a->next_out = (oi + 1) % NBUF;

	r = wait_dq(a, a->cap_type, &ci);
	if (r == 0)
		copy_out(out, a->cap[ci].p, a->cap_bpl, a->cap_uv, in, strength);
	if (r == 0 || r == -EIO)
		qcap(a, ci);
	/* get the output buffer back so the ring never runs dry */
	if (wait_dq(a, a->out_type, &oi) == -ETIMEDOUT || r == -ETIMEDOUT) {
		/* the hardware stopped answering: drop the session, start over next frame */
		stream(a, false);
		return -ETIMEDOUT;
	}
	return r;
}

static void ave_reset(void *state)
{
	stream(state, false);
}

static float ave_noise_sigma(void *state)
{
	return ((struct ave *)state)->sigma;
}

static const struct cdn_backend_ops ops = {
	.abi = CDN_BACKEND_ABI,
	.name = "ave",
	.create = ave_create,
	.process = ave_process,
	.reset = ave_reset,
	.noise_sigma = ave_noise_sigma,
	.destroy = ave_destroy,
};

__attribute__((visibility("default"))) const struct cdn_backend_ops *cdn_backend_entry(void)
{
	return &ops;
}
