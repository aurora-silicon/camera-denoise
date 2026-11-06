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

#define CDN_BACKEND_ABI 1
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
};

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

/* Optional ABI v1 companion symbol; the original ops/frame layouts stay intact.
 * Borrowed, linear NV12 DMA-BUFs; offsets are from the start of each fd,
 * including the PipeWire mapoffset and chunk offset. The caller owns the
 * buffers until this synchronous call returns. No explicit-sync streams.
 */
#define CDN_DMABUF_ENTRY "cdn_backend_dmabuf_entry"
#define CDN_DMABUF_ABI 1
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
