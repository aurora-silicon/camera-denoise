/* SPDX-License-Identifier: MIT */
#include "effects.h"
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef CDN_BACKEND_DIR
#define CDN_BACKEND_DIR "/usr/lib/aurora-camera-denoise"
#endif
#define QUEUE 4u
struct slot { uint8_t *data; uint64_t pts, seq, token, enqueued; };
struct queue { _Atomic unsigned read, write; struct slot slots[QUEUE]; };
struct cdn_effects {
	struct ce_config cfg;
	const struct ce_ops *ops;
	size_t bytes;
	struct queue input, output;
	pthread_t worker;
	sem_t wake;
	_Atomic unsigned stop;
	uint64_t token;
};
static uint64_t now_ns(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
static const struct ce_ops *load(void)
{
	static void *library;
	static const struct ce_ops *ops;
	const struct ce_ops *(*entry)(void);
	/* Called only on the main loop; keep the Python interpreter DSO resident. */
	if (ops) return ops;
	if (!library) library = dlopen(CDN_BACKEND_DIR "/libaurora-camera-effects.so", RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE);
	if (!library) { fprintf(stderr, "camera-effects: %s\n", dlerror()); return NULL; }
	*(void **)(&entry) = dlsym(library, CE_ENTRY);
	ops = entry ? entry() : NULL;
	if (!ops || ops->abi != CE_ABI || !ops->create || !ops->process || !ops->destroy) {
		fprintf(stderr, "camera-effects: incompatible plugin\n");
		ops = NULL;
	}
	return ops;
}
static struct cdn_frame frame(struct cdn_effects *s, uint8_t *data)
{
	return (struct cdn_frame) { data, data+(size_t)s->cfg.width*s->cfg.height,
		(int)s->cfg.width, (int)s->cfg.height, (int)s->cfg.width, (int)s->cfg.width };
}
static void *work(void *opaque)
{
	struct cdn_effects *s = opaque;
	void *state = s->ops->create(&s->cfg);
	uint64_t expected = 0, last_pts = 0;
	int reported = 0;
	if (!state) fprintf(stderr, "camera-effects: model initialization failed; frames pass through\n");
	while (!atomic_load_explicit(&s->stop, memory_order_acquire)) {
		while (sem_wait(&s->wake) && errno == EINTR) {}
		if (atomic_load_explicit(&s->stop, memory_order_acquire)) break;
		unsigned r = atomic_load_explicit(&s->input.read, memory_order_relaxed);
		unsigned w = atomic_load_explicit(&s->input.write, memory_order_acquire);
		if (r == w) continue;
		struct slot *in = &s->input.slots[r % QUEUE];
		unsigned ow = atomic_load_explicit(&s->output.write, memory_order_relaxed);
		unsigned or = atomic_load_explicit(&s->output.read, memory_order_acquire);
		if (ow - or < QUEUE) {
			struct slot *out = &s->output.slots[ow % QUEUE];
			if (state && s->ops->reset && (in->token != expected ||
				(last_pts && (in->pts <= last_pts || in->pts-last_pts > 1000000000u))))
				s->ops->reset(state);
			int rc = state ? s->ops->process(state, in->data, out->data, in->pts) : -EIO;
			if (rc) {
				memcpy(out->data, in->data, s->bytes);
				if (!reported++) fprintf(stderr, "camera-effects: inference failed (%d); frame passed through\n", rc);
			}
			out->pts = in->pts; out->seq = in->seq; out->enqueued = in->enqueued;
			expected = in->token + 1; last_pts = in->pts;
			atomic_store_explicit(&s->output.write, ow + 1, memory_order_release);
		}
		atomic_store_explicit(&s->input.read, r + 1, memory_order_release);
	}
	if (state) s->ops->destroy(state);
	return NULL;
}
struct cdn_effects *cdn_effects_new(const struct ce_config *cfg, const struct ce_ops *ops)
{
	if (!cfg || cfg->abi != CE_ABI || cfg->width < 2 || cfg->height < 2 ||
		cfg->width > 4096 || cfg->height > 2160 || ((cfg->width | cfg->height)&1)) return NULL;
	if (!ops) ops = load();
	if (!ops || ops->abi != CE_ABI || !ops->create || !ops->process || !ops->destroy) return NULL;
	struct cdn_effects *s = calloc(1, sizeof(*s));
	if (!s) return NULL;
	s->cfg = *cfg; s->ops = ops;
	s->bytes = (size_t)cfg->width * cfg->height * 3 / 2;
	atomic_init(&s->stop, 0);
	atomic_init(&s->input.read, 0); atomic_init(&s->input.write, 0);
	atomic_init(&s->output.read, 0); atomic_init(&s->output.write, 0);
	if (!atomic_is_lock_free(&s->input.write)) goto fail;
	for (unsigned i = 0; i < QUEUE; i++) {
		s->input.slots[i].data = malloc(s->bytes);
		s->output.slots[i].data = malloc(s->bytes);
		if (!s->input.slots[i].data || !s->output.slots[i].data) goto fail;
	}
	if (sem_init(&s->wake, 0, 0)) goto fail;
	if (pthread_create(&s->worker, NULL, work, s)) { sem_destroy(&s->wake); goto fail; }
	return s;
fail:
	for (unsigned i = 0; i < QUEUE; i++) { free(s->input.slots[i].data); free(s->output.slots[i].data); }
	free(s);
	return NULL;
}
void cdn_effects_free(struct cdn_effects *s)
{
	if (!s) return;
	atomic_store_explicit(&s->stop, 1, memory_order_release);
	sem_post(&s->wake);
	pthread_join(s->worker, NULL);
	sem_destroy(&s->wake);
	for (unsigned i = 0; i < QUEUE; i++) { free(s->input.slots[i].data); free(s->output.slots[i].data); }
	free(s);
}
int cdn_effects_submit(struct cdn_effects *s, const struct cdn_frame *in, uint64_t pts, uint64_t seq)
{
	unsigned w = atomic_load_explicit(&s->input.write, memory_order_relaxed);
	unsigned r = atomic_load_explicit(&s->input.read, memory_order_acquire);
	uint64_t token = s->token++;
	if (w-r == QUEUE) return 0;
	struct slot *out = &s->input.slots[w % QUEUE];
	struct cdn_frame packed = frame(s, out->data);
	cdn_frame_copy(in, &packed);
	out->pts = pts; out->seq = seq; out->token = token; out->enqueued = now_ns();
	atomic_store_explicit(&s->input.write, w+1, memory_order_release);
	sem_post(&s->wake);
	return 1;
}
int cdn_effects_take(struct cdn_effects *s, struct cdn_frame *out, uint64_t *pts, uint64_t *seq)
{
	unsigned r = atomic_load_explicit(&s->output.read, memory_order_relaxed);
	unsigned w = atomic_load_explicit(&s->output.write, memory_order_acquire);
	if (r == w) return 0;
	struct slot *in = &s->output.slots[(w-1) % QUEUE];
	int fresh = now_ns()-in->enqueued <= 200000000u;
	if (fresh) {
		struct cdn_frame packed = frame(s, in->data);
		cdn_frame_copy(&packed, out);
		*pts = in->pts; *seq = in->seq;
	}
	atomic_store_explicit(&s->output.read, w, memory_order_release);
	return fresh;
}
