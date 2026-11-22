/* SPDX-License-Identifier: MIT */
/*
 * Center Stage framing: a session of face Kalman filter -> framing-space warp -> tracker -> director ->
 * scene framing engine -> rectangle animator, checked against traces of Center Stage on macOS (identical
 * states, positions within ~1e-5 rad).  The tuning is read at run time from the user's install
 * (cf_tuning_read).
 *
 * Framing space is angles in radians around the optical axis: x = atan((u - cx) / fx),
 * y = atan((v - cy) / fy), y grows downwards.  A viewport is (x, y, w, h) in that space, h = H(w) for the
 * output aspect.  The output image is a virtual pinhole camera rotated to the viewport centre
 * (cf_viewport_homography).
 *
 * Build with -ffp-contract=off: results are float-order sensitive.
 */
#ifndef CF_FRAMING_H
#define CF_FRAMING_H

#include <stddef.h>
#include <stdint.h>

#define CF_MAX_FACES 8       /* per kind (faces, bodies) and frame */
#define CF_MAX_TRACKS 24

/* Pinhole intrinsics of the input frame in its own pixels, plus the usable lens circle. */
struct cf_camera {
	double fx, fy, cx, cy;   /* focal lengths and principal point, pixels */
	double width, height;    /* input frame size, pixels */
	double dcx, dcy;         /* lens-circle (distortion) centre, pixels */
	int radius;              /* lens-circle radius, pixels; 0 = none */
	int out_w, out_h;        /* output size (only the aspect matters) */
	/* framing-space width (rad) of the whole sensor when the input is a crop of it: the home view is
	 * view.default of that (0 = of the input's width).  Ours: Center Stage on macOS frames from the whole
	 * sensor, so this keeps the home size on a cropped Linux stream */
	double sensor_w;
	/* Lens distortion, as the session undistorts a point: r -> r / (1 + 0.01 * P((pix * r)^2)),
	 * P = sum inv[i] x^i (the inverse polynomial, % over mm^2; the J815's come from the ISP firmware's
	 * distortion-data reply).  pix: the input frame's pixel size in mm.  All zero = a pinhole. */
	float inv[8];
	double pix;
	/* the forward polynomial, for the renderer's point validity */
	float fwd[8];
};

/* A face or a body, as detected in one frame. */
struct cf_face {
	int id;                  /* stable per detection track (>= 0); a new id may take over a lost track */
	double x, y, w, h;       /* rect normalised to the input frame [0,1] */
};

struct cf_rect {
	double x, y, w, h;
};

struct cf_kf1 {
	float R, x[3], P[3][3], prev, pv, mean, msq;
	int init;
};

struct cf_kf {
	int oid, used, seen;
	struct cf_kf1 f[4];
};

/* A track: faces (kind 1) or bodies (kind 0: the tracks framing works on) */
struct cf_track {
	int used, kind, tracked, pair, has_rel;
	long id;                 /* creation counter; < 0: a body inferred from a face */
	long oid;                /* detection id: face id*2, body id*2+1, inferred -face id */
	int64_t last_t;
	struct cf_rect obs;      /* last observation, framing space */
	struct cf_rect rel;      /* the paired face relative to this body (kept once known) */
	struct cf_rect bb, fb;   /* boundingBoxForFraming, faceBoundingBoxForFraming */
};

struct cf_params {           /* framing parameters (single person / several people) */
	float w, h, top, minf, maxf, wpad, hpad;
};

struct cf_opts {             /* the session's options */
	float hacc, hspeed, vacc, vspeed, min_fov, default_fov;
	float xA, xB, yA, yB, zacc, zA, zB, zin, zout;
	float tiny_min, tiny_max, move_tol;
	float rec_dur, rec_idle, rec_delay, home_delay, home_dur;
	float drift_dur_thr, drift_off_thr;
	int drift_enabled;
};

struct cf_engine1 {          /* AnimationEngine (one axis) */
	float pos, vel, target, dur;
	double last;
	int has_last, has_start, opts;
};

/*
 * The tuning, read at run time (cf_tuning_read: <models>/tuning/framing.conf from the user's install). Speeds
 * and accelerations are multipliers; durations in seconds; the field of view as a share of the sensor's width.
 */
struct cf_tuning {
	struct cf_params single, multi;
	float hspeed, vspeed, zin, zout, hacc, vacc, zacc;
	float default_fov, min_fov;
	float tiny_min, tiny_max, move_tol;
	float rec_dur, rec_idle, rec_delay, home_delay, home_dur;
	float drift_enabled, drift_dur, drift_off;
	float focal_length;          /* the camera's focal length in pixels of its full-resolution frame */
};
/* 0, or -1 with the reason in err. */
int cf_tuning_read(struct cf_tuning *t, const char *path, char *err, size_t errlen);

struct cf_framing {
	struct cf_camera cam;
	struct cf_opts o;
	struct cf_params single, multi;
	/* geometry */
	double b[4];             /* framing-space bounds x, y, w, h */
	float aspect, maxw;
	double rmax;             /* warp radial clamp */
	double pix2;             /* the pixel size squared (float) */
	/* face filters and tracks */
	struct cf_kf kf[CF_MAX_FACES * 4];
	struct cf_track tr[CF_MAX_TRACKS];
	int list[2][CF_MAX_TRACKS], nlist[2];   /* track slots per kind, in the reference's order */
	long counter;
	/* scene framing engine */
	const struct cf_params *fp;
	struct cf_rect deadband, slack, ideal;
	int64_t last_detect_t, hupdate_t, stationary_t, drift_start_t, drift_end_t, rec_start_t, rec_end_t,
		home_start_t;
	int last_had_subjects, state, stationary, hcount, hhead;
	struct cf_rect hist[10], drift_start_db, rec_start_db;
	float home_from[4], home_to[4];
	/* animator */
	float s[6];
	struct cf_engine1 X, Y, Z;
	struct cf_rect cur, target;
};

void cf_init(struct cf_framing *f, const struct cf_camera *cam, const struct cf_tuning *t);
/* Back to the initial (centred, default FOV) viewport; forgets tracks. */
void cf_reset(struct cf_framing *f);
/* One frame.  t_us: absolute timestamp in microseconds, monotonic.  Faces and bodies as detected in this
 * frame (either may be empty).  Returns the viewport to render. */
struct cf_rect cf_step(struct cf_framing *f, int64_t t_us, const struct cf_face *faces, int nfaces,
		       const struct cf_face *bodies, int nbodies);
/* Output pixel (u, v, 1) -> input pixel homogeneous, row-major, for viewport v and an out_w x out_h image. */
void cf_viewport_homography(const struct cf_framing *f, struct cf_rect v, int out_w, int out_h, double Hm[9]);
/* State for logs: 0 idle, 1 drift, 2 recentre. */
static inline int cf_state(const struct cf_framing *f) { return f->state; }

#endif
