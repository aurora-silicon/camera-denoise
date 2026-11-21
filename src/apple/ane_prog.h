/* SPDX-License-Identifier: MIT */
#ifndef CDN_APPLE_ANE_PROG_H
#define CDN_APPLE_ANE_PROG_H
/*
 * One Neural Engine program that macOS compiled (an aned cache model.hwx),
 * run through the Linux accel/ane driver (ABI 3) with the four host-built
 * sections next to it. Every surface is mapped and requests run in place.
 * The tensor layout of each surface (name, N C H W, fp16 row and plane
 * strides) is read from the HWX itself, so callers find surfaces by name.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APPLE_PROG_SURFACES 16

struct apple_tensor {
	char name[64];
	uint32_t direction;           /* 1 input, 2 output */
	uint32_t n, c, h, w;
	uint64_t row, plane, batch;   /* bytes; fp16 elements */
	uint64_t vm;
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
 * The tensors of an HWX in surface (segment) order: its __FVMLIB segments,
 * named by their buffer references, described by their tensor commands.
 * Returns the count, or -1 if the file is not a well-formed HWX.
 */
int apple_hwx_tensors(const uint8_t *hwx, size_t size, struct apple_tensor *t, unsigned max);

/* Load dir/model.hwx with dir/{generic,operation,procedure,text-properties}.bin. */
int apple_prog_open(struct apple_prog *p, const char *dir);
void apple_prog_close(struct apple_prog *p);
int apple_prog_find(const struct apple_prog *p, const char *name);
/* fp16 element (c, y, x) of surface i */
static inline _Float16 *apple_prog_at(struct apple_prog *p, int i, unsigned c, unsigned y, unsigned x)
{
	return (_Float16 *)((uint8_t *)p->map[i] + c * p->t[i].plane + y * p->t[i].row) + x;
}
int apple_prog_run(struct apple_prog *p);

#endif
