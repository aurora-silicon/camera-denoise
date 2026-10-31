// SPDX-License-Identifier: MIT
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

static void defaults(struct cdn_config *c)
{
	memset(c, 0, sizeof(*c));
	c->mode = CDN_MODE_AUTO;
	c->framing.minimum = c->framing.maximum = c->framing.default_zoom = 1;
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
	else if (settings && !strcmp(k, "backend")) snprintf(c->backend, sizeof(c->backend), "%s", v);
	else if (settings && !strcmp(k, "prefer")) snprintf(c->prefer, sizeof(c->prefer), "%s", v);
	else if (settings && !strcmp(k, "threads") && num) c->threads = (int)d;
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

int cdn_config_load(struct cdn_config *c, const char *compat_path, const char *boards_path,
		    const char *const *settings, int nsettings)
{
	char compat[1024];
	size_t n = 0;
	FILE *f;

	defaults(c);
	f = compat_path ? fopen(compat_path, "rb") : NULL;
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
	for (int i = 0; i < nsettings; i++)
		if (settings[i] && parse(c, settings[i], NULL, NULL))
			return -1;
	if (!(1 <= c->framing.minimum && c->framing.minimum <= c->framing.default_zoom &&
		  c->framing.default_zoom <= c->framing.maximum && c->framing.maximum <= 6)) {
		fprintf(stderr, "camera-denoise: invalid board framing range\n");
		return -1;
	}
	if (c->trig.exit_gain > c->trig.enter_gain)
		c->trig.exit_gain = c->trig.enter_gain;
	if (c->trig.exit_noise > c->trig.enter_noise)
		c->trig.exit_noise = c->trig.enter_noise;
	return 0;
}

void cdn_config_print(const struct cdn_config *c)
{
	fprintf(stderr,
		"camera-denoise: board %s, mode %s, backend %s (prefer %s), "
		"enter gain >= %u, exit < %u, noise %.2f/%.2f, fps <= %.0f, ramp %d/%d frames\n",
		*c->board ? c->board : "(no board data)", cdn_mode_name(c->mode), c->backend,
		c->prefer, c->trig.enter_gain, c->trig.exit_gain, c->trig.enter_noise,
		c->trig.exit_noise, c->trig.fps_threshold, c->trig.ramp_up_frames,
		c->trig.ramp_down_frames);
}
