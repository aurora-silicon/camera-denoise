// SPDX-License-Identifier: MIT
/* Test transport layout and bounds without a PipeWire server or camera. */
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../src/pipewire-frame.h"
static int fails;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); fails++; } } while (0)
int main(void)
{
	int fd = memfd_create("cdn-layout-test",MFD_CLOEXEC);
	if (fd < 0) return 1;
	uint8_t y[700], uv[350];
	struct spa_chunk chunks[2] = {{.offset=12,.size=648,.stride=24}, {.offset=7,.size=288,.stride=32}};
	struct spa_data data[2] = {
		{.type=SPA_DATA_DmaBuf,.fd=fd,.mapoffset=4096,.maxsize=sizeof(y),.data=y,.chunk=&chunks[0]},
		{.type=SPA_DATA_DmaBuf,.fd=fd,.mapoffset=8192,.maxsize=sizeof(uv),.data=uv,.chunk=&chunks[1]}};
	struct spa_buffer b = {.n_datas=1,.datas=data};
	struct spa_video_info_raw fmt = {.size={18,18}};
	struct cdn_frame f;
	struct cdn_dmabuf dma;
	CHECK(cdn_frame_from_buffer(&b,&fmt,&f));
	CHECK(f.y==y+12 && f.uv==y+12+24*18 && f.stride_y==24 && f.stride_uv==24);
	CHECK(cdn_frame_dmabuf(&b,&f,&dma));
	CHECK(dma.y.offset==4108 && dma.uv.offset==4540 && dma.y.fd==fd && dma.uv.fd==fd);
	b.n_datas=2; chunks[0].size=432;
	CHECK(cdn_frame_from_buffer(&b,&fmt,&f));
	CHECK(f.y==y+12 && f.uv==uv+7 && f.stride_uv==32);
	CHECK(cdn_frame_dmabuf(&b,&f,&dma));
	CHECK(dma.uv.offset==8199 && dma.uv.stride==32);
	data[1].type=SPA_DATA_MemPtr; data[1].fd=-1;
	CHECK(cdn_frame_from_buffer(&b,&fmt,&f));
	CHECK(!cdn_frame_dmabuf(&b,&f,&dma));
	data[1].maxsize=20;
	CHECK(!cdn_frame_from_buffer(&b,&fmt,&f)); data[1].maxsize=sizeof(uv);
	chunks[1].size=20;
	CHECK(!cdn_frame_from_buffer(&b,&fmt,&f)); chunks[1].size=288;
	chunks[1].stride=-32;
	CHECK(!cdn_frame_from_buffer(&b,&fmt,&f)); chunks[1].stride=32;
	chunks[1].stride=16;
	CHECK(!cdn_frame_from_buffer(&b,&fmt,&f)); chunks[1].stride=32;
	chunks[1].offset=UINT32_MAX;
	CHECK(!cdn_frame_from_buffer(&b,&fmt,&f)); chunks[1].offset=7;
	data[1].type=SPA_DATA_DmaBuf; data[1].fd=fd; data[1].mapoffset=UINT32_MAX;
	CHECK(!cdn_frame_dmabuf(&b,&f,&dma)); data[1].mapoffset=8192;
	b.n_datas=0; CHECK(!cdn_frame_from_buffer(&b,&fmt,&f)); CHECK(!cdn_frame_dmabuf(&b,&f,&dma));
	b.n_datas=3; CHECK(!cdn_frame_from_buffer(&b,&fmt,&f)); CHECK(!cdn_frame_dmabuf(&b,&f,&dma));
	b.n_datas=1; chunks[0].size=432; CHECK(!cdn_frame_from_buffer(&b,&fmt,&f));
	close(fd);
	return fails ? 1 : 0;
}
