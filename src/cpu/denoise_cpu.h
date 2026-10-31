/* SPDX-License-Identifier: MIT */
#ifndef CDN_DENOISE_CPU_H
#define CDN_DENOISE_CPU_H

#include <stdbool.h>
#include "../backend.h"

void *cdn_cpu_create(int width, int height, int threads);
void cdn_cpu_destroy(void *state);
/* process with the NEON path on or off (tests compare the two) */
int cdn_cpu_process_ex(void *state, const struct cdn_frame *in,
		       struct cdn_frame *out, float strength, bool neon);

#endif
