// SPDX-License-Identifier: MIT
/*
 * aurora-camera-denoise: a PipeWire video filter for built-in cameras.
 *
 * For every PipeWire camera source whose V4L2 driver is listed in the
 * config (apple-isp by default), publish a Video/Source node with the same
 * formats (NV12). When a client streams from it, connect to the camera,
 * denoise each frame and pass it on. With no client, the camera is not
 * touched.
 *
 * Like macOS, two stages. The temporal filter runs on every frame, as
 * macOS runs its motion-compensated temporal filter on every camera
 * stream. A low-light stage (a backend plugin such as the Neural Engine
 * one) joins in when the trigger says so: it follows the sensor gain the
 * driver reports per frame, with per-board enter/exit thresholds and a
 * ramp, like macOS' automatic ML video noise reduction. With temporal =
 * off the trigger gates the one backend instead, and frames below the
 * threshold are copied unchanged.
 */
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>
#include <spa/debug/format.h>

#include "backend.h"
#include "config.h"
#include "gain.h"
#include "trigger.h"

#ifndef CDN_DATADIR
#define CDN_DATADIR "/usr/share/aurora-camera-denoise"
#endif
#ifndef CDN_SYSCONFDIR
#define CDN_SYSCONFDIR "/etc"
#endif

#define OWN_PROP "aurora.camera-denoise"
#define MAX_FORMATS 16

struct app;

struct cam {
	struct spa_list link;
	struct app *app;
	uint32_t src_id;
	char src_name[256];
	char src_desc[256];
	char src_nick[256];
	char v4l2_path[128];
	int src_prio;

	/* source node proxy, for its formats */
	struct pw_proxy *src_proxy;
	struct spa_hook src_listener;
	int enum_seq, sync_seq;
	struct spa_pod *formats[MAX_FORMATS];
	int n_formats;
	bool formats_done;
	/* decided from the node's full properties (registry globals lack them) */
	bool checked, accepted;

	struct pw_stream *out;
	struct spa_hook out_listener;
	struct pw_stream *in;
	struct spa_hook in_listener;
	bool out_streaming;

	struct spa_video_info_raw fmt; /* negotiated on out */
	bool have_fmt;
	int out_stride;

	/* the temporal stage, or with temporal = off the gated backend */
	const struct cdn_backend_ops *ops;
	void *bstate;
	/* the low-light stage on top of the temporal one, if a plugin runs */
	const struct cdn_backend_ops *ml_ops;
	void *ml_state;
	uint8_t *stage;
	size_t stage_size;
	struct cdn_trigger trig;
	struct cdn_gain gain;
	bool gain_tried;
	struct cdn_noise_tracker noise;

	/*
	 * Cached copy of the input frame. The camera's DMA-BUFs map uncached,
	 * so the filter reads them once, sequentially, instead of in every pass.
	 */
	uint8_t *copy;
	size_t copy_size;

	/* stats */
	uint64_t frames, denoised, boosted;
	double busy_ms, copy_ms;
	uint64_t last_pts;
	float fps_meas;
	time_t last_log;
};

struct app {
	struct pw_main_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct spa_hook core_listener;
	struct pw_registry *registry;
	struct spa_hook registry_listener;
	struct spa_list cams;
	struct cdn_config cfg;
	const char *cli_mode, *cli_backend;
	const char *boards_path;
	bool verbose;
};

static void cam_stop_input(struct cam *c);
static int cam_start_input(struct cam *c);

/* ---------------------------------------------------------------------- */

static int load_config(struct app *a)
{
	char user[PATH_MAX];
	const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
	const char *settings[2] = { CDN_SYSCONFDIR "/aurora-camera-denoise/denoise.conf", NULL };

	if (xdg && *xdg)
		snprintf(user, sizeof(user), "%s/aurora-camera-denoise/denoise.conf", xdg);
	else if (home)
		snprintf(user, sizeof(user), "%s/.config/aurora-camera-denoise/denoise.conf", home);
	else
		user[0] = 0;
	settings[1] = user[0] ? user : NULL;
	if (cdn_config_load(&a->cfg, "/proc/device-tree/compatible", a->boards_path,
			    settings, 2))
		return -1;
	if (a->cli_mode && cdn_mode_parse(a->cli_mode, &a->cfg.mode)) {
		fprintf(stderr, "camera-denoise: bad --mode %s\n", a->cli_mode);
		return -1;
	}
	if (a->cli_backend)
		snprintf(a->cfg.backend, sizeof(a->cfg.backend), "%s", a->cli_backend);
	cdn_config_print(&a->cfg);
	return 0;
}

static bool driver_listed(const struct app *a, const char *drv)
{
	char buf[128], *save = NULL;

	if (!drv)
		return false;
	snprintf(buf, sizeof(buf), "%s", a->cfg.drivers);
	for (char *t = strtok_r(buf, ", ", &save); t; t = strtok_r(NULL, ", ", &save))
		if (!strcmp(t, drv))
			return true;
	return false;
}

/* ---------------------------------------------------------------------- */
/* frame plumbing                                                          */

static bool frame_from_buffer(struct spa_buffer *b, const struct spa_video_info_raw *f,
			      struct cdn_frame *fr)
{
	struct spa_data *d = &b->datas[0];
	int w = (int)f->size.width, h = (int)f->size.height;

	if (!d->data || !d->chunk)
		return false;
	fr->width = w;
	fr->height = h;
	fr->stride_y = d->chunk->stride > 0 ? d->chunk->stride : w;
	fr->y = (uint8_t *)d->data + d->chunk->offset;
	if (b->n_datas >= 2 && b->datas[1].data && b->datas[1].chunk) {
		struct spa_data *u = &b->datas[1];
		fr->uv = (uint8_t *)u->data + u->chunk->offset;
		fr->stride_uv = u->chunk->stride > 0 ? u->chunk->stride : fr->stride_y;
	} else {
		size_t need = (size_t)fr->stride_y * h * 3 / 2;
		if (d->chunk->size && d->chunk->size < need)
			return false;
		if (d->maxsize < d->chunk->offset + need)
			return false;
		fr->uv = fr->y + (size_t)fr->stride_y * h;
		fr->stride_uv = fr->stride_y;
	}
	return true;
}

static void log_stats(struct cam *c, float strength, uint32_t gain, float noise)
{
	time_t now = time(NULL);

	if (!c->app->verbose || now - c->last_log < 5)
		return;
	c->last_log = now;
	fprintf(stderr,
		"camera-denoise: %s: %llu frames, %llu denoised, %llu low-light, %.2f ms per "
		"denoised frame (%.2f ms of it copying), gain %u, noise %.2f, fps %.1f, "
		"low-light strength %.2f\n",
		c->src_nick, (unsigned long long)c->frames, (unsigned long long)c->denoised,
		(unsigned long long)c->boosted, c->denoised ? c->busy_ms / c->denoised : 0.0,
		c->denoised ? c->copy_ms / c->denoised : 0.0, gain, noise, c->fps_meas, strength);
}

static double mono_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void on_in_process(void *data)
{
	struct cam *c = data;
	struct pw_buffer *ib, *ob;
	struct cdn_frame fi, fo;
	struct spa_meta_header *ih, *oh;
	uint32_t gain = 0, exposure = 0;
	float noise = -1, fps, strength;
	int w, h;

	ib = pw_stream_dequeue_buffer(c->in);
	if (!ib)
		return;
	if (!c->have_fmt || !c->out_streaming) {
		pw_stream_queue_buffer(c->in, ib);
		return;
	}
	ob = pw_stream_dequeue_buffer(c->out);
	if (!ob) {
		/* the client is behind: drop this frame */
		pw_stream_queue_buffer(c->in, ib);
		return;
	}
	w = (int)c->fmt.size.width;
	h = (int)c->fmt.size.height;

	struct spa_data *od = &ob->buffer->datas[0];
	if (!frame_from_buffer(ib->buffer, &c->fmt, &fi) || !od->data ||
	    od->maxsize < (uint32_t)c->out_stride * h * 3 / 2) {
		pw_stream_queue_buffer(c->in, ib);
		pw_stream_queue_buffer(c->out, ob);
		return;
	}
	fo = (struct cdn_frame){ od->data, (uint8_t *)od->data + (size_t)c->out_stride * h,
				 w, h, c->out_stride, c->out_stride };

	/* timing and frame rate */
	ih = spa_buffer_find_meta_data(ib->buffer, SPA_META_Header, sizeof(*ih));
	if (ih && ih->pts > 0) {
		if (c->last_pts && (uint64_t)ih->pts > c->last_pts) {
			float f = 1e9f / (float)((uint64_t)ih->pts - c->last_pts);
			c->fps_meas = c->fps_meas > 0 ? c->fps_meas * 0.9f + f * 0.1f : f;
		}
		c->last_pts = (uint64_t)ih->pts;
	}
	fps = c->fmt.framerate.denom ?
		(float)c->fmt.framerate.num / (float)c->fmt.framerate.denom : c->fps_meas;

	/* what the sensor is doing */
	if (!c->gain_tried) {
		c->gain_tried = true;
		if (cdn_gain_open(&c->gain, c->v4l2_path))
			fprintf(stderr, "camera-denoise: %s: using the sensor gain from %s\n",
				c->src_nick, c->v4l2_path);
		else
			fprintf(stderr, "camera-denoise: %s: no gain report from %s, "
				"triggering on measured noise only\n", c->src_nick, c->v4l2_path);
	}
	gain = cdn_gain_read(&c->gain, &exposure);
	noise = cdn_noise_track(&c->noise, fi.y, fi.stride_y, w, h);
	strength = cdn_trigger_step(&c->trig, gain, noise, fps);
	c->frames++;

	bool temporal = c->app->cfg.temporal && c->app->cfg.mode != CDN_MODE_OFF;

	if ((temporal || strength > 0) && c->bstate) {
		struct cdn_frame_info info = { gain, exposure, ih ? (uint64_t)ih->pts : 0 };
		bool boost = temporal && c->ml_state && strength > 0;
		double t0 = mono_ms();
		size_t need = (size_t)w * h * 3 / 2;

		/*
		 * Every pass reads the input, so read the camera's buffer once,
		 * sequentially, into cached memory first.
		 */
		if (c->copy_size < need) {
			free(c->copy);
			c->copy = malloc(need);
			c->copy_size = c->copy ? need : 0;
		}
		if (c->copy) {
			struct cdn_frame cf = { c->copy, c->copy + (size_t)w * h, w, h, w, w };
			cdn_frame_copy(&fi, &cf);
			fi = cf;
		}
		c->copy_ms += mono_ms() - t0;
		if (boost && c->stage_size < need) {
			free(c->stage);
			c->stage = malloc(need);
			c->stage_size = c->stage ? need : 0;
		}
		if (boost && c->stage) {
			/* temporal result B, then the low-light stage blends a*M(B) over it */
			struct cdn_frame sf = { c->stage, c->stage + (size_t)w * h, w, h, w, w };

			if (c->ops->process(c->bstate, &fi, &sf, &info, 1.0f))
				cdn_frame_copy(&fi, &sf);
			if (c->ml_ops->process(c->ml_state, &sf, &fo, &info, strength))
				cdn_frame_copy(&sf, &fo);
			c->boosted++;
		} else if (c->ops->process(c->bstate, &fi, &fo, &info,
					   temporal ? 1.0f : strength)) {
			cdn_frame_copy(&fi, &fo);
		}
		if (!boost && c->ml_state && c->ml_ops->reset)
			c->ml_ops->reset(c->ml_state);
		c->busy_ms += mono_ms() - t0;
		c->denoised++;
	} else {
		cdn_frame_copy(&fi, &fo);
		/* start clean when the filter comes back */
		if (c->bstate && c->ops->reset)
			c->ops->reset(c->bstate);
	}

	od->chunk->offset = 0;
	od->chunk->stride = c->out_stride;
	od->chunk->size = (uint32_t)c->out_stride * h * 3 / 2;
	od->chunk->flags = 0;
	oh = spa_buffer_find_meta_data(ob->buffer, SPA_META_Header, sizeof(*oh));
	if (oh) {
		if (ih)
			*oh = *ih;
		else
			oh->pts = -1;
	}
	pw_stream_queue_buffer(c->in, ib);
	pw_stream_queue_buffer(c->out, ob);
	pw_stream_trigger_process(c->out);
	log_stats(c, strength, gain, noise);
}

static void on_in_state(void *data, enum pw_stream_state old, enum pw_stream_state state,
			const char *error)
{
	struct cam *c = data;

	if (c->app->verbose || state == PW_STREAM_STATE_ERROR)
		fprintf(stderr, "camera-denoise: %s: input %s%s%s\n", c->src_nick,
			pw_stream_state_as_string(state), error ? ": " : "", error ? error : "");
	(void)old;
}

static void on_in_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
	struct cam *c = data;
	uint8_t buf[512];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	const struct spa_pod *params[2];

	if (id != SPA_PARAM_Format || !param)
		return;
	/* the camera exports DMA-BUFs; take those (mapped) or plain memory */
	params[0] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 16),
		SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(
			(1 << SPA_DATA_DmaBuf) | (1 << SPA_DATA_MemFd) |
			(1 << SPA_DATA_MemPtr)));
	params[1] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
		SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
	pw_stream_update_params(c->in, params, 2);
}

static const struct pw_stream_events in_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_in_state,
	.param_changed = on_in_param_changed,
	.process = on_in_process,
};

static int cam_start_input(struct cam *c)
{
	uint8_t buf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	const struct spa_pod *params[1];
	struct spa_video_info_raw r = c->fmt;
	struct pw_properties *props;

	if (c->in || !c->have_fmt)
		return 0;
	props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Video",
				  PW_KEY_MEDIA_CATEGORY, "Capture",
				  PW_KEY_MEDIA_ROLE, "Camera",
				  PW_KEY_NODE_NAME, "aurora_camera_denoise.input",
				  PW_KEY_NODE_DESCRIPTION, "Camera denoise input",
				  PW_KEY_TARGET_OBJECT, c->src_name,
				  PW_KEY_NODE_DONT_RECONNECT, "true",
				  OWN_PROP, "input", NULL);
	c->in = pw_stream_new(c->app->core, "camera-denoise-input", props);
	if (!c->in)
		return -errno;
	pw_stream_add_listener(c->in, &c->in_listener, &in_events, c);

	r.format = SPA_VIDEO_FORMAT_NV12;
	params[0] = spa_format_video_raw_build(&b, SPA_PARAM_EnumFormat, &r);
	/*
	 * Before the camera streams: may it slow down in low light? A camera
	 * that is already streaming for another client keeps its setting.
	 */
	if (c->v4l2_path[0]) {
		int err = cdn_v4l2_set_exposure_priority(c->v4l2_path,
							 c->app->cfg.low_light_frame_rate);
		if (err && err != -ENOTTY)
			fprintf(stderr, "camera-denoise: %s: low-light frame rate not set: %s\n",
				c->src_nick, strerror(-err));
	}
	c->last_pts = 0;
	c->fps_meas = 0;
	return pw_stream_connect(c->in, PW_DIRECTION_INPUT, PW_ID_ANY,
				 PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
				 params, 1);
}

static void cam_stop_input(struct cam *c)
{
	if (!c->in)
		return;
	spa_hook_remove(&c->in_listener);
	pw_stream_destroy(c->in);
	c->in = NULL;
	cdn_gain_close(&c->gain);
	c->gain_tried = false;
	cdn_noise_tracker_reset(&c->noise);
	if (c->bstate && c->ops->reset)
		c->ops->reset(c->bstate);
	if (c->ml_state && c->ml_ops->reset)
		c->ml_ops->reset(c->ml_state);
}

/* ---------------------------------------------------------------------- */
/* output node                                                             */

static void setup_backend(struct cam *c)
{
	int w = (int)c->fmt.size.width, h = (int)c->fmt.size.height;
	const struct cdn_config *cfg = &c->app->cfg;

	if (c->bstate)
		c->ops->destroy(c->bstate);
	c->bstate = NULL;
	if (c->ml_state)
		c->ml_ops->destroy(c->ml_state);
	c->ml_state = NULL;
	c->ml_ops = NULL;

	if (cfg->temporal) {
		/*
		 * The temporal filter on every frame: the encoder's motion-
		 * compensated filter when the driver offers it, as on macOS,
		 * else the CPU. A plugin on top for low light.
		 */
		bool tauto = !strcmp(cfg->temporal_backend, "auto");

		c->ops = cdn_backend_find(tauto ? "auto" : cfg->temporal_backend,
					  tauto ? "ave" : NULL);
		if (c->ops)
			c->bstate = c->ops->create(w, h, cfg->threads);
		if (!c->bstate && c->ops != cdn_cpu_backend()) {
			if (!tauto)
				fprintf(stderr, "camera-denoise: temporal backend %s unavailable "
					"for %dx%d, using cpu\n", cfg->temporal_backend, w, h);
			c->ops = cdn_cpu_backend();
			c->bstate = c->ops->create(w, h, cfg->threads);
		}
		c->ml_ops = cdn_backend_find(cfg->backend, cfg->prefer);
		if (c->ml_ops == cdn_cpu_backend())
			c->ml_ops = NULL;
		if (c->ml_ops)
			c->ml_state = c->ml_ops->create(w, h, cfg->threads);
		if (c->ml_ops && !c->ml_state)
			fprintf(stderr, "camera-denoise: backend %s unavailable for %dx%d, "
				"temporal filter only\n", c->ml_ops->name, w, h);
		fprintf(stderr, "camera-denoise: %s: %dx%d@%u/%u, temporal %s, low light %s\n",
			c->src_nick, w, h, c->fmt.framerate.num, c->fmt.framerate.denom,
			c->bstate ? c->ops->name : "none",
			c->ml_state ? c->ml_ops->name : "none");
		return;
	}

	c->ops = cdn_backend_find(cfg->backend, cfg->prefer);
	if (c->ops)
		c->bstate = c->ops->create(w, h, cfg->threads);
	if (!c->bstate && c->ops != cdn_cpu_backend()) {
		fprintf(stderr, "camera-denoise: backend %s unavailable for %dx%d, using cpu\n",
			c->ops ? c->ops->name : cfg->backend, w, h);
		c->ops = cdn_cpu_backend();
		c->bstate = c->ops->create(w, h, cfg->threads);
	}
	fprintf(stderr, "camera-denoise: %s: %dx%d@%u/%u, backend %s\n", c->src_nick, w, h,
		c->fmt.framerate.num, c->fmt.framerate.denom, c->bstate ? c->ops->name : "none");
}

static void on_out_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
	struct cam *c = data;
	uint8_t buf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	const struct spa_pod *params[2];
	uint32_t mtype, msub;
	struct spa_video_info_raw r = { 0 };

	if (id != SPA_PARAM_Format)
		return;
	if (!param) {
		c->have_fmt = false;
		cam_stop_input(c);
		return;
	}
	if (spa_format_parse(param, &mtype, &msub) < 0 || mtype != SPA_MEDIA_TYPE_video ||
	    msub != SPA_MEDIA_SUBTYPE_raw || spa_format_video_raw_parse(param, &r) < 0 ||
	    r.format != SPA_VIDEO_FORMAT_NV12 || !r.size.width || !r.size.height ||
	    (r.size.width & 1) || (r.size.height & 1)) {
		pw_stream_set_error(c->out, -EINVAL, "only even-sized NV12 is supported");
		return;
	}
	bool changed = !c->have_fmt || memcmp(&r.size, &c->fmt.size, sizeof(r.size)) ||
		       memcmp(&r.framerate, &c->fmt.framerate, sizeof(r.framerate));
	c->fmt = r;
	c->have_fmt = true;
	c->out_stride = SPA_ROUND_UP_N((int)r.size.width, 16);

	params[0] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 8),
		SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
		SPA_PARAM_BUFFERS_size, SPA_POD_Int(c->out_stride * (int)r.size.height * 3 / 2),
		SPA_PARAM_BUFFERS_stride, SPA_POD_Int(c->out_stride),
		SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(
			(1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr)));
	params[1] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
		SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
	pw_stream_update_params(c->out, params, 2);

	if (changed) {
		setup_backend(c);
		cam_stop_input(c);
		if (c->out_streaming)
			cam_start_input(c);
	}
}

static void on_out_state(void *data, enum pw_stream_state old, enum pw_stream_state state,
			 const char *error)
{
	struct cam *c = data;

	if (c->app->verbose || state == PW_STREAM_STATE_ERROR)
		fprintf(stderr, "camera-denoise: %s: output %s%s%s\n", c->src_nick,
			pw_stream_state_as_string(state), error ? ": " : "", error ? error : "");
	c->out_streaming = state == PW_STREAM_STATE_STREAMING;
	if (c->out_streaming) {
		cdn_trigger_init(&c->trig, &c->app->cfg.trig, c->app->cfg.mode);
		cam_start_input(c);
	} else if (old == PW_STREAM_STATE_STREAMING) {
		/* no client: release the camera */
		cam_stop_input(c);
	}
}

static const struct pw_stream_events out_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_out_state,
	.param_changed = on_out_param_changed,
};

static bool pod_is_nv12(const struct spa_pod *p)
{
	const struct spa_pod_prop *prop;
	uint32_t n, choice, *vals;
	const struct spa_pod *v;

	if (!spa_pod_is_object(p))
		return false;
	prop = spa_pod_find_prop(p, NULL, SPA_FORMAT_VIDEO_format);
	if (!prop)
		return false;
	v = spa_pod_get_values(&prop->value, &n, &choice);
	if (v->type != SPA_TYPE_Id)
		return false;
	vals = SPA_POD_BODY(v);
	for (uint32_t i = 0; i < n; i++)
		if (vals[i] == SPA_VIDEO_FORMAT_NV12)
			return true;
	return false;
}

static int cam_publish(struct cam *c)
{
	struct pw_properties *props;
	const struct spa_pod *params[MAX_FORMATS];
	uint8_t buf[4096];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	int n = 0;

	for (int i = 0; i < c->n_formats; i++) {
		/* the camera's NV12 format with its sizes and rates, as is */
		struct spa_pod_frame f;
		const struct spa_pod_object *o = (const struct spa_pod_object *)c->formats[i];
		struct spa_pod_prop *prop;

		spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
		SPA_POD_OBJECT_FOREACH(o, prop) {
			if (prop->key == SPA_FORMAT_VIDEO_format) {
				spa_pod_builder_prop(&b, SPA_FORMAT_VIDEO_format, 0);
				spa_pod_builder_id(&b, SPA_VIDEO_FORMAT_NV12);
			} else {
				spa_pod_builder_raw_padded(&b, prop, SPA_POD_PROP_SIZE(prop));
			}
		}
		params[n++] = spa_pod_builder_pop(&b, &f);
	}
	if (!n) {
		fprintf(stderr, "camera-denoise: %s offers no NV12 format\n", c->src_nick);
		return -ENOTSUP;
	}

	char name[300], desc[300], prio[16];
	snprintf(name, sizeof(name), "aurora_camera_denoise.%s", c->src_name);
	/* the clean picture is the camera; the raw node stays as "(V4L2)" */
	snprintf(desc, sizeof(desc), "%s", c->src_nick[0] ? c->src_nick : c->src_desc);
	snprintf(prio, sizeof(prio), "%d", c->src_prio + 10);
	props = pw_properties_new(PW_KEY_MEDIA_CLASS, "Video/Source",
				  PW_KEY_MEDIA_ROLE, "Camera",
				  PW_KEY_MEDIA_TYPE, "Video",
				  PW_KEY_MEDIA_CATEGORY, "Capture",
				  PW_KEY_NODE_NAME, name,
				  PW_KEY_NODE_DESCRIPTION, desc,
				  PW_KEY_NODE_NICK, c->src_nick,
				  PW_KEY_PRIORITY_SESSION, prio,
				  OWN_PROP, "output", NULL);
	c->out = pw_stream_new(c->app->core, "camera-denoise", props);
	if (!c->out)
		return -errno;
	pw_stream_add_listener(c->out, &c->out_listener, &out_events, c);
	fprintf(stderr, "camera-denoise: publishing %s for %s (%d formats)\n", name,
		c->src_name, n);
	return pw_stream_connect(c->out, PW_DIRECTION_OUTPUT, PW_ID_ANY,
				 PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_MAP_BUFFERS,
				 params, (uint32_t)n);
}

/* ---------------------------------------------------------------------- */
/* source discovery                                                        */

static void src_param(void *data, int seq, uint32_t id, uint32_t index, uint32_t next,
		      const struct spa_pod *param)
{
	struct cam *c = data;

	(void)index; (void)next; (void)seq;
	/* replies to our enum_params, until the core sync after it returns */
	if (!c->accepted || c->formats_done || id != SPA_PARAM_EnumFormat || !param)
		return;
	if (c->app->verbose) {
		fprintf(stderr, "camera-denoise: %s format %u:\n", c->src_nick, index);
		spa_debug_format(4, NULL, param);
	}
	if (c->n_formats < MAX_FORMATS && pod_is_nv12(param))
		c->formats[c->n_formats++] = spa_pod_copy(param);
}

static void src_info(void *data, const struct pw_node_info *info)
{
	struct cam *c = data;
	struct app *a = c->app;
	const struct spa_dict *props = info->props;
	const char *drv, *name, *p;

	if (c->checked || !props)
		return;
	c->checked = true;
	drv = spa_dict_lookup(props, "api.v4l2.cap.driver");
	name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
	if (!name || !driver_listed(a, drv))
		return;
	c->accepted = true;
	snprintf(c->src_name, sizeof(c->src_name), "%s", name);
	snprintf(c->src_desc, sizeof(c->src_desc), "%s",
		 spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION) ?: name);
	snprintf(c->src_nick, sizeof(c->src_nick), "%s",
		 spa_dict_lookup(props, PW_KEY_NODE_NICK) ?: c->src_desc);
	snprintf(c->v4l2_path, sizeof(c->v4l2_path), "%s",
		 spa_dict_lookup(props, "api.v4l2.path") ?: "");
	p = spa_dict_lookup(props, PW_KEY_PRIORITY_SESSION);
	c->src_prio = p ? atoi(p) : 0;
	fprintf(stderr, "camera-denoise: found %s (%s, %s)\n", c->src_name, drv,
		c->v4l2_path);

	c->enum_seq = pw_node_enum_params((struct pw_node *)c->src_proxy, 0,
					  SPA_PARAM_EnumFormat, 0, UINT32_MAX, NULL);
	c->sync_seq = pw_core_sync(a->core, PW_ID_CORE, 0);
}

static const struct pw_node_events src_events = {
	PW_VERSION_NODE_EVENTS,
	.info = src_info,
	.param = src_param,
};

static void on_core_done(void *data, uint32_t id, int seq)
{
	struct app *a = data;
	struct cam *c;

	if (id != PW_ID_CORE)
		return;
	spa_list_for_each(c, &a->cams, link)
		if (c->accepted && !c->formats_done && seq == c->sync_seq) {
			c->formats_done = true;
			if (cam_publish(c) < 0)
				fprintf(stderr, "camera-denoise: cannot publish %s\n", c->src_name);
		}
}

static void on_core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	struct app *a = data;

	fprintf(stderr, "camera-denoise: core error %d on %u: %s\n", res, id, message);
	if (id == PW_ID_CORE && res == -EPIPE)
		pw_main_loop_quit(a->loop);
	(void)seq;
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.done = on_core_done,
	.error = on_core_error,
};

static void cam_destroy(struct cam *c)
{
	cam_stop_input(c);
	if (c->out) {
		spa_hook_remove(&c->out_listener);
		pw_stream_destroy(c->out);
	}
	if (c->src_proxy) {
		spa_hook_remove(&c->src_listener);
		pw_proxy_destroy(c->src_proxy);
	}
	for (int i = 0; i < c->n_formats; i++)
		free(c->formats[i]);
	if (c->bstate)
		c->ops->destroy(c->bstate);
	if (c->ml_state)
		c->ml_ops->destroy(c->ml_state);
	cdn_noise_tracker_free(&c->noise);
	free(c->copy);
	free(c->stage);
	spa_list_remove(&c->link);
	free(c);
}

static void on_global(void *data, uint32_t id, uint32_t perms, const char *type,
		      uint32_t version, const struct spa_dict *props)
{
	struct app *a = data;
	const char *cls;
	struct cam *c;

	(void)perms; (void)version;
	if (strcmp(type, PW_TYPE_INTERFACE_Node) || !props)
		return;
	cls = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
	if (!cls || strcmp(cls, "Video/Source") || spa_dict_lookup(props, OWN_PROP))
		return;
	/* no temporal filter, nothing to decide on and not forced on: leave cameras alone */
	if (!a->cfg.temporal && a->cfg.mode != CDN_MODE_ON && !a->cfg.trig.enter_gain &&
	    a->cfg.trig.enter_noise <= 0) {
		static bool said;
		if (!said)
			fprintf(stderr, "camera-denoise: no trigger data for this board, "
				"not filtering (set mode = on to force)\n");
		said = true;
		return;
	}

	/* a candidate; src_info() decides from the node's full properties */
	c = calloc(1, sizeof(*c));
	if (!c)
		return;
	c->app = a;
	c->src_id = id;
	c->gain.fd = -1;
	spa_list_append(&a->cams, &c->link);
	c->src_proxy = pw_registry_bind(a->registry, id, PW_TYPE_INTERFACE_Node,
					PW_VERSION_NODE, 0);
	if (!c->src_proxy) {
		spa_list_remove(&c->link);
		free(c);
		return;
	}
	pw_proxy_add_object_listener(c->src_proxy, &c->src_listener, &src_events, c);
}

static void on_global_remove(void *data, uint32_t id)
{
	struct app *a = data;
	struct cam *c, *t;

	spa_list_for_each_safe(c, t, &a->cams, link)
		if (c->src_id == id) {
			if (c->accepted)
				fprintf(stderr, "camera-denoise: %s went away\n", c->src_name);
			cam_destroy(c);
		}
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = on_global,
	.global_remove = on_global_remove,
};

static void on_signal(void *data, int sig)
{
	struct app *a = data;
	struct cam *c;

	if (sig == SIGHUP) {
		if (load_config(a) == 0)
			spa_list_for_each(c, &a->cams, link) {
				cdn_trigger_init(&c->trig, &a->cfg.trig, a->cfg.mode);
				if (c->have_fmt)
					setup_backend(c);
			}
		return;
	}
	pw_main_loop_quit(a->loop);
}

static void usage(void)
{
	fprintf(stderr,
		"usage: aurora-camera-denoise [--mode auto|on|off] [--backend auto|cpu|NAME]\n"
		"                             [--boards FILE] [--verbose] [--print-config]\n");
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "mode", required_argument, NULL, 'm' },
		{ "backend", required_argument, NULL, 'b' },
		{ "boards", required_argument, NULL, 'B' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "print-config", no_argument, NULL, 'p' },
		{ "help", no_argument, NULL, 'h' },
		{ 0 },
	};
	struct app a = { .boards_path = CDN_DATADIR "/boards.conf" };
	bool print_only = false;
	int o;

	while ((o = getopt_long(argc, argv, "m:b:B:vph", opts, NULL)) != -1) {
		switch (o) {
		case 'm': a.cli_mode = optarg; break;
		case 'b': a.cli_backend = optarg; break;
		case 'B': a.boards_path = optarg; break;
		case 'v': a.verbose = true; break;
		case 'p': print_only = true; break;
		default: usage(); return o == 'h' ? 0 : 2;
		}
	}
	if (load_config(&a))
		return 1;
	if (print_only)
		return 0;

	pw_init(&argc, &argv);
	spa_list_init(&a.cams);
	a.loop = pw_main_loop_new(NULL);
	struct pw_loop *l = pw_main_loop_get_loop(a.loop);
	pw_loop_add_signal(l, SIGINT, on_signal, &a);
	pw_loop_add_signal(l, SIGTERM, on_signal, &a);
	pw_loop_add_signal(l, SIGHUP, on_signal, &a);
	a.context = pw_context_new(l, NULL, 0);
	a.core = pw_context_connect(a.context, NULL, 0);
	if (!a.core) {
		fprintf(stderr, "camera-denoise: cannot connect to PipeWire: %m\n");
		return 1;
	}
	pw_core_add_listener(a.core, &a.core_listener, &core_events, &a);
	a.registry = pw_core_get_registry(a.core, PW_VERSION_REGISTRY, 0);
	pw_registry_add_listener(a.registry, &a.registry_listener, &registry_events, &a);

	pw_main_loop_run(a.loop);

	struct cam *c, *t;
	spa_list_for_each_safe(c, t, &a.cams, link)
		cam_destroy(c);
	pw_proxy_destroy((struct pw_proxy *)a.registry);
	pw_core_disconnect(a.core);
	pw_context_destroy(a.context);
	pw_main_loop_destroy(a.loop);
	pw_deinit();
	return 0;
}
