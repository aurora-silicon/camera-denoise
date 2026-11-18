// SPDX-License-Identifier: MIT
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"

static void defaults(struct cdn_config *c)
{
	memset(c, 0, sizeof(*c));
	c->mode = CDN_MODE_AUTO;
	c->framing.minimum = c->framing.maximum = c->framing.default_zoom = 1;
	c->temporal = true;
	c->low_light_frame_rate = true;
	snprintf(c->temporal_backend, sizeof(c->temporal_backend), "auto");
	c->strength = 1.0f;
	c->spatial = 1.0f;
	c->temporal_frames = 8;
	snprintf(c->backend, sizeof(c->backend), "auto");
	snprintf(c->prefer, sizeof(c->prefer), "ane");
	snprintf(c->drivers, sizeof(c->drivers), "apple-isp");
	c->trig.fps_threshold = 30;
	c->trig.ramp_down_frames = 30;
	c->trig.ramp_up_frames = 0;
	c->trig.enter_noise = -1;
	c->trig.exit_noise = -1;
}

static char *trim(char *s)
{
	char *e;

	while (isspace((unsigned char)*s))
		s++;
	e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		*--e = 0;
	return s;
}

static int set_key(struct cdn_config *c, const char *k, const char *v, bool settings)
{
	char *end;
	double d = strtod(v, &end);
	bool num = end != v && *trim(end) == 0;

	/* Board data declares framing capability; a user's settings file may opt
	 * in on other boards (their choice), with the same keys and bounds. */
	if (!strcmp(k, "framing-supported") && (!strcmp(v,"true") || !strcmp(v,"false")))
		c->framing.supported = !strcmp(v,"true");
	else if (!strcmp(k,"framing-zoom-min") && num && isfinite(d)) c->framing.minimum = (float)d;
	else if (!strcmp(k,"framing-zoom-max") && num && isfinite(d)) c->framing.maximum = (float)d;
	else if (!strcmp(k,"framing-zoom-default") && num && isfinite(d)) c->framing.default_zoom = (float)d;
	else if (!strcmp(k, "enter-gain") && num) c->trig.enter_gain = (unsigned)d;
	else if (!strcmp(k, "exit-gain") && num) c->trig.exit_gain = (unsigned)d;
	else if (!strcmp(k, "enter-noise") && num) c->trig.enter_noise = (float)d;
	else if (!strcmp(k, "exit-noise") && num) c->trig.exit_noise = (float)d;
	else if (!strcmp(k, "fps-threshold") && num) c->trig.fps_threshold = (float)d;
	else if (!strcmp(k, "ramp-up-frames") && num) c->trig.ramp_up_frames = (int)d;
	else if (!strcmp(k, "ramp-down-frames") && num) c->trig.ramp_down_frames = (int)d;
	else if (settings && !strcmp(k, "mode")) return cdn_mode_parse(v, &c->mode);
	else if (settings && !strcmp(k, "temporal")) {
		if (!strcmp(v, "on")) c->temporal = true;
		else if (!strcmp(v, "off")) c->temporal = false;
		else return -1;
	}
	else if (settings && !strcmp(k, "low-light-frame-rate")) {
		if (!strcmp(v, "on")) c->low_light_frame_rate = true;
		else if (!strcmp(v, "off")) c->low_light_frame_rate = false;
		else return -1;
	}
	else if (settings && !strcmp(k, "temporal-backend")) {
		if (strcmp(v, "auto") && strcmp(v, "cpu") && strcmp(v, "gpu") && strcmp(v, "ave"))
			return -1;
		snprintf(c->temporal_backend, sizeof(c->temporal_backend), "%s", v);
	}
	else if (settings && !strcmp(k, "backend")) snprintf(c->backend, sizeof(c->backend), "%s", v);
	else if (settings && !strcmp(k, "prefer")) snprintf(c->prefer, sizeof(c->prefer), "%s", v);
	else if (settings && !strcmp(k, "threads") && num) c->threads = (int)d;
	else if (settings && !strcmp(k, "strength") && num && d >= 0 && d <= 1) c->strength = (float)d;
	else if (settings && !strcmp(k, "spatial") && num && d >= 0 && d <= 1) c->spatial = (float)d;
	else if (settings && !strcmp(k, "temporal-frames") && num && d >= 2 && d <= 8 && d == (int)d)
		c->temporal_frames = (int)d;
	else if (settings && !strcmp(k, "drivers")) snprintf(c->drivers, sizeof(c->drivers), "%s", v);
	else return -1;
	return 0;
}

/* section == NULL: apply [denoise]; else apply [section] only */
static int parse(struct cdn_config *c, const char *path, const char *section, bool *found)
{
	FILE *f = fopen(path, "r");
	char line[512], cur[128] = "";
	int lineno = 0, err = 0;

	if (!f)
		return errno == ENOENT ? 0 : (fprintf(stderr, "camera-denoise: %s: %s\n", path, strerror(errno)), -1);
	while (fgets(line, sizeof(line), f)) {
		char *s = trim(line), *eq;

		lineno++;
		if (!*s || *s == '#' || *s == ';')
			continue;
		if (*s == '[') {
			char *e = strchr(s, ']');
			if (!e) { err = -2; break; }
			*e = 0;
			snprintf(cur, sizeof(cur), "%s", trim(s + 1));
			if (section && found && !strcmp(cur, section))
				*found = true;
			continue;
		}
		if (!(eq = strchr(s, '='))) { err = -2; break; }
		*eq = 0;
		char *k = trim(s), *v = trim(eq + 1);
		char *hash = strchr(v, '#');
		if (hash) { *hash = 0; v = trim(v); }
		bool want = section ? !strcmp(cur, section) : !strcmp(cur, "denoise");
		if (want && set_key(c, k, v, section == NULL)) {
			fprintf(stderr, "camera-denoise: %s:%d: bad setting '%s = %s'\n", path, lineno, k, v);
			err = -1;
			break;
		}
	}
	if (err == -2)
		fprintf(stderr, "camera-denoise: %s:%d: syntax error\n", path, lineno);
	fclose(f);
	return err ? -1 : 0;
}

bool cdn_driver_listed(const char *drivers, const char *drv)
{
	char buf[128], *save = NULL;

	if (!drivers || !drv || !*drv)
		return false;
	snprintf(buf, sizeof(buf), "%s", drivers);
	for (char *t = strtok_r(buf, ", ", &save); t; t = strtok_r(NULL, ", ", &save))
		if (!strcmp(t, drv))
			return true;
	return false;
}

bool cdn_v4l2_sensor(const char *v4l2_path, const char *sysfs_root,
		     char *sensor, size_t sensor_sz)
{
	const char *root = sysfs_root && *sysfs_root ? sysfs_root : "/sys";
	const char *slash, *dev;
	char path[PATH_MAX], buf[64], *s;
	ssize_t n;
	int fd;

	if (!sensor || !sensor_sz)
		return false;
	sensor[0] = 0;
	if (!v4l2_path || !*v4l2_path)
		return false;
	slash = strrchr(v4l2_path, '/');
	dev = slash ? slash + 1 : v4l2_path;
	if (!*dev || dev[0] == '.')
		return false;
	if ((size_t)snprintf(path, sizeof(path), "%s/class/video4linux/%s/device/sensor",
			     root, dev) >= sizeof(path))
		return false;
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return false;
	buf[n] = 0;
	s = trim(buf);
	if (!*s || strlen(s) >= sensor_sz)
		return false;
	snprintf(sensor, sensor_sz, "%s", s);
	return true;
}

bool cdn_sysfs_find_sensor(const char *sysfs_root, const char *drivers,
			   char *sensor, size_t sensor_sz)
{
	const char *root = sysfs_root && *sysfs_root ? sysfs_root : "/sys";
	char dirpath[PATH_MAX], best_dev[256] = "", best_sensor[64] = "";
	struct dirent *de;
	DIR *d;

	if (!sensor || !sensor_sz)
		return false;
	sensor[0] = 0;
	if ((size_t)snprintf(dirpath, sizeof(dirpath), "%s/class/video4linux", root) >=
	    sizeof(dirpath))
		return false;
	d = opendir(dirpath);
	if (!d)
		return false;
	while ((de = readdir(d)) != NULL) {
		char cand[64];

		if (de->d_name[0] == '.')
			continue;
		if (drivers && *drivers) {
			char lpath[PATH_MAX], target[PATH_MAX];
			const char *drv;
			ssize_t ln;

			if ((size_t)snprintf(lpath, sizeof(lpath),
					     "%s/class/video4linux/%s/device/driver",
					     root, de->d_name) >= sizeof(lpath))
				continue;
			ln = readlink(lpath, target, sizeof(target) - 1);
			if (ln <= 0)
				continue;
			target[ln] = 0;
			drv = strrchr(target, '/');
			drv = drv ? drv + 1 : target;
			if (!cdn_driver_listed(drivers, drv))
				continue;
		}
		if (!cdn_v4l2_sensor(de->d_name, root, cand, sizeof(cand)))
			continue;
		if (!best_dev[0] || strcmp(de->d_name, best_dev) < 0) {
			snprintf(best_dev, sizeof(best_dev), "%s", de->d_name);
			snprintf(best_sensor, sizeof(best_sensor), "%s", cand);
		}
	}
	closedir(d);
	if (!best_sensor[0] || strlen(best_sensor) >= sensor_sz)
		return false;
	snprintf(sensor, sensor_sz, "%s", best_sensor);
	return true;
}

int cdn_config_load_sensor(struct cdn_config *c, const char *sensor,
			   const char *compat_path, const char *boards_path,
			   const char *const *settings, int nsettings)
{
	bool matched = false;

	defaults(c);
	if (sensor && *sensor) {
		char sec[sizeof(c->board)];
		bool found = false;

		if ((size_t)snprintf(sec, sizeof(sec), "sensor:%s", sensor) < sizeof(sec)) {
			if (parse(c, boards_path, sec, &found))
				return -1;
			if (found) {
				snprintf(c->board, sizeof(c->board), "%s", sec);
				matched = true;
			}
		}
	}
	if (!matched) {
		char compat[1024];
		size_t n = 0;
		FILE *f = compat_path ? fopen(compat_path, "rb") : NULL;

		if (f) {
			n = fread(compat, 1, sizeof(compat) - 1, f);
			fclose(f);
		}
		compat[n] = 0;
		/* NUL-separated list, most specific first */
		for (size_t off = 0; off < n; off += strlen(compat + off) + 1) {
			bool found = false;
			const char *name = compat + off;

			if (!*name)
				continue;
			if (parse(c, boards_path, name, &found))
				return -1;
			if (found) {
				snprintf(c->board, sizeof(c->board), "%s", name);
				break;
			}
		}
	}
	for (int i = 0; i < nsettings; i++)
		if (settings[i] && parse(c, settings[i], NULL, NULL))
			return -1;
	if (!(1 <= c->framing.minimum && c->framing.minimum <= c->framing.default_zoom &&
		  c->framing.default_zoom <= c->framing.maximum && c->framing.maximum <= 6)) {
		fprintf(stderr, "camera-denoise: invalid framing zoom range (needs 1 <= min <= default <= max <= 6)\n");
		return -1;
	}
	if (c->trig.exit_gain > c->trig.enter_gain)
		c->trig.exit_gain = c->trig.enter_gain;
	if (c->trig.exit_noise > c->trig.enter_noise)
		c->trig.exit_noise = c->trig.enter_noise;
	return 0;
}

int cdn_config_load(struct cdn_config *c, const char *compat_path, const char *boards_path,
		    const char *const *settings, int nsettings)
{
	return cdn_config_load_sensor(c, NULL, compat_path, boards_path, settings, nsettings);
}

const char *cdn_temporal_backend(const struct cdn_config *c)
{
	/* backend = cpu is the all-CPU baseline for measurements */
	if (!strcmp(c->backend, "cpu"))
		return "cpu";
	if (!strcmp(c->temporal_backend, "gpu") || !strcmp(c->temporal_backend, "ave"))
		return c->temporal_backend;
	/* auto: the video encoder's filter where it runs fully in hardware
	 * (macOS's output at a fraction of the CPU energy), else the CPU */
	if (!strcmp(c->temporal_backend, "auto"))
		return "ave";
	return "cpu";
}

bool cdn_temporal_auto(const struct cdn_config *c)
{
	return !strcmp(c->temporal_backend, "auto") && strcmp(c->backend, "cpu");
}

void cdn_config_print(const struct cdn_config *c)
{
	fprintf(stderr,
		"camera-denoise: board %s, mode %s, temporal %s (%s), low-light frame rate %s, "
		"backend %s (prefer %s), "
		"enter gain >= %u, exit < %u, noise %.2f/%.2f, fps <= %.0f, ramp %d/%d frames, "
		"strength %.2f, spatial %.2f, temporal frames %d\n",
		*c->board ? c->board : "(no board data)", cdn_mode_name(c->mode),
		c->temporal ? "on" : "off", cdn_temporal_auto(c) ? "auto" : cdn_temporal_backend(c), c->low_light_frame_rate ? "on" : "off", c->backend,
		c->prefer, c->trig.enter_gain, c->trig.exit_gain, c->trig.enter_noise,
		c->trig.exit_noise, c->trig.fps_threshold, c->trig.ramp_up_frames,
		c->trig.ramp_down_frames, c->strength, c->spatial, c->temporal_frames);
}
