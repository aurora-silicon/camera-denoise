/* SPDX-License-Identifier: MIT */
/*
 * Denoise backend interface.
 *
 * A backend turns one NV12 frame into a denoised NV12 frame. It may keep
 * temporal state between calls; cdn_backend_reset() drops it (stream
 * restart, format change, or after a pass-through stretch).
 *
 * Backends live in this binary (the CPU one) or in shared objects loaded
 * from CDN_BACKEND_DIR, named "cdn-backend-<name>.so", exporting
 * CDN_BACKEND_ENTRY ("cdn_backend_entry"), which returns its ops table.
 * That is how the ANE backend (open model on the Neural Engine through the
 * shared ANE runtime) plugs in without this program linking the runtime.
 */
#ifndef CDN_BACKEND_H
#define CDN_BACKEND_H

#include <stdint.h>
#include <stddef.h>

/*
 * 2: struct cdn_frame_info gained tuned, temporal_frames and spatial_q8, which
 * plugins read; an ABI 1 caller's info is too short for them, so a plugin
 * and its caller must agree on 2.
 */
#define CDN_BACKEND_ABI 2
#define CDN_BACKEND_ENTRY "cdn_backend_entry"

/* One NV12 frame: a luma plane and an interleaved half-height CbCr plane. */
struct cdn_frame {
	uint8_t *y;
	uint8_t *uv;
	int width;
	int height;
	int stride_y;
	int stride_uv;
};

/* What the caller knows about the frame, for backends that adapt to it. */
struct cdn_frame_info {
	/* total sensor gain in 1/256 steps (256 = unity), 0 when unknown */
	uint32_t gain;
	/* exposure in microseconds, 0 when unknown */
	uint32_t exposure_us;
	/* presentation time in nanoseconds */
	uint64_t pts_ns;
	/* the user's tuning (denoise.conf); tuned = 0: unset, backend defaults */
	uint8_t tuned;
	/* longest running mean of the temporal filter, 2..8 frames (default 8) */
	uint8_t temporal_frames;
	/* spatial smoothing level in 1/256 (0 = off, 256 = default) */
	uint16_t spatial_q8;
};

/* The tuning in info, or the defaults when info is NULL or untuned. */
static inline int cdn_info_age_max(const struct cdn_frame_info *info)
{
	int f = info && info->tuned ? info->temporal_frames : 8;
	return (f < 2 ? 2 : f > 8 ? 8 : f) - 1;
}
static inline int cdn_info_spatial_q8(const struct cdn_frame_info *info)
{
	int v = info && info->tuned ? info->spatial_q8 : 256;
	return v > 256 ? 256 : v;
}

struct cdn_backend_ops {
	/* CDN_BACKEND_ABI */
	uint32_t abi;
	const char *name;
	/*
	 * Allocate state for frames of this size. threads is a hint (0 =
	 * backend default). Returns NULL when the backend cannot run here
	 * (missing device, unsupported size): the caller then falls back.
	 */
	void *(*create)(int width, int height, int threads);
	/*
	 * Denoise in into out (same geometry, distinct buffers). strength is
	 * 0..1 and blends the result with the input; 0 still updates the
	 * temporal state, so a ramp can continue smoothly. Returns 0 or a
	 * negative errno; on error the caller passes the frame through.
	 */
	int (*process)(void *state, const struct cdn_frame *in,
		       struct cdn_frame *out, const struct cdn_frame_info *info,
		       float strength);
	/* Forget temporal history. */
	void (*reset)(void *state);
	/* Estimated luma noise of the last frame, in 8-bit code values. */
	float (*noise_sigma)(void *state);
	void (*destroy)(void *state);
};

typedef const struct cdn_backend_ops *(*cdn_backend_entry_fn)(void);

/* Optional companion symbol, versioned with CDN_DMABUF_ABI (its process takes
 * the same struct cdn_frame_info, so it moved to 2 with CDN_BACKEND_ABI).
 * Borrowed, linear NV12 DMA-BUFs; offsets are from the start of each fd,
 * including the PipeWire mapoffset and chunk offset. The caller owns the
 * buffers until this synchronous call returns. No explicit-sync streams.
 */
#define CDN_DMABUF_ENTRY "cdn_backend_dmabuf_entry"
#define CDN_DMABUF_ABI 2
#define CDN_BACKEND_SINGLE_UPLOAD 1u
struct cdn_dmabuf_plane {
	int fd;
	uint32_t offset, stride;
};
struct cdn_dmabuf {
	int width, height;
	struct cdn_dmabuf_plane y, uv;
};
struct cdn_backend_dmabuf_ops {
	uint32_t abi, flags;
	/* -ENOTSUP means retry the ordinary process path, without resetting
	 * history. -EAGAIN means the producer fence is not ready: drop the frame
	 * without reading it. Other errors permit CPU fallback. */
	int (*process)(void *state, const struct cdn_dmabuf *in,
		       struct cdn_frame *out, const struct cdn_frame_info *info,
		       float strength);
};
typedef const struct cdn_backend_dmabuf_ops *(*cdn_backend_dmabuf_entry_fn)(void);
const struct cdn_backend_dmabuf_ops *cdn_backend_dmabuf(const struct cdn_backend_ops *ops);

/* Optional companion symbol: answers questions about one instance.
 * Returns 1 for yes, 0 for no, negative for unknown questions. */
#define CDN_QUERY_ENTRY "cdn_backend_query"
/* Yes when this instance runs the backend's full hardware path, so
 * temporal-backend = auto may keep it instead of the CPU. */
#define CDN_QUERY_AUTO 1u
typedef int (*cdn_backend_query_fn)(void *state, uint32_t what);
/* The plugin's answer, or 0 when it has no query symbol (and for the CPU). */
int cdn_backend_ask(const struct cdn_backend_ops *ops, void *state, uint32_t what);

/* The built-in CPU backend (temporal + spatial, NEON on arm64). */
const struct cdn_backend_ops *cdn_cpu_backend(void);

/*
 * Find a backend by name: "cpu", or a plugin name. "auto" tries the
 * plugins listed in order (comma separated in prefer, e.g. "ane") and falls
 * back to "cpu". Returns NULL if nothing matches.
 */
const struct cdn_backend_ops *cdn_backend_find(const char *name,
					       const char *prefer);

/* Copy an NV12 frame (pass-through). */
void cdn_frame_copy(const struct cdn_frame *in, struct cdn_frame *out);

#endif
