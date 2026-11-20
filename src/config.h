/* SPDX-License-Identifier: MIT */
#ifndef CDN_CONFIG_H
#define CDN_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#include "trigger.h"
#include "picture.h"

struct cdn_framing_cfg {
	bool supported;
	float minimum, maximum, default_zoom;
};

struct cdn_config {
	struct cdn_framing_cfg framing;
	struct cdn_trigger_cfg trig;
	enum cdn_mode mode;
	/*
	 * Run the temporal filter on every frame, as macOS runs its
	 * motion-compensated temporal filter on every camera stream. The
	 * trigger then only decides when the low-light stage joins in.
	 */
	bool temporal;
	/*
	 * Let the camera slow down to 15 fps in low light for a longer exposure,
	 * as macOS does (MinFrameRateOverride 15), instead of raising the gain.
	 */
	bool low_light_frame_rate;
	/*
	 * The temporal stage: auto, cpu or gpu. auto is the CPU for now. On the
	 * Neo the Vulkan path costs about 3.3x the CPU path's energy, so the GPU
	 * stays opt-in until it is cheaper.
	 */
	char temporal_backend[16];
	char backend[32];	/* auto, cpu or a plugin name */
	char prefer[64];	/* plugins "auto" tries before cpu */
	int threads;		/* 0: backend default */
	/*
	 * Tuning: strength mixes the filtered picture with the camera's
	 * (1 = filtered only, 0.5 = half of each), spatial scales the 3x3
	 * smoothing that follows the temporal filter (0 = none, 1 = full),
	 * temporal_frames caps the running mean (fewer = less smear on motion,
	 * more grain left).
	 */
	float strength;
	float spatial;
	int temporal_frames;
	/* the picture stage after the denoiser (picture.h): all neutral by default */
	struct cdn_picture_cfg picture;
	/*
	 * Digital zoom without the effects plugin (zoom.h): a centred crop scaled
	 * back to the stream's size, 1 = off. A stream's camera.zoom control wins.
	 * zoom_engine: auto (the SoC scaler when present, else the CPU), cpu, scaler.
	 */
	float zoom;
	char zoom_engine[8];
	char drivers[128];	/* V4L2 drivers whose sources get a filter */
	char board[64];		/* matched sensor:<name> or compatible, "" if none */
};

/* true when @drv appears in the comma/space-separated @drivers list */
bool cdn_driver_listed(const char *drivers, const char *drv);

/*
 * Read the camera sensor name from <sysfs_root>/class/video4linux/<dev>/device/sensor
 * (@sysfs_root NULL means "/sys"). Returns false when absent or not yet detected
 * (-ENODATA before a deferred camera start).
 */
bool cdn_v4l2_sensor(const char *v4l2_path, const char *sysfs_root,
		     char *sensor, size_t sensor_sz);

/*
 * Scan <sysfs_root>/class/video4linux for the first camera bound to a driver in
 * @drivers (or any driver if @drivers is NULL/empty) that already reports a
 * sensor name in sysfs.
 */
bool cdn_sysfs_find_sensor(const char *sysfs_root, const char *drivers,
			   char *sensor, size_t sensor_sz);

/*
 * Defaults, then [sensor:<sensor>] in boards_path if @sensor is non-empty and
 * present, otherwise the [<compatible>] section of boards_path matching the
 * first device-tree compatible that has one, then each settings file in order
 * ([denoise] section; later files win). Missing files are fine.
 * Returns 0, or -1 with a message on stderr for a malformed file.
 */
int cdn_config_load_sensor(struct cdn_config *c, const char *sensor,
			   const char *compat_path, const char *boards_path,
			   const char *const *settings, int nsettings);

int cdn_config_load(struct cdn_config *c, const char *compat_path,
		    const char *boards_path, const char *const *settings,
		    int nsettings);

/* The plugin the temporal stage runs on: "cpu" or "gpu" */
const char *cdn_temporal_backend(const struct cdn_config *c);
/* true when the temporal backend was picked by auto (keep it only if it
 * runs its full hardware path) */
bool cdn_temporal_auto(const struct cdn_config *c);

void cdn_config_print(const struct cdn_config *c);

#endif
