// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../src/backend.h"
static int copy(const char *src, const char *dst)
{
	FILE *in=fopen(src,"rb"), *out=fopen(dst,"wb");
	if (!in || !out) return 1;
	char buf[4096]; size_t n;
	while ((n=fread(buf,1,sizeof(buf),in))) if (fwrite(buf,1,n,out)!=n) return 1;
	int err=ferror(in);
	fclose(in);
	return fclose(out) || err;
}
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while (0)
int main(int argc, char **argv)
{
	CHECK(argc==5);
	char dir[1024], path[1200];
	snprintf(dir,sizeof(dir),"%s/plugin-test-XXXXXX",argv[1]);
	CHECK(mkdtemp(dir));
	CHECK(!setenv("CDN_BACKEND_DIR",dir,1));
	CHECK(!cdn_backend_find("missing",NULL));
	snprintf(path,sizeof(path),"%s/cdn-backend-missing.so",dir);
	CHECK(!copy(argv[2],path));
	const struct cdn_backend_ops *ops=cdn_backend_find("missing",NULL);
	CHECK(ops && ops==cdn_backend_find("missing",NULL));
	const struct cdn_backend_dmabuf_ops *ext=cdn_backend_dmabuf(ops);
	CHECK(ext && ext->abi==CDN_DMABUF_ABI && ext->process);
	CHECK(ext->flags & CDN_BACKEND_SINGLE_UPLOAD);
	void *st=ops->create(64,64,0);
	CHECK(st && cdn_backend_ask(ops,st,CDN_QUERY_AUTO)==1);
	CHECK(cdn_backend_ask(ops,st,99)==0);
	ops->destroy(st);
	unlink(path);
	// Bad ABI is rejected and closed; replacing it can be retried.
	snprintf(path,sizeof(path),"%s/cdn-backend-retry.so",dir);
	CHECK(!copy(argv[3],path));
	CHECK(!cdn_backend_find("retry",NULL));
	CHECK(!copy(argv[2],path));
	CHECK(cdn_backend_find("retry",NULL));
	unlink(path);
	// ABI v1 without the optional companion symbol still works.
	snprintf(path,sizeof(path),"%s/cdn-backend-legacy.so",dir);
	CHECK(!copy(argv[4],path));
	ops=cdn_backend_find("legacy",NULL);
	CHECK(ops && !cdn_backend_dmabuf(ops));
	// Without the query symbol, auto does not keep a plugin.
	st=ops->create(64,64,0);
	CHECK(st && cdn_backend_ask(ops,st,CDN_QUERY_AUTO)==0);
	ops->destroy(st);
	CHECK(cdn_backend_ask(cdn_cpu_backend(),NULL,CDN_QUERY_AUTO)==0);
	CHECK(cdn_backend_find("auto","absent,cpu")==cdn_cpu_backend());
	CHECK(!cdn_backend_find("../bad",NULL));
	unlink(path); rmdir(dir);
	return 0;
}
