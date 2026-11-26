// SPDX-License-Identifier: MIT
/* Test transport layout and bounds without a PipeWire server or camera. */
#include <limits.h>
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

	/* Output layout: stride rounded to 16, NV12 size, no 32-bit wrap. */
	int stride = 0;
	uint32_t size = 0;
	CHECK(cdn_output_layout(18,18,&stride,&size) && stride==32 && size==32*18+32*9);
	CHECK(cdn_output_layout(1920,1080,&stride,&size) && stride==1920 && size==1920*1080*3/2);
	CHECK(cdn_output_layout(CDN_MAX_FRAME_DIM,CDN_MAX_FRAME_DIM,&stride,&size) &&
	      stride==CDN_MAX_FRAME_DIM && size==(uint32_t)CDN_MAX_FRAME_DIM*CDN_MAX_FRAME_DIM*3/2);
	CHECK(!cdn_output_layout(CDN_MAX_FRAME_DIM+2,2,&stride,&size));
	CHECK(!cdn_output_layout(2,CDN_MAX_FRAME_DIM+2,&stride,&size));
	CHECK(!cdn_output_layout(0,2,&stride,&size) && !cdn_output_layout(2,0,&stride,&size));
	CHECK(!cdn_output_layout(17,18,&stride,&size) && !cdn_output_layout(18,17,&stride,&size));
	/* Its real output needs 2^32 + 131072 bytes; 32-bit arithmetic says 131072. */
	CHECK(!cdn_output_layout(65522,43692,&stride,&size));
	CHECK(!cdn_output_layout(INT_MAX-1,2,&stride,&size));
	CHECK(!cdn_output_layout(UINT32_MAX-1,2,&stride,&size));	/* stride rounding wraps */
	CHECK(!cdn_output_layout(UINT32_MAX-1,UINT32_MAX-1,&stride,&size));

	/* The output mapping must hold the whole frame, computed in 64 bits. */
	static uint8_t out[32*27];
	struct spa_data od = {.type=SPA_DATA_MemPtr,.data=out,.maxsize=sizeof(out)};
	struct cdn_frame o;
	CHECK(cdn_output_layout(18,18,&stride,&size) && size==sizeof(out));
	CHECK(cdn_output_frame(&od,18,18,stride,&o));
	CHECK(o.y==out && o.uv==out+32*18 && o.stride_y==32 && o.stride_uv==32 &&
	      o.width==18 && o.height==18);
	od.maxsize=sizeof(out)-1; CHECK(!cdn_output_frame(&od,18,18,stride,&o)); od.maxsize=sizeof(out);
	od.data=NULL; CHECK(!cdn_output_frame(&od,18,18,stride,&o)); od.data=out;
	CHECK(!cdn_output_frame(&od,18,18,16,&o));		/* stride below the width */
	CHECK(!cdn_output_frame(&od,18,18,0,&o));
	/* A 128 KiB mapping for the wrapping 65522x43692 frame is refused. */
	od.maxsize=131072;
	CHECK(!cdn_output_frame(&od,65522,43692,65536,&o));
	return fails ? 1 : 0;
}
