/* SPDX-License-Identifier: MIT */
#ifndef AURORA_CAMERA_EFFECTS_H
#define AURORA_CAMERA_EFFECTS_H
#include <stdint.h>
#define CE_ABI 1u
#define CE_ENTRY "camera_effects_entry"
/* All calls run on a non-RT worker, sequentially per instance. No PipeWire
 * buffers/pointers escape a call. NV12 input/output are tightly packed. */
struct ce_config {
    uint32_t abi, width, height;
    uint32_t background; /* 0 off, 1 blur, 2 replace */
    uint32_t framing;    /* explicit user choice; caller checks board support */
    float zoom_min, zoom_max, zoom_default, zoom; /* zoom 0 = automatic */
    uint32_t matrix;     /* 601 or 709 */
    uint32_t full_range;
    char replacement[1024];
};
struct ce_ops {
    uint32_t abi;
    void *(*create)(const struct ce_config *);
    int (*process)(void *, const uint8_t *, uint8_t *, uint64_t pts_ns);
    void (*reset)(void *);
    void (*destroy)(void *);
};
const struct ce_ops *camera_effects_entry(void);
#endif
