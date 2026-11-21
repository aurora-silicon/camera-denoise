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
#define ANE_CHIP_H17 17
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

static uint32_t rd32(const uint8_t *p)
{
	uint32_t v;

	memcpy(&v, p, 4);
	return v;
}

static uint64_t rd64(const uint8_t *p)
{
	uint64_t v;

	memcpy(&v, p, 8);
	return v;
}

/* NUL-terminated within [p, end), else NULL */
static const char *cstr(const uint8_t *p, const uint8_t *end)
{
	return p < end && memchr(p, 0, (size_t)(end - p)) ? (const char *)p : NULL;
}

int apple_hwx_tensors(const uint8_t *b, size_t size, struct apple_tensor *t, unsigned max)
{
	struct { uint64_t vm; const char *name; } refs[APPLE_PROG_SURFACES * 2];
	const uint8_t *tensors[APPLE_PROG_SURFACES * 2];
	unsigned nseg = 0, nref = 0, nten = 0;
	size_t pos = 32;

	if (size < 32 || rd32(b) != 0xbeefface)
		return -1;
	for (uint32_t i = 0, n = rd32(b + 16); i < n; i++) {
		if (pos + 8 > size)
			return -1;
		const uint8_t *c = b + pos;
		uint32_t cmd = rd32(c), len = rd32(c + 4);

		if (len < 8 || pos + len > size)
			return -1;
		if (cmd == 0x19 && len >= 72 && !strncmp((const char *)c + 8, "__FVMLIB", 16)) {
			uint32_t prot = rd32(c + 60);

			if (prot == 1 || prot == 2) {
				if (nseg == max)
					return -1;
				memset(&t[nseg], 0, sizeof(t[nseg]));
				t[nseg].vm = rd64(c + 24);
				t[nseg].direction = prot;
				nseg++;
			}
		} else if (cmd == 0x40 && len >= 32 && nref < sizeof(refs) / sizeof(refs[0])) {
			refs[nref].vm = rd64(c + 0x10);
			refs[nref].name = cstr(c + 0x18, c + len);
			if (refs[nref].name)
				nref++;
		} else if (cmd == 4 && len >= 0xd48 && rd32(c + 8) == 3 &&
			   nten < sizeof(tensors) / sizeof(tensors[0])) {
			tensors[nten++] = c;
		}
		pos += len;
	}
	for (unsigned s = 0; s < nseg; s++) {
		const char *name = NULL;

		for (unsigned r = 0; r < nref; r++)
			if (refs[r].vm == t[s].vm)
				name = refs[r].name;
		if (!name || strlen(name) >= sizeof(t[s].name))
			return -1;
		snprintf(t[s].name, sizeof(t[s].name), "%s", name);
		for (unsigned k = 0; k < nten; k++) {
			const uint8_t *c = tensors[k];
			uint32_t len = rd32(c + 4), shortoff = rd32(c + 0x7c);
			const char *sn = shortoff < len ? cstr(c + shortoff, c + len) : NULL;

			if (!sn || strcmp(sn, name))
				continue;
			t[s].n = rd32(c + 0x28);
			t[s].c = rd32(c + 0x2c);
			t[s].h = rd32(c + 0x30);
			t[s].w = rd32(c + 0x34);
			t[s].batch = rd64(c + 0x50);
			t[s].plane = rd64(c + 0x58);
			t[s].row = rd64(c + 0x60);
		}
		if (!t[s].c || !t[s].h || !t[s].w || t[s].row < 2ull * t[s].w ||
		    t[s].plane < t[s].row * t[s].h || t[s].batch < t[s].plane * t[s].c)
			return -1;
	}
	return (int)nseg;
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

static int open_ane(void)
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
		/* Apple's camera programs are H17 (M5 is H17G) */
		if (!ioctl(f, ANE_IOCTL_GET_CAPS, &caps) && caps.abi_version == ANE_ABI_V3 &&
		    caps.chip_family == ANE_CHIP_H17)
			fd = f;
		else
			close(f);
	}
	globfree(&g);
	return fd;
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
	int ret = -EINVAL, n;

	memset(p, 0, sizeof(*p));
	p->fd = -1;
	hwx = read_file(dir, "model.hwx", &hwx_size);
	for (int i = 0; i < ANE_SECTIONS; i++)
		sec[i] = read_file(dir, section_files[i], &sec_size[i]);
	if (!hwx || !sec[0] || !sec[1] || !sec[2] || !sec[3]) {
		ret = -ENOENT;
		goto out;
	}
	n = apple_hwx_tensors(hwx, hwx_size, p->t, APPLE_PROG_SURFACES);
	if (n < 2) {
		fprintf(stderr, "camera-effects: apple: %s: not an HWX this engine reads\n", dir);
		goto out;
	}
	p->fd = open_ane();
	if (p->fd < 0) {
		ret = -ENODEV;
		goto out;
	}
	for (int i = 0; i < ANE_SECTIONS; i++)
		table[i] = (struct ane_h16_section){ (uint32_t)i + 1, (uint32_t)sec_size[i],
						     (uint64_t)(uintptr_t)sec[i] };
	wide.hwx_ptr = (uint64_t)(uintptr_t)hwx;
	wide.hwx_size = hwx_size;
	wide.sections_ptr = (uint64_t)(uintptr_t)table;
	wide.section_count = ANE_SECTIONS;
	if (ioctl(p->fd, ANE_IOCTL_H16_LOAD_WIDE, &wide)) {
		/* A kernel before the wide load: the narrow one for 4 surfaces or fewer. */
		struct ane_h16_load narrow = { 0 };

		if ((errno != ENOTTY && errno != EINVAL) || n > ANE_SURFACES) {
			ret = -errno;
			fprintf(stderr, "camera-effects: apple: load %s: %s\n", dir, strerror(errno));
			goto out;
		}
		narrow.hwx_ptr = wide.hwx_ptr;
		narrow.hwx_size = wide.hwx_size;
		narrow.sections_ptr = wide.sections_ptr;
		narrow.section_count = ANE_SECTIONS;
		if (ioctl(p->fd, ANE_IOCTL_H16_LOAD, &narrow)) {
			ret = -errno;
			fprintf(stderr, "camera-effects: apple: load %s: %s\n", dir, strerror(errno));
			goto out;
		}
		wide.surface_count = narrow.surface_count;
		for (int i = 0; i < ANE_SURFACES; i++) {
			wide.surface_size[i] = narrow.surface_size[i];
			wide.surface_direction[i] = narrow.surface_direction[i];
		}
	}
	if (wide.surface_count != (uint32_t)n) {
		fprintf(stderr, "camera-effects: apple: %s: %u surfaces loaded, %d in the HWX\n",
			dir, wide.surface_count, n);
		goto out;
	}
	p->count = (unsigned)n;
	for (unsigned i = 0; i < p->count; i++) {
		const struct apple_tensor *t = &p->t[i];

		p->size[i] = wide.surface_size[i];
		if (wide.surface_direction[i] != t->direction || p->size[i] < t->n * t->batch) {
			fprintf(stderr, "camera-effects: apple: %s: surface %u (%s) does not match\n",
				dir, i, t->name);
			goto out;
		}
		p->map_len[i] = (p->size[i] + (size_t)page - 1) & ~((size_t)page - 1);
		p->map[i] = mmap(NULL, p->map_len[i], t->direction == 1 ? PROT_READ | PROT_WRITE : PROT_READ,
				 MAP_SHARED, p->fd, ANE_H16_MMAP_OFFSET(i));
		if (p->map[i] == MAP_FAILED) {
			p->map[i] = NULL;
			ret = -errno;
			fprintf(stderr, "camera-effects: apple: %s: map surface %u: %s\n",
				dir, i, strerror(errno));
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

int apple_prog_find(const struct apple_prog *p, const char *name)
{
	for (unsigned i = 0; i < p->count; i++)
		if (!strcmp(p->t[i].name, name))
			return (int)i;
	return -1;
}

int apple_prog_run(struct apple_prog *p)
{
	struct ane_h16_exec exec = { .count = p->count, .flags = ANE_H16_EXEC_IN_PLACE };

	return ioctl(p->fd, ANE_IOCTL_H16_EXEC, &exec) ? -errno : 0;
}
