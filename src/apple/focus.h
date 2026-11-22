/* SPDX-License-Identifier: MIT */
#ifndef CDN_APPLE_FOCUS_H
#define CDN_APPLE_FOCUS_H

/*
 * Portrait's and Studio Light's focus from faces, in plain C.
 * Each face's disparity is the median of the row medians of a 3x3 grid over the rect's middle (40..60% of
 * its width and height) of the disparity x 1.94097, capped at 2. The focus is the nearest face, then the
 * farthest face still close to it in inverse depth (1/d within 0.6 of the nearest's and 0.1 of last
 * frame's focus, or within 0.4 of the nearest's for a face in state 0); it is smoothed per renderer (weights 1 on the first frame,
 * then 0.25 / 0.25 / 0.5) and turned into the renderers' focus offsets: -x / (1 / focus + y), with
 * (x, y) = (0.935, 0) for the blur, (1, 0.1), (1, 0.2) for Studio Light. With no face the offsets stay
 * -1.5 (focus 0.77282). Studio Light also fades its effect out (0.0111 per frame) when the focus disparity
 * goes under 0.5..0.6 (the person far away).
 */
struct focus_rect {
	float x, y, w, h;               /* top-left origin, normalised */
};

struct focus_state {
	int frames;                     /* renders with a face so far */
	float last;                     /* last frame's focus disparity */
	float disparity[3];             /* smoothed, per renderer */
	float effect;                   /* Studio Light's effect distance modifier (0..1) */
};

enum { FOCUS_BLUR = 0, FOCUS_MID = 1, FOCUS_STUDIO = 2 };

void focus_init(struct focus_state *f);
/* disp: dw x dh disparity x 1.94097 (the scaled network output). */
float focus_face_disparity(const float *disp, int dw, int dh, const struct focus_rect *r);
/*
 * face_disp: per face (focus_face_disparity); state: the per-face state (0 for a tracked
 * face, the only one seen so far, every frame; 2 keeps last frame's disparity; 3 = no face in that slot; 1
 * is sampled like 0 but not let into the 0.4 window). offsets[3] out.
 */
void focus_update(struct focus_state *f, const float *face_disp, const int *state, int nfaces, float offsets[3]);

#endif
