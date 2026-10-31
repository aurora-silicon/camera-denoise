/* SPDX-License-Identifier: MIT */
/* Host-only synthetic NV12 source/sink. Never opens a device. */
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
struct fixture { struct pw_main_loop *loop; struct pw_stream *stream; bool sink; unsigned frames; int status; int64_t last_pts; uint64_t last_seq; };
static void quit(void *data, int sig) { (void)sig; pw_main_loop_quit(((struct fixture *)data)->loop); }
static void tick(void *data, uint64_t count) { (void)count; pw_stream_trigger_process(((struct fixture *)data)->stream); }
static void format(void *data, uint32_t id, const struct spa_pod *param)
{
	struct fixture *f=data;
	if (id!=SPA_PARAM_Format || !param) return;
	uint8_t buf[512];struct spa_pod_builder b=SPA_POD_BUILDER_INIT(buf,sizeof(buf));
	const struct spa_pod *params[2];
	params[0]=spa_pod_builder_add_object(&b,SPA_TYPE_OBJECT_ParamBuffers,SPA_PARAM_Buffers,
		SPA_PARAM_BUFFERS_buffers,SPA_POD_CHOICE_RANGE_Int(4,2,8),
		SPA_PARAM_BUFFERS_blocks,SPA_POD_Int(1),SPA_PARAM_BUFFERS_size,SPA_POD_Int(64*32*3/2),
		SPA_PARAM_BUFFERS_stride,SPA_POD_Int(64),SPA_PARAM_BUFFERS_dataType,SPA_POD_CHOICE_FLAGS_Int((1<<SPA_DATA_MemFd)|(1<<SPA_DATA_MemPtr)));
	params[1]=spa_pod_builder_add_object(&b,SPA_TYPE_OBJECT_ParamMeta,SPA_PARAM_Meta,
		SPA_PARAM_META_type,SPA_POD_Id(SPA_META_Header),SPA_PARAM_META_size,SPA_POD_Int(sizeof(struct spa_meta_header)));
	pw_stream_update_params(f->stream,params,2);
}
static void process(void *data)
{
	struct fixture *f=data;
	struct pw_buffer *buffer=pw_stream_dequeue_buffer(f->stream);
	if (!buffer) return;
	struct spa_data *d=&buffer->buffer->datas[0];
	struct spa_meta_header *header=spa_buffer_find_meta_data(buffer->buffer,SPA_META_Header,sizeof(*header));
	if (f->sink) {
		if (d->chunk && d->chunk->size) {
			if (!d->data || ((unsigned char *)d->data)[0]!=1 || !header || header->pts<=f->last_pts ||
				(f->frames && header->seq<=f->last_seq)) {
				f->status=2;pw_main_loop_quit(f->loop);
				pw_stream_queue_buffer(f->stream,buffer);
				return;
			}
			if (header) { f->last_pts=header->pts;f->last_seq=header->seq; }
			if (++f->frames==20) { f->status=0;pw_main_loop_quit(f->loop); }
		}
	} else if (d->data && d->chunk && d->maxsize>=64*32*3/2) {
		memset(d->data,128,64*32*3/2);
		d->chunk->offset=0;d->chunk->stride=64;d->chunk->size=64*32*3/2;
		if (header) {
			struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
			*header=(struct spa_meta_header){.pts=(int64_t)t.tv_sec*1000000000+t.tv_nsec,.seq=f->frames++};
		}
	}
	pw_stream_queue_buffer(f->stream,buffer);
}
int main(int argc, char **argv)
{
	struct fixture f={.sink=argc>1 && !strcmp(argv[1],"--sink"),.status=1};
	pw_init(&argc,&argv);f.loop=pw_main_loop_new(NULL);
	static const struct pw_stream_events events={PW_VERSION_STREAM_EVENTS,.param_changed=format,.process=process};
	struct pw_properties *props=pw_properties_new(PW_KEY_NODE_NAME,f.sink?"effects-test-sink":"camera-effects-fixture",
		PW_KEY_MEDIA_CLASS,f.sink?"Video/Sink":"Video/Source",PW_KEY_MEDIA_TYPE,"Video",
		PW_KEY_MEDIA_CATEGORY,f.sink?"Playback":"Capture","api.v4l2.cap.driver","apple-isp",NULL);
	f.stream=pw_stream_new_simple(pw_main_loop_get_loop(f.loop),"effects fixture",props,&events,&f);
	uint8_t buf[512];struct spa_pod_builder b=SPA_POD_BUILDER_INIT(buf,sizeof(buf));
	struct spa_video_info_raw info={.format=SPA_VIDEO_FORMAT_NV12,.size={64,32},.framerate={30,1}};
	const struct spa_pod *fmt=spa_format_video_raw_build(&b,SPA_PARAM_EnumFormat,&info);
	if (pw_stream_connect(f.stream,f.sink?PW_DIRECTION_INPUT:PW_DIRECTION_OUTPUT,PW_ID_ANY,
		PW_STREAM_FLAG_MAP_BUFFERS|(f.sink?0:PW_STREAM_FLAG_DRIVER),&fmt,1)<0) return 1;
	if (!f.sink) {
		struct spa_source *timer=pw_loop_add_timer(pw_main_loop_get_loop(f.loop),tick,&f);
		struct timespec first={0,10000000},interval={0,33333333};
		pw_loop_update_timer(pw_main_loop_get_loop(f.loop),timer,&first,&interval,false);
	}
	pw_loop_add_signal(pw_main_loop_get_loop(f.loop),SIGTERM,quit,&f);
	pw_main_loop_run(f.loop);
	pw_stream_destroy(f.stream);pw_main_loop_destroy(f.loop);pw_deinit();
	if (f.sink) printf("synthetic sink received %u complete effect frames, status %d\n",f.frames,f.status);
	return f.sink?f.status:0;
}
