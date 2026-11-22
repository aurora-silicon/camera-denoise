// SPDX-License-Identifier: MIT
#include "ane_prog.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* accel/ane uapi, ABI version 3 (include/uapi/drm/ane_accel.h) */
#define ANE_ABI_V3 3
#define ANE_SECTIONS 4
#define ANE_SURFACES 4
#define ANE_SURFACES_WIDE 16
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
/* DRM_IOCTL_VERSION minor 2: programs with several outputs (up to 16 surfaces) */
struct ane_h16_load_wide {
	uint64_t hwx_ptr, hwx_size, sections_ptr;
	uint32_t section_count, flags, surface_count, pad;
	uint64_t surface_size[ANE_SURFACES_WIDE];
	uint32_t surface_direction[ANE_SURFACES_WIDE];
};
struct ane_h16_exec {
	uint64_t buffers_ptr;
	uint32_t count, flags;
};
#define ANE_IOCTL_GET_CAPS _IOWR('d', 0x40 + 0x00, struct ane_get_caps)
#define ANE_IOCTL_H16_LOAD _IOWR('d', 0x40 + 0x07, struct ane_h16_load)
#define ANE_IOCTL_H16_EXEC _IOW('d', 0x40 + 0x08, struct ane_h16_exec)
#define ANE_IOCTL_H16_LOAD_WIDE _IOWR('d', 0x40 + 0x09, struct ane_h16_load_wide)
#define ANE_H16_MMAP_OFFSET(i) ((off_t)((uint64_t)((i) + 1) << 32))
#define ANE_H16_EXEC_IN_PLACE 1u

static const char *const section_files[ANE_SECTIONS] = {
	"generic.bin", "operation.bin", "procedure.bin", "text-properties.bin",
};

int apple_tensor_layout(struct apple_tensor *t, uint64_t size, unsigned c, unsigned h, unsigned w)
{
	if (!c || !h || !w || size % c || (size / c) % h)
		return -1;
	uint64_t plane = size / c, row = plane / h;

	if (row < 2ull * w || row % 2)
		return -1;
	t->c = c;
	t->h = h;
	t->w = w;
	t->plane = plane;
	t->row = row;
	return 0;
}

static void *read_file(const char *dir, const char *name, size_t *size)
{
	char path[1024];
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

/* An ABI 3 node (the H16 program interface), or -1. */
static int open_node(const char *path)
{
	struct ane_get_caps caps = { 0 };
	int f = open(path, O_RDWR | O_CLOEXEC);

	if (f < 0)
		return -1;
	if (ioctl(f, ANE_IOCTL_GET_CAPS, &caps) || caps.abi_version != ANE_ABI_V3) {
		close(f);
		return -1;
	}
	return f;
}

/* Load on fd: the wide load, or the narrow one on a kernel before it (4 surfaces or fewer). */
static int load(int fd, struct ane_h16_load_wide *wide)
{
	struct ane_h16_load narrow = { 0 };

	if (!ioctl(fd, ANE_IOCTL_H16_LOAD_WIDE, wide))
		return 0;
	if (errno != ENOTTY && errno != EINVAL)
		return -errno;
	narrow.hwx_ptr = wide->hwx_ptr;
	narrow.hwx_size = wide->hwx_size;
	narrow.sections_ptr = wide->sections_ptr;
	narrow.section_count = wide->section_count;
	if (ioctl(fd, ANE_IOCTL_H16_LOAD, &narrow))
		return -errno;
	wide->surface_count = narrow.surface_count;
	for (int i = 0; i < ANE_SURFACES; i++) {
		wide->surface_size[i] = narrow.surface_size[i];
		wide->surface_direction[i] = narrow.surface_direction[i];
	}
	return 0;
}

void apple_prog_close(struct apple_prog *p)
{
	if (!p)
		return;
	for (unsigned i = 0; i < APPLE_PROG_SURFACES; i++)
		if (p->map[i] && p->map[i] != MAP_FAILED)
			munmap(p->map[i], p->map_len[i]);
	if (p->fd >= 0)
		close(p->fd);
	memset(p, 0, sizeof(*p));
	p->fd = -1;
}

int apple_prog_open(struct apple_prog *p, const char *dir)
{
	void *hwx = NULL, *sec[ANE_SECTIONS] = { 0 };
	size_t hwx_size = 0, sec_size[ANE_SECTIONS] = { 0 };
	struct ane_h16_section table[ANE_SECTIONS];
	struct ane_h16_load_wide wide = { 0 };
	long page = sysconf(_SC_PAGESIZE);
	int ret = -ENODEV;
	glob_t g;

	memset(p, 0, sizeof(*p));
	p->fd = -1;
	hwx = read_file(dir, "model.hwx", &hwx_size);
	for (int i = 0; i < ANE_SECTIONS; i++)
		sec[i] = read_file(dir, section_files[i], &sec_size[i]);
	if (!hwx || !sec[0] || !sec[1] || !sec[2] || !sec[3]) {
		ret = -ENOENT;
		goto out;
	}
	for (int i = 0; i < ANE_SECTIONS; i++)
		table[i] = (struct ane_h16_section){ (uint32_t)i + 1, (uint32_t)sec_size[i],
						     (uint64_t)(uintptr_t)sec[i] };
	/* the driver decides: the first node that loads the program runs it */
	if (!glob("/dev/accel/accel*", 0, NULL, &g)) {
		for (size_t i = 0; i < g.gl_pathc && p->fd < 0; i++) {
			int fd = open_node(g.gl_pathv[i]), err;

			if (fd < 0)
				continue;
			memset(&wide, 0, sizeof(wide));
			wide.hwx_ptr = (uint64_t)(uintptr_t)hwx;
			wide.hwx_size = hwx_size;
			wide.sections_ptr = (uint64_t)(uintptr_t)table;
			wide.section_count = ANE_SECTIONS;
			err = load(fd, &wide);
			if (!err && wide.surface_count >= 2 && wide.surface_count <= APPLE_PROG_SURFACES) {
				p->fd = fd;
			} else {
				fprintf(stderr, "camera-effects: apple: %s on %s: %s\n", dir, g.gl_pathv[i],
					err ? strerror(-err) : "unexpected surface count");
				close(fd);
			}
		}
		globfree(&g);
	}
	if (p->fd < 0)
		goto out;
	p->count = wide.surface_count;
	for (unsigned i = 0; i < p->count; i++) {
		struct apple_tensor *t = &p->t[i];

		t->direction = wide.surface_direction[i];
		p->size[i] = wide.surface_size[i];
		if ((t->direction != 1 && t->direction != 2) || !p->size[i]) {
			fprintf(stderr, "camera-effects: apple: %s: surface %u: direction %u, %llu bytes\n", dir, i,
				t->direction, (unsigned long long)p->size[i]);
			ret = -EINVAL;
			goto out;
		}
		p->map_len[i] = (p->size[i] + (size_t)page - 1) & ~((size_t)page - 1);
		p->map[i] = mmap(NULL, p->map_len[i], t->direction == 1 ? PROT_READ | PROT_WRITE : PROT_READ,
				 MAP_SHARED, p->fd, ANE_H16_MMAP_OFFSET(i));
		if (p->map[i] == MAP_FAILED) {
			p->map[i] = NULL;
			ret = -errno;
			fprintf(stderr, "camera-effects: apple: %s: map surface %u: %s\n", dir, i, strerror(errno));
			goto out;
		}
		if (t->direction == 1)
			memset(p->map[i], 0, p->size[i]);
	}
	ret = 0;
out:
	free(hwx);
	for (int i = 0; i < ANE_SECTIONS; i++)
		free(sec[i]);
	if (ret)
		apple_prog_close(p);
	return ret;
}

int apple_prog_surface(const struct apple_prog *p, uint32_t direction, unsigned k)
{
	for (unsigned i = 0; i < p->count; i++)
		if (p->t[i].direction == direction && !k--)
			return (int)i;
	return -1;
}

int apple_prog_run(struct apple_prog *p)
{
	struct ane_h16_exec exec = { .count = p->count, .flags = ANE_H16_EXEC_IN_PLACE };

	return ioctl(p->fd, ANE_IOCTL_H16_EXEC, &exec) ? -errno : 0;
}
