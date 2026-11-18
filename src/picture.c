// SPDX-License-Identifier: MIT
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "picture.h"

#define GAIN_ONE 4096 /* Q12 */

struct cdn_picture {
	int w, h;
	float vignette;
	uint16_t *gy;  /* luma gain map, w * h, Q12 */
	uint16_t *guv; /* chroma gain map, (w / 2) * (h / 2), Q12 */
	struct cdn_picture_cfg lut_cfg;
	bool lut_valid;
	uint8_t ylut[256];
	int16_t sat_q8;       /* saturation, Q8 */
	int16_t u_off, v_off; /* warmth / tint, chroma levels */
	uint8_t *rows;        /* sharpen: 3 luma rows before the pass */
	int rows_w;
};

void cdn_picture_defaults(struct cdn_picture_cfg *p)
{
	*p = (struct cdn_picture_cfg){ .contrast = 1.0f, .saturation = 1.0f };
}

bool cdn_picture_neutral(const struct cdn_picture_cfg *p)
{
	return p->vignette == 0 && p->brightness == 0 && p->contrast == 1 && p->saturation == 1 &&
	       p->warmth == 0 && p->tint == 0 && p->sharpen == 0;
}

struct cdn_picture *cdn_picture_create(void)
{
	return calloc(1, sizeof(struct cdn_picture));
}

void cdn_picture_destroy(struct cdn_picture *s)
{
	if (!s)
		return;
	free(s->gy);
	free(s->guv);
	free(s->rows);
	free(s);
}

static inline uint8_t clamp8(int v)
{
	return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v;
}

/* Gain 1 + vignette * r^2, r = 1 in the corners: a cos^4 falloff to first order. */
static int build_gain(struct cdn_picture *s, int w, int h, float vignette)
{
	int cw = w / 2, ch = h / 2;
	uint16_t *gy = malloc((size_t)w * h * sizeof(*gy));
	uint16_t *guv = malloc((size_t)(cw ? cw : 1) * (ch ? ch : 1) * sizeof(*guv));
	float cx = (w - 1) * 0.5f, cy = (h - 1) * 0.5f, norm = 1.0f / (cx * cx + cy * cy + 1e-6f);

	if (!gy || !guv) {
		free(gy);
		free(guv);
		return -ENOMEM;
	}
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			float dx = x - cx, dy = y - cy;
			gy[(size_t)y * w + x] = (uint16_t)lrintf(GAIN_ONE * (1.0f + vignette * (dx * dx + dy * dy) * norm));
		}
	for (int y = 0; y < ch; y++)
		for (int x = 0; x < cw; x++) {
			float dx = 2 * x + 0.5f - cx, dy = 2 * y + 0.5f - cy;
			guv[(size_t)y * cw + x] = (uint16_t)lrintf(GAIN_ONE * (1.0f + vignette * (dx * dx + dy * dy) * norm));
		}
	free(s->gy);
	free(s->guv);
	s->gy = gy;
	s->guv = guv;
	s->w = w;
	s->h = h;
	s->vignette = vignette;
	return 0;
}

static void build_lut(struct cdn_picture *s, const struct cdn_picture_cfg *p)
{
	for (int i = 0; i < 256; i++)
		s->ylut[i] = clamp8((int)lrintf(128.0f + (i - 128) * p->contrast + p->brightness * 64.0f));
	s->sat_q8 = (int16_t)lrintf(p->saturation * 256.0f);
	/* warmer = more red (Cr), less blue (Cb); magenta = more of both */
	s->u_off = (int16_t)lrintf(-p->warmth * 10.0f + p->tint * 8.0f);
	s->v_off = (int16_t)lrintf(p->warmth * 10.0f + p->tint * 8.0f);
	s->lut_cfg = *p;
	s->lut_valid = true;
}

/* Unsharp mask with a 3x3 box, on the luma as the earlier passes left it. */
static int sharpen(struct cdn_picture *s, struct cdn_frame *f, float amount)
{
	int w = f->width, h = f->height, k = (int)lrintf(amount * 1.5f * 256.0f);
	uint8_t *r[3];

	if (h < 3 || w < 3)
		return 0;
	if (s->rows_w < w) {
		free(s->rows);
		s->rows = malloc((size_t)3 * w);
		s->rows_w = s->rows ? w : 0;
		if (!s->rows)
			return -ENOMEM;
	}
	r[0] = s->rows;
	r[1] = s->rows + w;
	r[2] = s->rows + 2 * (size_t)w;
	memcpy(r[0], f->y, w);
	memcpy(r[1], f->y + f->stride_y, w);
	for (int y = 1; y < h - 1; y++) {
		uint8_t *above = r[(y - 1) % 3], *mid = r[y % 3], *below = r[(y + 1) % 3];
		uint8_t *out = f->y + (size_t)y * f->stride_y;

		memcpy(below, f->y + (size_t)(y + 1) * f->stride_y, w);
		for (int x = 1; x < w - 1; x++) {
			int sum = above[x - 1] + above[x] + above[x + 1] + mid[x - 1] + mid[x] + mid[x + 1] +
				  below[x - 1] + below[x] + below[x + 1];
			int detail = mid[x] * 9 - sum; /* 9 * (pixel - mean) */
			out[x] = clamp8(mid[x] + (detail * k) / (9 * 256));
		}
	}
	return 0;
}

int cdn_picture_apply(struct cdn_picture *s, const struct cdn_picture_cfg *p, struct cdn_frame *f)
{
	int w = f->width, h = f->height, cw = w / 2, ch = h / 2;
	bool vig = p->vignette > 0, lut, chroma;

	if (cdn_picture_neutral(p))
		return 0;
	if (vig && (s->w != w || s->h != h || s->vignette != p->vignette || !s->gy) &&
	    build_gain(s, w, h, p->vignette))
		return -ENOMEM;
	if (!s->lut_valid || memcmp(&s->lut_cfg, p, sizeof(*p)))
		build_lut(s, p);
	lut = p->brightness != 0 || p->contrast != 1;
	chroma = vig || p->saturation != 1 || p->warmth != 0 || p->tint != 0;

	if (vig || lut)
		for (int y = 0; y < h; y++) {
			uint8_t *row = f->y + (size_t)y * f->stride_y;
			const uint16_t *g = vig ? s->gy + (size_t)y * w : NULL;

			for (int x = 0; x < w; x++) {
				int v = row[x];

				if (g)
					v = clamp8(16 + (((v - 16) * g[x]) >> 12));
				row[x] = s->ylut[v];
			}
		}
	if (chroma)
		for (int y = 0; y < ch; y++) {
			uint8_t *row = f->uv + (size_t)y * f->stride_uv;
			const uint16_t *g = vig ? s->guv + (size_t)y * cw : NULL;

			for (int x = 0; x < cw; x++) {
				int u = row[2 * x] - 128, v = row[2 * x + 1] - 128;

				if (g) {
					u = (u * g[x]) >> 12;
					v = (v * g[x]) >> 12;
				}
				row[2 * x] = clamp8(128 + ((u * s->sat_q8) >> 8) + s->u_off);
				row[2 * x + 1] = clamp8(128 + ((v * s->sat_q8) >> 8) + s->v_off);
			}
		}
	if (p->sharpen > 0)
		return sharpen(s, f, p->sharpen);
	return 0;
}
