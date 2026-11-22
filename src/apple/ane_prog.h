/* SPDX-License-Identifier: MIT */
#ifndef CDN_APPLE_ANE_PROG_H
#define CDN_APPLE_ANE_PROG_H
/*
 * One Neural Engine program, compiled for this Mac's engine and installed from the user's own macOS
 * (install-apple-effects), run through the Linux accel/ane driver (ABI 3) with the four host-built sections next
 * to it. The driver validates the program and reports its surfaces (size and direction, in the program's order);
 * every surface is mapped and requests run in place. The caller knows its network's interface and gives each
 * surface its shape (apple_prog_layout); the fp16 strides follow from the size the driver reports.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APPLE_PROG_SURFACES 16

struct apple_tensor {
	uint32_t direction;           /* 1 input, 2 output (from the driver) */
	uint32_t c, h, w;             /* given by apple_prog_layout */
	uint64_t row, plane;          /* bytes; fp16 elements */
};

struct apple_prog {
	int fd;
	unsigned count;
	struct apple_tensor t[APPLE_PROG_SURFACES];
	uint64_t size[APPLE_PROG_SURFACES];
	void *map[APPLE_PROG_SURFACES];
	size_t map_len[APPLE_PROG_SURFACES];
};

/*
 * Load dir/model.hwx with dir/{generic,operation,procedure,text-properties}.bin on the first ANE node whose
 * driver accepts it. 0, -ENOENT (files missing), -ENODEV (no node took it) or another -errno.
 */
int apple_prog_open(struct apple_prog *p, const char *dir);
void apple_prog_close(struct apple_prog *p);
/* The index of the k-th surface (from 0) of a direction, -1 if there is none. */
int apple_prog_surface(const struct apple_prog *p, uint32_t direction, unsigned k);
/*
 * Give surface i the shape c x h x w (fp16): rows and planes padded alike, so that size = c * plane and
 * plane = h * row, row >= 2 w. Returns 0, or -1 when the reported size does not fit that shape.
 */
int apple_tensor_layout(struct apple_tensor *t, uint64_t size, unsigned c, unsigned h, unsigned w);
static inline int apple_prog_layout(struct apple_prog *p, int i, unsigned c, unsigned h, unsigned w)
{
	return i < 0 || (unsigned)i >= p->count ? -1 : apple_tensor_layout(&p->t[i], p->size[i], c, h, w);
}
/* fp16 element (c, y, x) of surface i */
static inline _Float16 *apple_prog_at(struct apple_prog *p, int i, unsigned c, unsigned y, unsigned x)
{
	return (_Float16 *)((uint8_t *)p->map[i] + c * p->t[i].plane + y * p->t[i].row) + x;
}
int apple_prog_run(struct apple_prog *p);

#endif
