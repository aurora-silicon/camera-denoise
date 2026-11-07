/* SPDX-License-Identifier: MIT */
/* Host test plugin, never installed by the project. Inject a long inference
 * after output has begun, then require the stream to recover. */
#include "../src/camera_effects.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
struct state { size_t bytes; unsigned calls; };
static void *create(const struct ce_config *c)
{
	struct state *s=calloc(1,sizeof(*s));
	if (s) s->bytes=(size_t)c->width*c->height*3/2;
	return s;
}
static int process(void *opaque,const uint8_t *in,uint8_t *out,uint64_t pts)
{
	struct state *s=opaque;(void)pts;
	if (++s->calls==6) { struct timespec pause={0,350000000};nanosleep(&pause,NULL); }
	memcpy(out,in,s->bytes);out[0]=1;
	return 0;
}
const struct ce_ops *camera_effects_entry(void)
{
	static const struct ce_ops ops={CE_ABI,create,process,NULL,free};return &ops;
}
