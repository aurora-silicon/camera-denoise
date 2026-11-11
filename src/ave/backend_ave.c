// SPDX-License-Identifier: MIT
/*
 * Temporal backend on the video encoder's motion-compensated temporal filter
 * (MCTF), the hardware macOS uses for its temporal noise filter.
 *
 * The AVE driver exposes the filter as a V4L2 memory-to-memory device
 * ("Apple AVE MCTF"). It works on Apple's lossless compressed 4:2:0 form
 * (V4L2_PIX_FMT_APPLE_L8V0), which the platform scaler reads and writes, so
 * a frame goes NV12 -> scaler -> MCTF -> scaler -> NV12 with dmabufs between
 * the devices and no conversion on the CPU. Where the scaler lacks the
 * lossless form, the MCTF device takes NV12 itself (its driver converts on
 * the CPU). The hardware keeps its own motion-compensated history; stopping
 * the queues drops it.
 *
 * The session runs at full strength with the level left to the session, as
 * the VideoToolbox filter does at strength 1.0; the result matches macOS's
 * output for the same frames.
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

#ifndef V4L2_PIX_FMT_APPLE_L8V0
#define V4L2_PIX_FMT_APPLE_L8V0 v4l2_fourcc('A', 'L', '8', 'V')
#endif

#define NBUF 2
#define MCTF_CARD "Apple AVE MCTF"
#define STRENGTH_CTRL "MCTF Strength"
#define LEVEL_CTRL "MCTF Level"
/* VideoToolbox strength 1.0 */
#define SESSION_STRENGTH 24
#define TIMEOUT_MS 200
#define OUTQ V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
#define CAPQ V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE

/* One m2m context: what goes in and comes out, and its buffers. */
struct node {
	int fd;
	uint32_t out_mem, cap_mem;
	uint32_t out_bpl, cap_bpl, out_size, cap_size;
	void *out[NBUF], *cap[NBUF];
	size_t out_len[NBUF], cap_len[NBUF];
	int cap_fd[NBUF];	/* exported CAPTURE buffers, -1 when not exported */
};

struct ave {
	int w, h;
	bool chain;		/* scaler -> MCTF -> scaler, else MCTF on NV12 */
	struct node in, mctf, out;	/* in/out: scaler contexts (chain only) */
	bool streaming;
	uint32_t next;
};

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;

	do
		r = ioctl(fd, req, arg);
	while (r < 0 && errno == EINTR);
	return r;
}

static bool supports(int fd, uint32_t type, uint32_t fourcc)
{
	struct v4l2_fmtdesc d = { .type = type };

	for (d.index = 0; !xioctl(fd, VIDIOC_ENUM_FMT, &d); d.index++)
		if (d.pixelformat == fourcc)
			return true;
	return false;
}

/* The first m2m device whose card matches (or that converts to lossless). */
static int open_dev(const char *card, bool lossless)
{
	glob_t g;
	int fd = -1;

	if (glob("/dev/video*", 0, NULL, &g))
		return -1;
	for (size_t i = 0; i < g.gl_pathc && fd < 0; i++) {
		struct v4l2_capability cap = { 0 };
		int f = open(g.gl_pathv[i], O_RDWR | O_NONBLOCK | O_CLOEXEC);

		if (f < 0)
			continue;
		if (!xioctl(f, VIDIOC_QUERYCAP, &cap) &&
		    (cap.device_caps & V4L2_CAP_VIDEO_M2M_MPLANE) &&
		    (card ? !strcmp((const char *)cap.card, card) :
			    lossless && strcmp((const char *)cap.card, MCTF_CARD) &&
			    supports(f, OUTQ, V4L2_PIX_FMT_APPLE_L8V0) &&
			    supports(f, CAPQ, V4L2_PIX_FMT_APPLE_L8V0) &&
			    supports(f, OUTQ, V4L2_PIX_FMT_NV12)))
			fd = f;
		else
			close(f);
	}
	globfree(&g);
	return fd;
}

static int set_fmt(struct node *n, uint32_t type, uint32_t fourcc, int w, int h)
{
	struct v4l2_format f = { .type = type };

	f.fmt.pix_mp.width = (uint32_t)w;
	f.fmt.pix_mp.height = (uint32_t)h;
	f.fmt.pix_mp.pixelformat = fourcc;
	f.fmt.pix_mp.num_planes = 1;
	f.fmt.pix_mp.field = V4L2_FIELD_NONE;
	if (xioctl(n->fd, VIDIOC_S_FMT, &f))
		return -errno;
	if (f.fmt.pix_mp.pixelformat != fourcc || f.fmt.pix_mp.num_planes != 1 ||
	    f.fmt.pix_mp.width != (uint32_t)w || f.fmt.pix_mp.height != (uint32_t)h)
		return -EINVAL;
	if (type == OUTQ) {
		n->out_bpl = f.fmt.pix_mp.plane_fmt[0].bytesperline;
		n->out_size = f.fmt.pix_mp.plane_fmt[0].sizeimage;
	} else {
		n->cap_bpl = f.fmt.pix_mp.plane_fmt[0].bytesperline;
		n->cap_size = f.fmt.pix_mp.plane_fmt[0].sizeimage;
	}
	return 0;
}

/* Request buffers; map MMAP ones, export the CAPTURE ones when asked. */
static int setup_bufs(struct node *n, uint32_t type, uint32_t mem, bool export)
{
	struct v4l2_requestbuffers rb = { .count = NBUF, .type = type, .memory = mem };

	if (xioctl(n->fd, VIDIOC_REQBUFS, &rb) || rb.count < NBUF)
		return -ENOMEM;
	for (uint32_t i = 0; mem == V4L2_MEMORY_MMAP && i < NBUF; i++) {
		struct v4l2_plane pl = { 0 };
		struct v4l2_buffer b = { .type = type, .memory = mem, .index = i,
					 .m.planes = &pl, .length = 1 };
		void *p;

		if (xioctl(n->fd, VIDIOC_QUERYBUF, &b))
			return -errno;
		p = mmap(NULL, pl.length, PROT_READ | PROT_WRITE, MAP_SHARED, n->fd,
			 pl.m.mem_offset);
		if (p == MAP_FAILED)
			return -errno;
		if (type == OUTQ) {
			n->out[i] = p;
			n->out_len[i] = pl.length;
		} else {
			n->cap[i] = p;
			n->cap_len[i] = pl.length;
		}
		if (export) {
			struct v4l2_exportbuffer e = { .type = type, .index = i,
						       .flags = O_RDWR | O_CLOEXEC };

			if (xioctl(n->fd, VIDIOC_EXPBUF, &e))
				return -errno;
			n->cap_fd[i] = e.fd;
		}
	}
	if (type == OUTQ)
		n->out_mem = mem;
	else
		n->cap_mem = mem;
	return 0;
}

static void set_ctrl(int fd, const char *name, int value)
{
	struct v4l2_query_ext_ctrl qc = { .id = V4L2_CTRL_FLAG_NEXT_CTRL };

	while (!xioctl(fd, VIDIOC_QUERY_EXT_CTRL, &qc)) {
		if (!strcmp(qc.name, name)) {
			struct v4l2_control c = { .id = qc.id, .value = value };

			xioctl(fd, VIDIOC_S_CTRL, &c);
			return;
		}
		qc.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
	}
}

static void stream_node(struct node *n, bool on)
{
	int t = OUTQ;

	if (n->fd < 0)
		return;
	xioctl(n->fd, on ? VIDIOC_STREAMON : VIDIOC_STREAMOFF, &t);
	t = CAPQ;
	xioctl(n->fd, on ? VIDIOC_STREAMON : VIDIOC_STREAMOFF, &t);
}

static void stream(struct ave *a, bool on)
{
	if (a->streaming == on)
		return;
	if (on) {
		/* the session strength applies at STREAMON */
		set_ctrl(a->mctf.fd, STRENGTH_CTRL, SESSION_STRENGTH);
		set_ctrl(a->mctf.fd, LEVEL_CTRL, -1);
	}
	stream_node(&a->in, on);
	stream_node(&a->mctf, on);
	stream_node(&a->out, on);
	a->streaming = on;
}

static void free_node(struct node *n)
{
	if (n->fd < 0)
		return;
	stream_node(n, false);
	for (int i = 0; i < NBUF; i++) {
		if (n->out[i])
			munmap(n->out[i], n->out_len[i]);
		if (n->cap[i])
			munmap(n->cap[i], n->cap_len[i]);
		if (n->cap_fd[i] >= 0)
			close(n->cap_fd[i]);
	}
	close(n->fd);
	n->fd = -1;
}

static void ave_destroy(void *state)
{
	struct ave *a = state;

	if (!a)
		return;
	free_node(&a->in);
	free_node(&a->mctf);
	free_node(&a->out);
	free(a);
}

static void init_node(struct node *n)
{
	memset(n, 0, sizeof(*n));
	n->fd = -1;
	for (int i = 0; i < NBUF; i++)
		n->cap_fd[i] = -1;
}

static int setup_chain(struct ave *a)
{
	const uint32_t l8 = V4L2_PIX_FMT_APPLE_L8V0, nv12 = V4L2_PIX_FMT_NV12;

	a->in.fd = open_dev(NULL, true);
	a->out.fd = a->in.fd >= 0 ? open_dev(NULL, true) : -1;
	if (a->out.fd < 0 || !supports(a->mctf.fd, OUTQ, l8))
		return -ENODEV;
	if (set_fmt(&a->in, OUTQ, nv12, a->w, a->h) || set_fmt(&a->in, CAPQ, l8, a->w, a->h) ||
	    set_fmt(&a->mctf, OUTQ, l8, a->w, a->h) || set_fmt(&a->mctf, CAPQ, l8, a->w, a->h) ||
	    set_fmt(&a->out, OUTQ, l8, a->w, a->h) || set_fmt(&a->out, CAPQ, nv12, a->w, a->h))
		return -EINVAL;
	if (setup_bufs(&a->in, OUTQ, V4L2_MEMORY_MMAP, false) ||
	    setup_bufs(&a->in, CAPQ, V4L2_MEMORY_MMAP, true) ||
	    setup_bufs(&a->mctf, OUTQ, V4L2_MEMORY_DMABUF, false) ||
	    setup_bufs(&a->mctf, CAPQ, V4L2_MEMORY_MMAP, true) ||
	    setup_bufs(&a->out, OUTQ, V4L2_MEMORY_DMABUF, false) ||
	    setup_bufs(&a->out, CAPQ, V4L2_MEMORY_MMAP, false))
		return -ENOMEM;
	return 0;
}

static void *ave_create(int width, int height, int threads)
{
	struct ave *a = calloc(1, sizeof(*a));
	const uint32_t nv12 = V4L2_PIX_FMT_NV12;

	(void)threads;
	if (!a)
		return NULL;
	a->w = width;
	a->h = height;
	init_node(&a->in);
	init_node(&a->mctf);
	init_node(&a->out);
	a->mctf.fd = open_dev(MCTF_CARD, false);
	if (a->mctf.fd < 0)
		goto fail;
	a->chain = !getenv("CDN_AVE_NO_SCALER") && !setup_chain(a);
	if (!a->chain) {
		/* no lossless scaler: the MCTF device takes NV12 itself */
		free_node(&a->in);
		free_node(&a->out);
		close(a->mctf.fd);
		init_node(&a->mctf);
		a->mctf.fd = open_dev(MCTF_CARD, false);
		if (a->mctf.fd < 0 ||
		    set_fmt(&a->mctf, OUTQ, nv12, width, height) ||
		    set_fmt(&a->mctf, CAPQ, nv12, width, height) ||
		    setup_bufs(&a->mctf, OUTQ, V4L2_MEMORY_MMAP, false) ||
		    setup_bufs(&a->mctf, CAPQ, V4L2_MEMORY_MMAP, false))
			goto fail;
	}
	return a;
fail:
	ave_destroy(a);
	return NULL;
}

/* NV12 rows into or out of one single-plane NV12 buffer of @bpl bytes per row. */
static void put_nv12(uint8_t *dst, uint32_t bpl, int h, const struct cdn_frame *in)
{
	uint8_t *uv = dst + (size_t)bpl * (size_t)h;

	for (int y = 0; y < h; y++)
		memcpy(dst + (size_t)y * bpl, in->y + (size_t)y * in->stride_y, (size_t)in->width);
	for (int y = 0; y < h / 2; y++)
		memcpy(uv + (size_t)y * bpl, in->uv + (size_t)y * in->stride_uv, (size_t)in->width);
}

static void get_nv12(struct cdn_frame *out, const uint8_t *src, uint32_t bpl, int h,
		     const struct cdn_frame *in, float strength)
{
	const uint8_t *uv = src + (size_t)bpl * (size_t)h;
	int k = (int)(strength * 256.0f + 0.5f);

	for (int y = 0; y < h; y++) {
		const uint8_t *s = src + (size_t)y * bpl, *o = in->y + (size_t)y * in->stride_y;
		uint8_t *d = out->y + (size_t)y * out->stride_y;

		if (k >= 256)
			memcpy(d, s, (size_t)in->width);
		else
			for (int x = 0; x < in->width; x++)
				d[x] = (uint8_t)(o[x] + (((s[x] - o[x]) * k) >> 8));
	}
	for (int y = 0; y < h / 2; y++) {
		const uint8_t *s = uv + (size_t)y * bpl, *o = in->uv + (size_t)y * in->stride_uv;
		uint8_t *d = out->uv + (size_t)y * out->stride_uv;

		if (k >= 256)
			memcpy(d, s, (size_t)in->width);
		else
			for (int x = 0; x < in->width; x++)
				d[x] = (uint8_t)(o[x] + (((s[x] - o[x]) * k) >> 8));
	}
}

static int qbuf(struct node *n, uint32_t type, uint32_t i, uint32_t used, int dmabuf, uint64_t pts)
{
	struct v4l2_plane pl = { .bytesused = used };
	struct v4l2_buffer b = { .type = type, .index = i, .m.planes = &pl, .length = 1,
				 .memory = type == OUTQ ? n->out_mem : n->cap_mem };

	if (b.memory == V4L2_MEMORY_DMABUF) {
		pl.m.fd = dmabuf;
		pl.length = used;
	}
	b.timestamp.tv_sec = (time_t)(pts / 1000000000u);
	b.timestamp.tv_usec = (suseconds_t)(pts % 1000000000u / 1000u);
	return xioctl(n->fd, VIDIOC_QBUF, &b) ? -errno : 0;
}

/* Wait for a buffer of @type; -EIO for one the driver marked broken. */
static int dqbuf(struct node *n, uint32_t type, uint32_t *idx)
{
	struct pollfd p = { .fd = n->fd, .events = POLLIN | POLLOUT };

	for (;;) {
		struct v4l2_plane pl = { 0 };
		struct v4l2_buffer b = { .type = type, .m.planes = &pl, .length = 1,
					 .memory = type == OUTQ ? n->out_mem : n->cap_mem };

		if (!xioctl(n->fd, VIDIOC_DQBUF, &b)) {
			*idx = b.index;
			return b.flags & V4L2_BUF_FLAG_ERROR ? -EIO : 0;
		}
		if (errno != EAGAIN)
			return -errno;
		if (poll(&p, 1, TIMEOUT_MS) <= 0)
			return -ETIMEDOUT;
	}
}

/* One frame through one node: @src_fd is a dmabuf for a DMABUF queue. */
static int run_node(struct node *n, uint32_t i, uint32_t used, int src_fd, uint64_t pts,
		    uint32_t *ci)
{
	uint32_t oi;
	int r, r2;

	r = qbuf(n, CAPQ, i, 0, -1, 0);
	if (!r)
		r = qbuf(n, OUTQ, i, used, src_fd, pts);
	if (r)
		return r;
	r = dqbuf(n, CAPQ, ci);
	r2 = dqbuf(n, OUTQ, &oi);
	return r ? r : r2;
}

static int ave_process(void *state, const struct cdn_frame *in, struct cdn_frame *out,
		       const struct cdn_frame_info *info, float strength)
{
	struct ave *a = state;
	uint64_t pts = info ? info->pts_ns : 0;
	uint32_t i, ci, mi, bi;
	int r;

	if (in->width != a->w || in->height != a->h)
		return -EINVAL;
	stream(a, true);
	i = a->next;
	a->next = (i + 1) % NBUF;
	if (!a->chain) {
		put_nv12(a->mctf.out[i], a->mctf.out_bpl, a->h, in);
		r = run_node(&a->mctf, i, a->mctf.out_size, -1, pts, &ci);
		if (!r)
			get_nv12(out, a->mctf.cap[ci], a->mctf.cap_bpl, a->h, in, strength);
	} else {
		put_nv12(a->in.out[i], a->in.out_bpl, a->h, in);
		r = run_node(&a->in, i, a->in.out_size, -1, pts, &ci);
		if (!r)
			r = run_node(&a->mctf, i, a->in.cap_size, a->in.cap_fd[ci], pts, &mi);
		if (!r)
			r = run_node(&a->out, i, a->mctf.cap_size, a->mctf.cap_fd[mi], pts, &bi);
		if (!r)
			get_nv12(out, a->out.cap[bi], a->out.cap_bpl, a->h, in, strength);
	}
	if (r)
		/* drop the session; the next frame starts a new one */
		stream(a, false);
	return r;
}

static void ave_reset(void *state)
{
	stream(state, false);
}

static float ave_noise_sigma(void *state)
{
	(void)state;
	return -1.0f;
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
