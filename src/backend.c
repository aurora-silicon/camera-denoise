// SPDX-License-Identifier: MIT
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backend.h"

#ifndef CDN_BACKEND_DIR
#define CDN_BACKEND_DIR "/usr/lib/aurora-camera-denoise"
#endif

void cdn_frame_copy(const struct cdn_frame *in, struct cdn_frame *out)
{
	for (int y = 0; y < in->height; y++)
		memcpy(out->y + (size_t)y * out->stride_y,
		       in->y + (size_t)y * in->stride_y, (size_t)in->width);
	for (int y = 0; y < in->height / 2; y++)
		memcpy(out->uv + (size_t)y * out->stride_uv,
		       in->uv + (size_t)y * in->stride_uv, (size_t)in->width);
}

/* Plugins are loaded on the main thread before streaming. Cache one dlopen
 * per name, including failures, so format renegotiation does not leak handles. */
static struct plugin {
	char name[65];
	const struct cdn_backend_ops *ops;
	const struct cdn_backend_dmabuf_ops *dmabuf;
} plugins[32];
static unsigned nplugins;

const struct cdn_backend_dmabuf_ops *cdn_backend_dmabuf(const struct cdn_backend_ops *ops)
{
	for (unsigned i = 0; i < nplugins; i++)
		if (plugins[i].ops == ops)
			return plugins[i].dmabuf;
	return NULL;
}

static const struct cdn_backend_ops *load_plugin(const char *name)
{
	char path[512];
	void *dl;
	cdn_backend_entry_fn entry;
	cdn_backend_dmabuf_entry_fn dmabuf_entry;
	struct plugin *p;
	const struct cdn_backend_ops *ops;

	if (strchr(name, '/') || strlen(name) > 64)
		return NULL;
	for (unsigned i = 0; i < nplugins; i++)
		if (!strcmp(plugins[i].name, name))
			return plugins[i].ops;
	if (nplugins == sizeof(plugins) / sizeof(plugins[0]))
		return NULL;
	p = &plugins[nplugins++];
	snprintf(p->name, sizeof(p->name), "%s", name);
	const char *dir = getenv("CDN_BACKEND_DIR");
	if (!dir || !*dir) dir = CDN_BACKEND_DIR;
	if (snprintf(path, sizeof(path), "%s/cdn-backend-%s.so", dir, name) >= (int)sizeof(path))
		return NULL;
	dl = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!dl)
		return NULL;
	entry = (cdn_backend_entry_fn)dlsym(dl, CDN_BACKEND_ENTRY);
	ops = entry ? entry() : NULL;
	if (!ops || ops->abi != CDN_BACKEND_ABI || !ops->create || !ops->process ||
	    !ops->destroy) {
		fprintf(stderr, "camera-denoise: %s: not a usable backend (ABI %u)\n",
			path, ops ? ops->abi : 0);
		dlclose(dl);
		return NULL;
	}
	p->ops = ops;
	dmabuf_entry = (cdn_backend_dmabuf_entry_fn)dlsym(dl, CDN_DMABUF_ENTRY);
	p->dmabuf = dmabuf_entry ? dmabuf_entry() : NULL;
	if (p->dmabuf && p->dmabuf->abi != CDN_DMABUF_ABI)
		p->dmabuf = NULL;
	/* stays loaded for the life of the process */
	return ops;
}

const struct cdn_backend_ops *cdn_backend_find(const char *name, const char *prefer)
{
	if (!name || !strcmp(name, "cpu"))
		return cdn_cpu_backend();
	if (strcmp(name, "auto"))
		return load_plugin(name);

	if (prefer) {
		char buf[256], *save = NULL;
		snprintf(buf, sizeof(buf), "%s", prefer);
		for (char *t = strtok_r(buf, ", ", &save); t; t = strtok_r(NULL, ", ", &save)) {
			const struct cdn_backend_ops *ops = !strcmp(t, "cpu") ?
				cdn_cpu_backend() : load_plugin(t);
			if (ops)
				return ops;
		}
	}
	return cdn_cpu_backend();
}
