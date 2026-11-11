// SPDX-License-Identifier: MIT
/* The ANE backend gives up cleanly: no program for the size, odd sizes, or no
 * ANE node (the test machines have none) all return NULL from create(). */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "../src/backend.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)
int main(int argc, char **argv)
{
	char dir[1024];
	CHECK(argc == 3);
	void *dl = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	CHECK(dl);
	cdn_backend_entry_fn entry = (cdn_backend_entry_fn)dlsym(dl, CDN_BACKEND_ENTRY);
	CHECK(entry);
	const struct cdn_backend_ops *ops = entry();
	CHECK(ops && ops->abi == CDN_BACKEND_ABI && ops->create && ops->process && ops->destroy);
	snprintf(dir, sizeof(dir), "%s/ane-test-models", argv[2]);
	mkdir(dir, 0755);
	CHECK(!setenv("CDN_ANE_MODEL_DIR", dir, 1));
	CHECK(!ops->create(1920, 1080, 0));	/* no ANE node or no program here */
	CHECK(!ops->create(1921, 1080, 0));	/* odd sizes have no grid */
	CHECK(!ops->create(0, 0, 0));
	CHECK(ops->noise_sigma(NULL) < 0);
	return 0;
}
