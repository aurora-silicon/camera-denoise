// SPDX-License-Identifier: MIT
/*
 * Low-light backend on the Neural Engine.
 *
 * A small open CNN (luma only) runs on the 2x2 space-to-depth grid of the
 * frame: 4 input planes of half width and height in, a residual for each of
 * them out. The host does the space-to-depth, the residual add and the
 * depth-to-space inside the u8 <-> fp16 conversion it needs anyway, so the
 * ANE program is only the convolutions. Chroma passes through.
 *
 * The program is compiled by our ANE compiler (mil-hwx-compiler) from the
 * MIL and weights in tools/ane/, with the four host-built H16 load sections
 * next to it, one directory per ANE generation and grid size:
 *   <datadir>/aurora-camera-denoise/ane/<h16g|h17>-<W>x<H>/
 * The Linux ANE driver (accel/ane, ABI 3) loads it. Where the driver maps
 * the program's surfaces, the frame is converted straight into the input
 * surface and the residual read straight from the first four planes of the
 * output, and requests run in place; otherwise EXECUTE copies both ways. Where there is no ANE node, no program for the
 * frame size or the load fails, create() returns NULL and the filter keeps
 * the temporal stage alone.
 */
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../backend.h"

#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#define ANE_NEON 1
#endif

#ifndef CDN_DATADIR
#define CDN_DATADIR "/usr/share/aurora-camera-denoise"
#endif

/* accel/ane uapi, ABI version 3 (include/uapi/drm/ane_accel.h) */
#define ANE_ABI_V3 3
#define ANE_CHIP_H16 16
#define ANE_CHIP_H17 17
#define ANE_SECTIONS 4
#define ANE_SURFACES 4
struct ane_get_caps {
	uint32_t flags, size, abi_version, chip_family, section_size, bind_size, exec_io_size, pad;
};
struct ane_h16_section {
	uint32_t kind, size;
	uint64_t data_ptr;
};
struct ane_h16_load {
	uint64_t hwx_ptr, hwx_size, sections_ptr;
	uint32_t section_count, flags, surface_count, pad;
	uint64_t surface_size[ANE_SURFACES];
	uint32_t surface_direction[ANE_SURFACES];
};
struct ane_h16_buffer {
	uint64_t ptr, size;
};
struct ane_h16_exec {
	uint64_t buffers_ptr;
	uint32_t count, flags;
};
#define ANE_IOCTL_GET_CAPS _IOWR('d', 0x40 + 0x00, struct ane_get_caps)
#define ANE_IOCTL_H16_LOAD _IOWR('d', 0x40 + 0x07, struct ane_h16_load)
#define ANE_IOCTL_H16_EXEC _IOW('d', 0x40 + 0x08, struct ane_h16_exec)
/* accel minor 1: map the program's surfaces and run on them in place */
#define ANE_H16_MMAP_OFFSET(i) ((off_t)((uint64_t)((i) + 1) << 32))
#define ANE_H16_EXEC_IN_PLACE 1u

/* the network: 4 grid planes in, 16 planes out (the first 4 are the residual) */
#define IN_PLANES 4
#define OUT_PLANES 16

static const char *const section_files[ANE_SECTIONS] = {
	"generic.bin", "operation.bin", "procedure.bin", "text-properties.bin",
};

struct ane {
	int fd;
	int gw, gh;		/* grid size: half the frame */
	size_t row;		/* fp16 surface row bytes, 64-aligned */
	_Float16 *in, *out;
	size_t in_size, out_size;
	/* in and out are the driver's surfaces, mapped (lengths in_map, out_map) */
	bool mapped;
	size_t in_map, out_map;
	_Float16 lut[256];	/* u8 code -> fp16 in [0, 1] */
};

static void *read_file(const char *dir, const char *name, size_t *size)
{
	char path[512];
	struct stat st;
	void *buf = NULL;
	int fd;

	if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path))
		return NULL;
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return NULL;
	if (!fstat(fd, &st) && st.st_size > 0 && st.st_size < (64 << 20) &&
	    (buf = malloc((size_t)st.st_size)) &&
	    read(fd, buf, (size_t)st.st_size) != st.st_size) {
		free(buf);
		buf = NULL;
	}
	if (buf)
		*size = (size_t)st.st_size;
	close(fd);
	return buf;
}

/* The first ABI 3 ANE node; *chip gets its generation. */
static int open_ane(uint32_t *chip)
{
	glob_t g;
	int fd = -1;

	if (glob("/dev/accel/accel*", 0, NULL, &g))
		return -1;
	for (size_t i = 0; i < g.gl_pathc && fd < 0; i++) {
		struct ane_get_caps caps = { 0 };
		int f = open(g.gl_pathv[i], O_RDWR | O_CLOEXEC);

		if (f < 0)
			continue;
		if (!ioctl(f, ANE_IOCTL_GET_CAPS, &caps) && caps.abi_version == ANE_ABI_V3 &&
		    (caps.chip_family == ANE_CHIP_H16 || caps.chip_family == ANE_CHIP_H17)) {
			fd = f;
			*chip = caps.chip_family;
		}
		else
			close(f);
	}
	globfree(&g);
	return fd;
}

static void ane_destroy(void *state)
{
	struct ane *a = state;

	if (!a)
		return;
	if (a->mapped) {
		munmap(a->in, a->in_map);
		munmap(a->out, a->out_map);
	} else {
		free(a->in);
		free(a->out);
	}
	if (a->fd >= 0)
		close(a->fd);
	free(a);
}

static void *ane_create(int width, int height, int threads)
{
	char dir[512];
	const char *root = getenv("CDN_ANE_MODEL_DIR");
	void *hwx = NULL, *sec[ANE_SECTIONS] = { 0 };
	size_t hwx_size = 0, sec_size[ANE_SECTIONS] = { 0 };
	struct ane_h16_section table[ANE_SECTIONS];
	struct ane_h16_load load = { 0 };
	struct ane *a;
	uint32_t chip = 0;
	bool ok = false;

	(void)threads;
	if (width <= 0 || height <= 0 || (width & 1) || (height & 1))
		return NULL;
	a = calloc(1, sizeof(*a));
	if (!a)
		return NULL;
	a->fd = -1;
	a->gw = width / 2;
	a->gh = height / 2;
	a->row = ((size_t)a->gw * 2 + 63) & ~(size_t)63;
	a->fd = open_ane(&chip);
	if (a->fd < 0)
		goto out;
	if (snprintf(dir, sizeof(dir), "%s/%s-%dx%d", root && *root ? root : CDN_DATADIR "/ane",
		     chip == ANE_CHIP_H17 ? "h17" : "h16g", a->gw, a->gh) >= (int)sizeof(dir))
		goto out;
	hwx = read_file(dir, "model.hwx", &hwx_size);
	for (int i = 0; i < ANE_SECTIONS; i++)
		sec[i] = read_file(dir, section_files[i], &sec_size[i]);
	if (!hwx || !sec[0] || !sec[1] || !sec[2] || !sec[3])
		goto out;
	for (int i = 0; i < ANE_SECTIONS; i++)
		table[i] = (struct ane_h16_section){ (uint32_t)i + 1, (uint32_t)sec_size[i],
						     (uint64_t)(uintptr_t)sec[i] };
	load.hwx_ptr = (uint64_t)(uintptr_t)hwx;
	load.hwx_size = hwx_size;
	load.sections_ptr = (uint64_t)(uintptr_t)table;
	load.section_count = ANE_SECTIONS;
	if (ioctl(a->fd, ANE_IOCTL_H16_LOAD, &load)) {
		fprintf(stderr, "camera-denoise: ane: load %s: %s\n", dir, strerror(errno));
		goto out;
	}
	a->in_size = (size_t)load.surface_size[0];
	a->out_size = (size_t)load.surface_size[1];
	if (load.surface_count != 2 || load.surface_direction[0] != 1 ||
	    load.surface_direction[1] != 2 || a->in_size < IN_PLANES * a->row * a->gh ||
	    a->out_size < OUT_PLANES * a->row * a->gh) {
		fprintf(stderr, "camera-denoise: ane: %s does not fit %dx%d\n", dir, width, height);
		goto out;
	}
	if (!getenv("CDN_ANE_COPY")) {
		long page = sysconf(_SC_PAGESIZE);
		/* only the residual planes of the output are read */
		size_t used = (size_t)IN_PLANES * a->row * a->gh;
		void *in, *out;

		a->in_map = (a->in_size + page - 1) & ~(size_t)(page - 1);
		a->out_map = (used + page - 1) & ~(size_t)(page - 1);
		in = mmap(NULL, a->in_map, PROT_READ | PROT_WRITE, MAP_SHARED, a->fd, ANE_H16_MMAP_OFFSET(0));
		out = in == MAP_FAILED ? MAP_FAILED :
			mmap(NULL, a->out_map, PROT_READ, MAP_SHARED, a->fd, ANE_H16_MMAP_OFFSET(1));
		if (out != MAP_FAILED) {
			a->in = in;
			a->out = out;
			a->mapped = true;
			fprintf(stderr, "camera-denoise: ane: %s: running in place on the mapped surfaces\n", dir);
		} else if (in != MAP_FAILED) {
			munmap(in, a->in_map);
		}
	}
	if (!a->mapped) {
		a->in = aligned_alloc(64, (a->in_size + 63) & ~(size_t)63);
		a->out = aligned_alloc(64, (a->out_size + 63) & ~(size_t)63);
		if (!a->in || !a->out)
			goto out;
	}
	memset(a->in, 0, a->in_size);
	for (int i = 0; i < 256; i++)
		a->lut[i] = (_Float16)(i / 255.0f);
	ok = true;
out:
	free(hwx);
	for (int i = 0; i < ANE_SECTIONS; i++)
		free(sec[i]);
	if (!ok) {
		ane_destroy(a);
		return NULL;
	}
	return a;
}

/* plane c of the grid holds the frame pixels (2y + c/2, 2x + c%2) */
static inline _Float16 *plane(_Float16 *base, size_t row, int gh, int c)
{
	return (_Float16 *)((uint8_t *)base + (size_t)c * row * gh);
}

static int ane_process(void *state, const struct cdn_frame *in, struct cdn_frame *out,
		       const struct cdn_frame_info *info, float strength)
{
	struct ane *a = state;
	struct ane_h16_buffer bufs[2] = {
		{ (uint64_t)(uintptr_t)a->in, a->in_size },
		{ (uint64_t)(uintptr_t)a->out, a->out_size },
	};
	struct ane_h16_exec exec = { (uint64_t)(uintptr_t)bufs, 2, 0 };

	if (a->mapped)
		exec = (struct ane_h16_exec){ 0, 2, ANE_H16_EXEC_IN_PLACE };

	(void)info;
	if (in->width != 2 * a->gw || in->height != 2 * a->gh)
		return -EINVAL;
	for (int y = 0; y < a->gh; y++)
		for (int by = 0; by < 2; by++) {
			const uint8_t *src = in->y + (size_t)(2 * y + by) * in->stride_y;
			_Float16 *p0 = (_Float16 *)((uint8_t *)plane(a->in, a->row, a->gh, 2 * by) + (size_t)y * a->row);
			_Float16 *p1 = (_Float16 *)((uint8_t *)plane(a->in, a->row, a->gh, 2 * by + 1) + (size_t)y * a->row);
			int x = 0;
#ifdef ANE_NEON
			/* base ARMv8 only: u8 -> f32 * 1/255 -> f16 (no FP16 arithmetic) */
			const float32x4_t scale = vdupq_n_f32(1.0f / 255.0f);
			for (; x + 16 <= a->gw; x += 16) {
				uint8x16x2_t v = vld2q_u8(src + 2 * x);	/* even and odd columns */
				for (int c = 0; c < 2; c++) {
					float16_t *p = (float16_t *)(c ? p1 : p0) + x;
					uint16x8_t lo = vmovl_u8(vget_low_u8(v.val[c])), hi = vmovl_high_u8(v.val[c]);
					vst1q_f16(p, vcombine_f16(vcvt_f16_f32(vmulq_f32(vcvtq_f32_u32(vmovl_u16(vget_low_u16(lo))), scale)),
								  vcvt_f16_f32(vmulq_f32(vcvtq_f32_u32(vmovl_high_u16(lo)), scale))));
					vst1q_f16(p + 8, vcombine_f16(vcvt_f16_f32(vmulq_f32(vcvtq_f32_u32(vmovl_u16(vget_low_u16(hi))), scale)),
								      vcvt_f16_f32(vmulq_f32(vcvtq_f32_u32(vmovl_high_u16(hi)), scale))));
				}
			}
#endif
			for (; x < a->gw; x++) {
				p0[x] = a->lut[src[2 * x]];
				p1[x] = a->lut[src[2 * x + 1]];
			}
		}
	if (ioctl(a->fd, ANE_IOCTL_H16_EXEC, &exec))
		return -errno;
	if (strength < 0.0f) strength = 0.0f;
	if (strength > 1.0f) strength = 1.0f;
	const float k = 255.0f * strength;
	for (int y = 0; y < a->gh; y++)
		for (int by = 0; by < 2; by++) {
			const uint8_t *src = in->y + (size_t)(2 * y + by) * in->stride_y;
			uint8_t *dst = out->y + (size_t)(2 * y + by) * out->stride_y;
			const _Float16 *r0 = (const _Float16 *)((const uint8_t *)plane(a->out, a->row, a->gh, 2 * by) + (size_t)y * a->row);
			const _Float16 *r1 = (const _Float16 *)((const uint8_t *)plane(a->out, a->row, a->gh, 2 * by + 1) + (size_t)y * a->row);
			int x = 0;
#ifdef ANE_NEON
			const float32x4_t kk = vdupq_n_f32(k), half = vdupq_n_f32(0.5f);
			for (; x + 8 <= a->gw; x += 8) {
				uint8x8x2_t v = vld2_u8(src + 2 * x), o;
				for (int c = 0; c < 2; c++) {
					float16x8_t r = vld1q_f16((const float16_t *)(c ? r1 : r0) + x);
					uint16x8_t s16 = vmovl_u8(v.val[c]);
					float32x4_t lo = vfmaq_f32(vaddq_f32(vcvtq_f32_u32(vmovl_u16(vget_low_u16(s16))), half),
								   vcvt_f32_f16(vget_low_f16(r)), kk);
					float32x4_t hi = vfmaq_f32(vaddq_f32(vcvtq_f32_u32(vmovl_high_u16(s16)), half),
								   vcvt_high_f32_f16(r), kk);
					/* float -> u32 truncates and saturates negatives to 0, then narrow with saturation */
					uint16x8_t w = vcombine_u16(vqmovn_u32(vcvtq_u32_f32(lo)), vqmovn_u32(vcvtq_u32_f32(hi)));
					o.val[c] = vqmovn_u16(w);
				}
				vst2_u8(dst + 2 * x, o);
			}
#endif
			for (; x < a->gw; x++) {
				float v0 = src[2 * x] + k * (float)r0[x] + 0.5f;
				float v1 = src[2 * x + 1] + k * (float)r1[x] + 0.5f;

				dst[2 * x] = v0 < 0.0f ? 0 : v0 > 255.0f ? 255 : (uint8_t)v0;
				dst[2 * x + 1] = v1 < 0.0f ? 0 : v1 > 255.0f ? 255 : (uint8_t)v1;
			}
		}
	for (int y = 0; y < in->height / 2; y++)
		memcpy(out->uv + (size_t)y * out->stride_uv, in->uv + (size_t)y * in->stride_uv,
		       (size_t)in->width);
	return 0;
}

static float ane_noise_sigma(void *state)
{
	(void)state;
	return -1.0f;
}

static const struct cdn_backend_ops ops = {
	.abi = CDN_BACKEND_ABI,
	.name = "ane",
	.create = ane_create,
	.process = ane_process,
	.noise_sigma = ane_noise_sigma,
	.destroy = ane_destroy,
};

__attribute__((visibility("default"))) const struct cdn_backend_ops *cdn_backend_entry(void)
{
	return &ops;
}
