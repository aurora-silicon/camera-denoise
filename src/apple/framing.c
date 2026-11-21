// SPDX-License-Identifier: MIT
#include "framing.h"

#include <math.h>
#include <string.h>

/*
 * CinematicFraming.framework/Resources/cinematicFramingParameters.plist as
 * shipped for the MacBook Air M5 (J815, identical on J813): viewport default
 * 0.68 and minimum 0.34; one subject's deadband 0.6 x 0.3 of the viewport, a
 * group's 0.8 x 0.5; head top 0.15 below the viewport top; speed multipliers
 * 2 (horizontal), 2.5 (vertical), 1 (zoom in), 2 (zoom out); acceleration
 * multipliers 1.5, 1.5, 1; ease-in 2.5 / 3.75 / 1; recentring after 5 s still
 * over 1.5 s; nobody for 5 s: home over 5 s; heads under 1.5 % are ignored;
 * moves under 0.2 of the head are "still".
 *
 * The plist's viewport is a share of the sensor's width. The Linux frame is
 * already a crop of it: the lens-circle crop takes 1242 of the 1520 binned
 * columns (0.82). So Apple's default 0.68 is 0.83 of our frame and its
 * minimum 0.34 is 0.42 (2026-11-21 round 3: with 0.68/0.5 used as is the
 * viewport sat at 0.5, a 2x zoom, "still zooming too much").
 *
 * Ours, not Apple's: face_fov and max_fov. A head at laptop distance is
 * 0.15-0.20 of our frame high (the 2026-11-21 run); face_fov 4.4 puts a 0.19
 * head at the default.
 * One person is never framed wider than max_fov, so there is always room to
 * follow them (2026-11-21: with no cap the viewport grew to the whole frame
 * and Center Stage stopped). min_fov stays above Apple's 0.42: one person
 * at a laptop is not framed tighter than 1.4x.
 */
void apple_framing_params_default(struct apple_framing_params *p)
{
	*p = (struct apple_framing_params) {
		.default_fov = 0.83f, .min_fov = 0.7f,
		.deadband_w = 0.6f, .deadband_h = 0.3f,
		.group_deadband_w = 0.8f, .group_deadband_h = 0.5f,
		.top_offset = 0.15f, .group_width = 0.8f,
		.speed_x = 2.0f, .speed_y = 2.5f, .speed_zoom_in = 1.0f, .speed_zoom_out = 2.0f,
		.accel_x = 1.5f, .accel_y = 1.5f, .accel_zoom = 1.0f,
		.ease_x = 2.5f, .ease_y = 3.75f, .ease_zoom = 1.0f,
		.recenter_idle_s = 5.0f, .recenter_s = 1.5f,
		.no_subject_wait_s = 5.0f, .no_subject_return_s = 5.0f,
		.tiny_head = 0.015f, .movement_tolerance = 0.2f,
		.face_fov = 4.4f, .max_fov = 0.86f,
	};
}

/* Base rates the multipliers scale: viewport fractions per second (squared). */
#define SPEED_BASE 0.25f
#define ACCEL_BASE 1.0f
/* Smoothing of the per-frame subject box (seconds); the head size, which
 * sets the zoom, more (a size jitter shows as pumping). */
#define SUBJECT_TAU 0.35f
#define SIZE_TAU 0.8f
/* the target size changes only when the wanted one differs by more than this */
#define SIZE_HYSTERESIS 0.15f

static float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static struct apple_viewport home(const struct apple_framing *f)
{
	float size = f->zoom > 0 ? 1.0f / f->zoom : f->p.default_fov;

	return (struct apple_viewport) { 0.5f, 0.5f, clampf(size, 0.05f, 1.0f) };
}

static void clamp_viewport(struct apple_viewport *v)
{
	float h = v->size / 2;

	v->size = clampf(v->size, 0.05f, 1.0f);
	h = v->size / 2;
	v->cx = clampf(v->cx, h, 1 - h);
	v->cy = clampf(v->cy, h, 1 - h);
}

void apple_framing_init(struct apple_framing *f, const struct apple_framing_params *p,
			float zoom)
{
	memset(f, 0, sizeof(*f));
	if (p)
		f->p = *p;
	else
		apple_framing_params_default(&f->p);
	f->zoom = zoom > 0 ? zoom : 0;
	apple_framing_reset(f);
}

void apple_framing_reset(struct apple_framing *f)
{
	f->pos = f->target = home(f);
	f->vel = (struct apple_viewport) { 0 };
	f->have_subject = false;
	f->started = false;
	f->since_subject = f->still_for = f->since_move = f->returning_for = 0;
}

/* The viewport the subject asks for: head near the top, or the group framed. */
static struct apple_viewport wanted(const struct apple_framing *f, const struct apple_subject *s,
				    struct apple_viewport *raw)
{
	struct apple_viewport v;
	float size;

	if (f->zoom > 0) {
		size = 1.0f / f->zoom;
	} else {
		size = s->head_h * f->p.face_fov;
		if (s->people > 1)
			size = fmaxf(size, (s->x1 - s->x0) / f->p.group_width);
		/* never cut the head: it and the offset above it fit */
		size = fmaxf(size, s->head_h / (1.0f - f->p.top_offset - 0.2f));
		size = clampf(size, f->p.min_fov, s->people > 1 ? 1.0f : f->p.max_fov);
	}
	v.size = size;
	v.cx = s->people > 1 ? (s->x0 + s->x1) / 2 : s->head_x;
	v.cy = s->head_top + size * (0.5f - f->p.top_offset);
	/* where the subject is, before the frame's edges stop the viewport */
	*raw = v;
	clamp_viewport(&v);
	return v;
}

/*
 * One axis: a critically damped spring towards the target (ease in, ease
 * out, no overshoot), its speed capped at vmax. smooth is the time it takes
 * to cover most of a step; the acceleration it asks for stays within amax.
 * The old controller stopped dead when it reached the target, which showed
 * as a jerk at the end of every move (2026-11-21).
 */
static void axis(float *p, float *v, float target, float smooth, float vmax, float amax, float dt)
{
	float omega = 2.0f / fmaxf(smooth, 1e-3f);
	float x = omega * dt;
	float decay = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
	float change = clampf(*p - target, -vmax * smooth, vmax * smooth);
	float goal = *p - change;
	float temp = (*v + omega * change) * dt;
	float nv = (*v - omega * temp) * decay;
	float np = goal + (change + temp) * decay;

	/* keep the acceleration within amax (a sudden new target eases in) */
	nv = clampf(nv, *v - amax * dt, *v + amax * dt);
	nv = clampf(nv, -vmax, vmax);
	np = *p + nv * dt;
	/* no overshoot; and settle once within ~1 px at rest, or the crop keeps
	 * creeping by 2-pixel steps for seconds at the tail of the spring */
	if ((target - *p > 0) == (np > target) ||
	    (fabsf(target - np) < 0.0015f && fabsf(nv) < 0.01f)) {
		np = target;
		nv = 0;
	}
	*p = np;
	*v = nv;
}

struct apple_viewport apple_framing_step(struct apple_framing *f,
					 const struct apple_subject *in, float dt)
{
	const struct apple_framing_params *p = &f->p;
	struct apple_subject s = in ? *in : (struct apple_subject) { 0 };
	/* ease_* are approach rates (1/s): a spring of smoothing time 1/rate */
	float gain_x = p->ease_x, gain_y = p->ease_y, gain_z = p->ease_zoom;

	if (!(dt > 0) || dt > 1.0f)
		dt = 1.0f / 30;
	if (s.present && s.head_h < p->tiny_head)
		s.present = false;

	if (s.present) {
		float a = 1.0f - expf(-dt / SUBJECT_TAU);
		float moved;

		if (!f->have_subject) {
			f->smooth = s;
			moved = 1e9f;
		} else {
			moved = fmaxf(fabsf(s.head_x - f->smooth.head_x),
				      fabsf(s.head_top - f->smooth.head_top));
			f->smooth.x0 += a * (s.x0 - f->smooth.x0);
			f->smooth.y0 += a * (s.y0 - f->smooth.y0);
			f->smooth.x1 += a * (s.x1 - f->smooth.x1);
			f->smooth.y1 += a * (s.y1 - f->smooth.y1);
			f->smooth.head_x += a * (s.head_x - f->smooth.head_x);
			f->smooth.head_top += a * (s.head_top - f->smooth.head_top);
			f->smooth.head_h += (1.0f - expf(-dt / SIZE_TAU)) * (s.head_h - f->smooth.head_h);
			f->smooth.people = s.people;
			f->smooth.present = true;
		}
		f->have_subject = true;
		f->since_subject = 0;
		f->returning_for = 0;
		if (moved < p->movement_tolerance * f->smooth.head_h * dt * 30)
			f->still_for += dt;
		else
			f->still_for = 0;

		struct apple_viewport raw, w = wanted(f, &f->smooth, &raw);
		bool group = f->smooth.people > 1;
		float half_w = (group ? p->group_deadband_w : p->deadband_w) * f->target.size / 2;
		float half_h = (group ? p->group_deadband_h : p->deadband_h) * f->target.size / 2;
		bool recenter = f->still_for >= p->recenter_idle_s;

		/* First sight: frame the subject. Then move only when it leaves the
		 * deadband, or recentre once it has been still for a while. The
		 * deadband is tested on where the subject is, not on the clamped
		 * viewport: near an edge the clamped centre moves less than the
		 * subject and never left the deadband (2026-11-21). */
		if (!f->started || fabsf(raw.cx - f->target.cx) > half_w || recenter)
			f->target.cx = w.cx;
		if (!f->started || fabsf(raw.cy - f->target.cy) > half_h || recenter)
			f->target.cy = w.cy;
		if (!f->started || fabsf(w.size - f->target.size) > SIZE_HYSTERESIS * f->target.size ||
		    recenter || f->zoom > 0)
			f->target.size = w.size;
		if (recenter) {
			float k = 3.0f / p->recenter_s;

			gain_x = fmaxf(gain_x, k);
			gain_y = fmaxf(gain_y, k);
			f->still_for = 0;
		}
		f->started = true;
	} else {
		f->since_subject += dt;
		if (f->since_subject >= p->no_subject_wait_s) {
			float k = 3.0f / p->no_subject_return_s;

			f->target = home(f);
			gain_x = gain_y = gain_z = k;
			f->have_subject = false;
			f->still_for = 0;
		}
	}
	clamp_viewport(&f->target);

	axis(&f->pos.cx, &f->vel.cx, f->target.cx, 1.0f / gain_x, SPEED_BASE * p->speed_x,
	     ACCEL_BASE * p->accel_x, dt);
	axis(&f->pos.cy, &f->vel.cy, f->target.cy, 1.0f / gain_y, SPEED_BASE * p->speed_y,
	     ACCEL_BASE * p->accel_y, dt);
	/* size up = zoom out (faster), size down = zoom in */
	axis(&f->pos.size, &f->vel.size, f->target.size, 1.0f / gain_z,
	     SPEED_BASE * (f->target.size > f->pos.size ? p->speed_zoom_out : p->speed_zoom_in),
	     ACCEL_BASE * p->accel_zoom, dt);
	clamp_viewport(&f->pos);
	return f->pos;
}

void apple_framing_crop(const struct apple_viewport *v, int width, int height,
			int *x, int *y, int *w, int *h)
{
	int cw = (int)lroundf(v->size * width) & ~1, ch = (int)lroundf(v->size * height) & ~1;

	cw = cw < 64 ? 64 : cw > (width & ~1) ? (width & ~1) : cw;
	ch = ch < 64 ? 64 : ch > (height & ~1) ? (height & ~1) : ch;
	int cx = (int)lroundf(v->cx * width - cw / 2.0f), cy = (int)lroundf(v->cy * height - ch / 2.0f);

	cx = cx < 0 ? 0 : cx > width - cw ? width - cw : cx;
	cy = cy < 0 ? 0 : cy > height - ch ? height - ch : cy;
	*x = cx & ~1;
	*y = cy & ~1;
	*w = cw;
	*h = ch;
}
