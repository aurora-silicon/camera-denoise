/* SPDX-License-Identifier: MIT */
#ifndef CDN_EFFECTS_H
#define CDN_EFFECTS_H
#include "backend.h"
#include "camera_effects.h"
struct cdn_effects;
/* Bounded SPSC input/output queues, owned frame storage and a non-RT worker.
 * new/free belong on the main loop. submit/take have one caller. A NULL ops
 * loads the optional model plugin. Injectable ops are for deterministic tests. */
struct cdn_effects *cdn_effects_new(const struct ce_config *, const struct ce_ops *);
void cdn_effects_free(struct cdn_effects *);
int cdn_effects_submit(struct cdn_effects *, const struct cdn_frame *, uint64_t pts, uint64_t seq);
/* Take the newest complete frame, retaining its original presentation time.
 * 0 means pending/late: caller drops this output, never uses a stale mask. */
int cdn_effects_take(struct cdn_effects *, struct cdn_frame *, uint64_t *pts, uint64_t *seq);
#endif
