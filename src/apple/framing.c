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
 * moves under 0.2 of the head are "still". face_fov puts a head 0.15 of the
 * frame high (the usual distance from a laptop) at the default viewport.
 */
void apple_framing_params_default(struct apple_framing_params *p)
{
	*p = (struct apple_framing_params) {
		.default_fov = 0.68f, .min_fov = 0.34f,
		.deadband_w = 0.6f, .deadband_h = 0.3f,
		.group_deadband_w = 0.8f, .group_deadband_h = 0.5f,
		.top_offset = 0.15f, .group_width = 0.8f,
		.speed_x = 2.0f, .speed_y = 2.5f, .speed_zoom_in = 1.0f, .speed_zoom_out = 2.0f,
		.accel_x = 1.5f, .accel_y = 1.5f, .accel_zoom = 1.0f,
		.ease_x = 2.5f, .ease_y = 3.75f, .ease_zoom = 1.0f,
		.recenter_idle_s = 5.0f, .recenter_s = 1.5f,
		.no_subject_wait_s = 5.0f, .no_subject_return_s = 5.0f,
		.tiny_head = 0.015f, .movement_tolerance = 0.2f,
		.face_fov = 4.5f,
	};
}

/* Base rates the multipliers scale: viewport fractions per second (squared). */
#define SPEED_BASE 0.25f
#define ACCEL_BASE 1.0f
/* Smoothing of the per-frame subject box (seconds). */
#define SUBJECT_TAU 0.15f

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
static struct apple_viewport wanted(const struct apple_framing *f, const struct apple_subject *s)
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
		size = clampf(size, f->p.min_fov, 1.0f);
	}
	v.size = size;
	v.cx = s->people > 1 ? (s->x0 + s->x1) / 2 : s->head_x;
	v.cy = s->head_top + size * (0.5f - f->p.top_offset);
	clamp_viewport(&v);
	return v;
}

/* One axis: velocity toward k * error, capped at vmax, changed at most amax per s. */
static void axis(float *p, float *v, float target, float k, float vmax_up, float vmax_down,
		 float amax, float dt)
{
	float err = target - *p;
	float want = clampf(k * err, -vmax_down, vmax_up);
	float dv = clampf(want - *v, -amax * dt, amax * dt);

	*v += dv;
	/* do not overshoot the target within this step */
	if ((err > 0 && *v * dt > err) || (err < 0 && *v * dt < err)) {
		*p = target;
		*v = 0;
		return;
	}
	*p += *v * dt;
}

struct apple_viewport apple_framing_step(struct apple_framing *f,
					 const struct apple_subject *in, float dt)
{
	const struct apple_framing_params *p = &f->p;
	struct apple_subject s = in ? *in : (struct apple_subject) { 0 };
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
			f->smooth.head_h += a * (s.head_h - f->smooth.head_h);
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

		struct apple_viewport w = wanted(f, &f->smooth);
		bool group = f->smooth.people > 1;
		float half_w = (group ? p->group_deadband_w : p->deadband_w) * f->target.size / 2;
		float half_h = (group ? p->group_deadband_h : p->deadband_h) * f->target.size / 2;
		bool recenter = f->still_for >= p->recenter_idle_s;

		/* First sight: frame the subject. Then move only when it leaves the
		 * deadband, or recentre once it has been still for a while. */
		if (!f->started || fabsf(w.cx - f->target.cx) > half_w || recenter)
			f->target.cx = w.cx;
		if (!f->started || fabsf(w.cy - f->target.cy) > half_h || recenter)
			f->target.cy = w.cy;
		if (!f->started || fabsf(w.size - f->target.size) > 0.1f * f->target.size ||
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

	axis(&f->pos.cx, &f->vel.cx, f->target.cx, gain_x, SPEED_BASE * p->speed_x,
	     SPEED_BASE * p->speed_x, ACCEL_BASE * p->accel_x, dt);
	axis(&f->pos.cy, &f->vel.cy, f->target.cy, gain_y, SPEED_BASE * p->speed_y,
	     SPEED_BASE * p->speed_y, ACCEL_BASE * p->accel_y, dt);
	/* size up = zoom out (faster), size down = zoom in */
	axis(&f->pos.size, &f->vel.size, f->target.size, gain_z, SPEED_BASE * p->speed_zoom_out,
	     SPEED_BASE * p->speed_zoom_in, ACCEL_BASE * p->accel_zoom, dt);
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
