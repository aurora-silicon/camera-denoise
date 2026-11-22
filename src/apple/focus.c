// SPDX-License-Identifier: MIT
/* Portrait's focus from faces; see focus.h. */
#include <math.h>

#include "focus.h"

typedef _Float16 h16;

static const float offset_xy[3][2] = { { 0.935f, 0 }, { 1, 0.1f }, { 1, 0.2f } };
static const float floor_disp[3] = { 0, 0.5f, 0 };
static const float ema_first[3] = { 1, 1, 1 }, ema[3] = { 0.25f, 0.25f, 0.5f };

void focus_init(struct focus_state *f)
{
	*f = (struct focus_state) { .frames = 0, .last = 1e-4f, .effect = 1 };
}

/* Bilinear, clamp_to_edge, the filtered fp16 rounded half away from zero (the GPU's texture unit). */
static float tap(const float *img, int w, int h, float u, float v)
{
	float x = u * w - 0.5f, y = v * h - 0.5f;
	int x0 = (int)floorf(x), y0 = (int)floorf(y);
	float fx = x - x0, fy = y - y0;
	int xa = x0 < 0 ? 0 : x0 >= w ? w - 1 : x0, xb = x0 + 1 < 0 ? 0 : x0 + 1 >= w ? w - 1 : x0 + 1;
	int ya = y0 < 0 ? 0 : y0 >= h ? h - 1 : y0, yb = y0 + 1 < 0 ? 0 : y0 + 1 >= h ? h - 1 : y0 + 1;
	float a = img[ya * w + xa], b = img[ya * w + xb], c = img[yb * w + xa], d = img[yb * w + xb];
	float r = (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
	int e;

	if (r == 0)
		return 0;
	frexpf(fabsf(r), &e);
	float ulp = e - 1 < -14 ? ldexpf(1, -24) : ldexpf(1, e - 11);

	return copysignf(floorf(fabsf(r) / ulp + 0.5f) * ulp, r);
}

static float median3(float a, float b, float c)
{
	return fmaxf(fminf(a, b), fminf(fmaxf(a, b), c));
}

float focus_face_disparity(const float *disp, int dw, int dh, const struct focus_rect *r)
{
	float med[3];
	h16 cap = (h16)2.0f;            /* focusDisparityMax */

	for (int a = 0; a < 3; a++) {
		float s[3];

		for (int b = 0; b < 3; b++) {
			float u = (0.4f + 0.1f * a) * r->w + r->x, v = (0.4f + 0.1f * b) * r->h + r->y;
			h16 d = (h16)tap(disp, dw, dh, u, v);

			s[b] = (float)(d < cap ? d : cap);
		}
		med[a] = median3(s[0], s[1], s[2]);
	}
	return median3(med[0], med[1], med[2]);
}

void focus_update(struct focus_state *f, const float *face_disp, const int *state, int nfaces, float offsets[3])
{
	int live = 0;

	for (int i = 0; i < nfaces && i < 4; i++)
		live += state[i] != 3;
	if (nfaces <= 0 || !live) {
		/* no face: Portrait's default focus (offset -1.5 for every renderer) */
		for (int k = 0; k < 3; k++)
			offsets[k] = -1.5f;
		f->frames = 0;
		return;
	}
	if (nfaces > 4)
		nfaces = 4;
	/* the nearest face */
	float nearest = 1e-4f;
	int any = 0;

	for (int i = 0; i < nfaces; i++)
		if (state[i] != 3 && (!any || face_disp[i] > nearest)) {
			nearest = face_disp[i];
			any = 1;
		}
	/* the farthest face close to it */
	float inv = 1 / fmaxf(nearest, 1e-4f), inv_last = 1 / fmaxf(f->last, 1e-4f), focus = nearest;

	for (int i = 0; i < nfaces; i++) {
		float iv = 1 / fmaxf(face_disp[i], 1e-4f), a = iv - inv, b = iv - inv_last;

		if (state[i] == 3)
			continue;
		if ((b < 0.1f && a < 0.6f) || (a < 0.4f && state[i] == 0))
			focus = fminf(focus, face_disp[i]);
	}
	f->last = focus;
	for (int k = 0; k < 3; k++) {
		float w = f->frames ? ema[k] : ema_first[k];
		float d = f->frames ? f->disparity[k] + (focus - f->disparity[k]) * w : focus * w;

		f->disparity[k] = fmaxf(floor_disp[k], d);
		float fd = 1 / (1 / fmaxf(f->disparity[k], 1e-5f) + offset_xy[k][1]) * offset_xy[k][0];

		offsets[k] = -fd;
		if (k == FOCUS_STUDIO) {
			h16 t = (h16)fminf(1, fmaxf(0, (fd - 0.6f) / (0.6f - 0.5f)));
			h16 step = (h16)0.0111111f, dlt = (h16)((float)t - f->effect);

			dlt = dlt < -step ? -step : dlt > step ? step : dlt;
			f->effect = fminf(1, fmaxf(0, f->effect + (float)dlt));
		}
	}
	f->frames++;
}
