/* SPDX-License-Identifier: MIT */
#include "../src/effects.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static pthread_t main_thread;
static _Atomic unsigned calls, resets;
struct state { unsigned value; size_t bytes; };
static void *create(const struct ce_config *c)
{
	assert(!pthread_equal(pthread_self(), main_thread));
	struct state *s = malloc(sizeof(*s));
	assert(s);
	*s = (struct state){ c->background, (size_t)c->width*c->height*3/2 };
	return s;
}
static int process(void *opaque, const uint8_t *in, uint8_t *out, uint64_t pts)
{
	struct state *s = opaque;
	assert(!pthread_equal(pthread_self(), main_thread));
	assert(pts > 0);
	memcpy(out, in, s->bytes);
	out[0] = (uint8_t)s->value;
	atomic_fetch_add(&calls, 1);
	return s->value == 2 ? -1 : 0; /* Simulate partial output before failure. */
}
static void reset(void *s) { (void)s; atomic_fetch_add(&resets, 1); }
static const struct ce_ops ops = {CE_ABI, create, process, reset, free};
static int take(struct cdn_effects *s, struct cdn_frame *f, uint64_t *pts, uint64_t *seq)
{
	struct timespec pause = {0,1000000};
	for (unsigned i=0; i<1000; i++) {
		if (cdn_effects_take(s,f,pts,seq)) return 1;
		nanosleep(&pause,NULL);
	}
	return 0;
}
int main(void)
{
	main_thread = pthread_self();
	struct ce_config cfg = {.abi=CE_ABI,.width=8,.height=4,.background=1};
	struct cdn_effects *a=cdn_effects_new(&cfg,&ops);
	cfg.background=2;
	struct cdn_effects *b=cdn_effects_new(&cfg,&ops);
	assert(a && b);
	uint8_t src[72],dst[72];
	memset(src,77,sizeof(src)); memset(dst,0xa5,sizeof(dst));
	struct cdn_frame in={src,src+48,8,4,12,12},out={dst,dst+48,8,4,12,12};
	uint64_t pts=0,seq=0;
	assert(cdn_effects_submit(a,&in,1000,123));
	assert(cdn_effects_submit(b,&in,2000,456));
	assert(take(a,&out,&pts,&seq));
	assert(pts==1000 && seq==123 && dst[0]==1 && dst[1]==77 && dst[48]==77);
	for (int row=0;row<6;row++) for (int col=8;col<12;col++) assert(dst[row*12+col]==0xa5);
	assert(take(b,&out,&pts,&seq));
	assert(pts==2000 && seq==456 && dst[0]==77); /* Failed frame is complete pass-through. */
	assert(cdn_effects_submit(a,&in,500,999)); /* Timestamp reversal resets crop history. */
	assert(take(a,&out,&pts,&seq));
	assert(pts==500 && seq==999 && atomic_load(&resets)>0);
	/* Saturating a slow consumer stays bounded, preserves complete frames and
	 * cannot reuse a ring slot still owned by the worker. */
	for (unsigned i=0;i<2000;i++) cdn_effects_submit(a,&in,100000+i,i);
	assert(take(a,&out,&pts,&seq));
	assert(dst[0]==1 && dst[1]==77 && dst[48]==77);
	cdn_effects_free(a); cdn_effects_free(b);
	assert(atomic_load(&calls)>=4);
	return 0;
}
