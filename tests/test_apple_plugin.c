/* SPDX-License-Identifier: MIT */
/* The built Apple effects plugin: it loads with every symbol resolved
 * (RTLD_NOW), exports the ABI, and without its network create fails cleanly
 * so the filter can fall back. */
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

#include "../src/camera_effects.h"

int main(int argc, char **argv)
{
	assert(argc == 3);
	void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);

	if (!lib) {
		fprintf(stderr, "%s\n", dlerror());
		return 1;
	}
	const struct ce_ops *(*entry)(void);

	*(void **)(&entry) = dlsym(lib, CE_ENTRY);
	assert(entry);
	const struct ce_ops *ops = entry();

	assert(ops && ops->abi == CE_ABI && ops->create && ops->process && ops->reset && ops->destroy);
	/* without the user's network the plugin refuses to start (and the loader falls back) */
	if (!access(CDN_APPLE_DIR "/segmentation/model.hwx", R_OK)) {
		printf("SKIP: a segmentation network is installed in " CDN_APPLE_DIR "\n");
		return 77;
	}
	struct ce_config cfg = { .abi = CE_ABI, .width = 640, .height = 480, .background = 1,
				 .matrix = 709, .studio_light = 0.5f };

	assert(!ops->create(&cfg));
	printf("plugin ok\n");
	return 0;
}
