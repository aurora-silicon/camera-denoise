// SPDX-License-Identifier: MIT
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/videodev2.h>

#include "gain.h"

static bool ro_volatile(int fd, uint32_t id)
{
	struct v4l2_queryctrl q = { .id = id };

	if (ioctl(fd, VIDIOC_QUERYCTRL, &q) || (q.flags & V4L2_CTRL_FLAG_DISABLED))
		return false;
	/* a settable gain is a manual control, not a report of what AE chose */
	return (q.flags & V4L2_CTRL_FLAG_READ_ONLY) && (q.flags & V4L2_CTRL_FLAG_VOLATILE);
}

bool cdn_gain_open(struct cdn_gain *g, const char *path)
{
	memset(g, 0, sizeof(*g));
	g->fd = -1;
	if (!path || !*path)
		return false;
	g->fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (g->fd < 0)
		return false;
	g->have_gain = ro_volatile(g->fd, V4L2_CID_GAIN);
	g->have_exposure = ro_volatile(g->fd, V4L2_CID_EXPOSURE_ABSOLUTE);
	if (!g->have_gain) {
		close(g->fd);
		g->fd = -1;
	}
	return g->have_gain;
}

uint32_t cdn_gain_read(struct cdn_gain *g, uint32_t *exposure_us)
{
	struct v4l2_control c = { .id = V4L2_CID_GAIN };

	if (exposure_us)
		*exposure_us = 0;
	if (g->fd < 0 || ioctl(g->fd, VIDIOC_G_CTRL, &c) || c.value <= 0)
		return 0;
	if (exposure_us && g->have_exposure) {
		struct v4l2_control e = { .id = V4L2_CID_EXPOSURE_ABSOLUTE };
		if (!ioctl(g->fd, VIDIOC_G_CTRL, &e) && e.value > 0)
			*exposure_us = (uint32_t)e.value * 100;
	}
	return (uint32_t)c.value;
}

void cdn_gain_close(struct cdn_gain *g)
{
	if (g->fd >= 0)
		close(g->fd);
	g->fd = -1;
}

#define NT_STEP 4
#define NT_BX 16
#define NT_BY 9

static int cmp_u16(const void *a, const void *b)
{
	return (int)*(const uint16_t *)a - (int)*(const uint16_t *)b;
}

float cdn_noise_track(struct cdn_noise_tracker *t, const uint8_t *y, int stride,
		      int width, int height)
{
	int w = width / NT_STEP, h = height / NT_STEP;
	uint16_t meds[NT_BX * NT_BY];
	int nm = 0;

	if (!t->prev || t->w != w || t->h != h) {
		free(t->prev);
		t->prev = malloc((size_t)w * h);
		t->w = w;
		t->h = h;
		t->valid = false;
		if (!t->prev)
			return -1;
	}
	int bw = w / NT_BX, bh = h / NT_BY;
	for (int by = 0; by < NT_BY; by++)
		for (int bx = 0; bx < NT_BX; bx++) {
			uint32_t bins[256] = { 0 }, n = 0, sum = 0;
			for (int j = by * bh; j < (by + 1) * bh; j++) {
				const uint8_t *row = y + (size_t)j * NT_STEP * stride;
				uint8_t *p = t->prev + (size_t)j * w;
				for (int i = bx * bw; i < (bx + 1) * bw; i++) {
					int v = row[i * NT_STEP];
					if (t->valid)
						bins[abs(v - p[i])]++;
					sum += (uint32_t)v;
					p[i] = (uint8_t)v;
					n++;
				}
			}
			if (!t->valid || !n)
				continue;
			uint32_t mean = sum / n;
			if (mean < 20 || mean > 245)
				continue;
			/* interpolated median, x16 */
			uint32_t acc = 0, half = n / 2;
			for (int k = 0; k < 256; k++) {
				acc += bins[k];
				if (acc > half) {
					uint32_t below = acc - bins[k];
					meds[nm++] = (uint16_t)(k * 16 + (int)(((half - below) * 16) / bins[k]));
					break;
				}
			}
		}
	bool was_valid = t->valid;
	t->valid = true;
	if (!was_valid || nm < 4)
		return -1;
	qsort(meds, (size_t)nm, sizeof(meds[0]), cmp_u16);
	/*
	 * median |d| of two noisy frames -> per-frame std-like scale: / sqrt(2)
	 * for the two frames, x 1.75 for this heavy-tailed camera noise
	 * (J700 calibration), which matches the CPU backend's sigma.
	 */
	return meds[nm / 2] / 16.0f * 1.2374f;
}

void cdn_noise_tracker_reset(struct cdn_noise_tracker *t)
{
	t->valid = false;
}

void cdn_noise_tracker_free(struct cdn_noise_tracker *t)
{
	free(t->prev);
	t->prev = NULL;
	t->valid = false;
}
