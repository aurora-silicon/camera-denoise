// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/config.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : ".";
	char compat[512], boards[512], user[512], bad[512];
	struct cdn_config c;
	FILE *f;

	snprintf(compat, sizeof(compat), "%s/compat-test", dir);
	snprintf(boards, sizeof(boards), "%s/boards-test.conf", dir);
	snprintf(user, sizeof(user), "%s/user-test.conf", dir);
	snprintf(bad, sizeof(bad), "%s/bad-test.conf", dir);
	f = fopen(compat, "wb");
	fwrite("apple,j700\0apple,t8140\0apple,arm-platform\0", 1, 42, f);
	fclose(f);
	f = fopen(boards, "w");
	fputs("# test\n[apple,j616s]\nenter-gain = 3584\nexit-gain = 2816\n"
	      "[apple,j700]\nenter-gain = 2304  # Neo\nexit-gain = 1792\nramp-down-frames = 30\n", f);
	fclose(f);
	f = fopen(user, "w");
	fputs("[denoise]\nmode = on\nbackend = cpu\nexit-gain = 1500\ntemporal = off\nlow-light-frame-rate = off\n", f);
	fclose(f);
	f = fopen(bad, "w");
	fputs("[denoise]\nmode = sometimes\n", f);
	fclose(f);

	CHECK(cdn_config_load(&c, compat, boards, NULL, 0) == 0);
	CHECK(!strcmp(c.board, "apple,j700"));
	CHECK(c.trig.enter_gain == 2304 && c.trig.exit_gain == 1792);
	CHECK(!c.framing.supported);
	CHECK(c.mode == CDN_MODE_AUTO && !strcmp(c.backend, "auto"));
	CHECK(c.temporal);
	CHECK(c.low_light_frame_rate);
	/* auto tries the encoder's filter (kept only on its full hardware
	 * path); the GPU costs more energy than the CPU, so it is opt-in */
	CHECK(!strcmp(c.temporal_backend, "auto"));
	CHECK(!strcmp(cdn_temporal_backend(&c), "ave") && cdn_temporal_auto(&c));
	snprintf(c.temporal_backend, sizeof(c.temporal_backend), "gpu");
	CHECK(!strcmp(cdn_temporal_backend(&c), "gpu") && !cdn_temporal_auto(&c));
	snprintf(c.temporal_backend, sizeof(c.temporal_backend), "ave");
	CHECK(!strcmp(cdn_temporal_backend(&c), "ave") && !cdn_temporal_auto(&c));
	snprintf(c.temporal_backend, sizeof(c.temporal_backend), "cpu");
	CHECK(!strcmp(cdn_temporal_backend(&c), "cpu") && !cdn_temporal_auto(&c));
	snprintf(c.temporal_backend, sizeof(c.temporal_backend), "auto");
	snprintf(c.backend, sizeof(c.backend), "cpu");
	CHECK(!strcmp(cdn_temporal_backend(&c), "cpu") && !cdn_temporal_auto(&c));
	snprintf(c.temporal_backend, sizeof(c.temporal_backend), "gpu");
	CHECK(!strcmp(cdn_temporal_backend(&c), "cpu"));

	const char *s1[] = { "/nonexistent", user };
	CHECK(cdn_config_load(&c, compat, boards, s1, 2) == 0);
	CHECK(c.mode == CDN_MODE_ON && !strcmp(c.backend, "cpu"));
	CHECK(!c.temporal);
	CHECK(!c.low_light_frame_rate);
	CHECK(c.trig.enter_gain == 2304 && c.trig.exit_gain == 1500);

	/* Digital framing is a user opt-in, not a packaged board capability. */
	f = fopen(user, "a");
	fputs("framing-supported = true\nframing-zoom-min = 1\nframing-zoom-max = 6\nframing-zoom-default = 1.5\n", f);
	fclose(f);
	CHECK(cdn_config_load(&c, compat, boards, s1, 2) == 0);
	CHECK(c.framing.supported && c.framing.default_zoom == 1.5f);
	CHECK(cdn_config_load(&c, compat, boards, NULL, 0) == 0);
	CHECK(!c.framing.supported);
	f = fopen(user, "a"); fputs("framing-zoom-default = 7\n", f); fclose(f);
	CHECK(cdn_config_load(&c, compat, boards, s1, 2) != 0);

	f = fopen(user, "w");
	fputs("[denoise]\ntemporal-backend = gpu\n", f);
	fclose(f);
	CHECK(cdn_config_load(&c, compat, boards, s1, 2) == 0);
	CHECK(!strcmp(cdn_temporal_backend(&c), "gpu") && !strcmp(c.backend, "auto"));
	f = fopen(user, "w");
	fputs("[denoise]\ntemporal-backend = vulkan\n", f);
	fclose(f);
	CHECK(cdn_config_load(&c, compat, boards, s1, 2) != 0);

	const char *s2[] = { bad };
	CHECK(cdn_config_load(&c, compat, boards, s2, 1) != 0);

	/* unknown board: no thresholds, so only the temporal filter runs */
	CHECK(cdn_config_load(&c, "/nonexistent", boards, NULL, 0) == 0);
	CHECK(c.board[0] == 0 && c.trig.enter_gain == 0 && c.temporal);

    f = fopen(boards, "a");
    fputs("framing-supported = true\nframing-zoom-min = 1\nframing-zoom-max = 6\nframing-zoom-default = 1.5\n", f);
    fclose(f);
    CHECK(cdn_config_load(&c, compat, boards, NULL, 0) == 0);
    CHECK(c.framing.supported && c.framing.minimum == 1 && c.framing.maximum == 6 && c.framing.default_zoom == 1.5f);
    f = fopen(boards, "a"); fputs("framing-zoom-default = 7\n", f); fclose(f);
    CHECK(cdn_config_load(&c, compat, boards, NULL, 0) != 0);
	/* A user may opt in to framing on a board without framing data. */
	{
		char optin[512];
		snprintf(optin, sizeof(optin), "%s/framing-optin.conf", dir);
		f = fopen(optin, "w");
		fputs("[denoise]\nframing-supported = true\nframing-zoom-min = 1\nframing-zoom-max = 2\nframing-zoom-default = 1\n", f);
		fclose(f);
		f = fopen(boards, "w"); fputs("[apple,j700]\nenter-gain = 2304\n", f); fclose(f);
		const char *s3[] = { optin };
		CHECK(cdn_config_load(&c, compat, boards, NULL, 0) == 0 && !c.framing.supported);
		CHECK(cdn_config_load(&c, compat, boards, s3, 1) == 0);
		CHECK(c.framing.supported && c.framing.minimum == 1 && c.framing.maximum == 2 && c.framing.default_zoom == 1);
		f = fopen(optin, "a"); fputs("framing-zoom-max = 9\n", f); fclose(f);
		CHECK(cdn_config_load(&c, compat, boards, s3, 1) != 0);
		remove(optin);
	}

	/* Sensor-keyed profiles: [sensor:<name>] takes priority over board
	 * compatible fallback, covers J614s/J616s/J614c/J616c via shipped
	 * boards.conf, and upgrades cleanly after deferred camera detection. */
	{
		const char *shipped = argc > 2 ? argv[2] : "../data/boards.conf";
		char sysfs[256], v0_dev[1024], v0_drv[1088], v0_sensor[1088];
		char v1_dev[1024], v1_drv[1088], v1_sensor[1088], tmp[1024];
		char compat_m4[512], sensor[64] = "";

		snprintf(compat_m4, sizeof(compat_m4), "%s/compat-j616c", dir);
		f = fopen(compat_m4, "wb");
		fwrite("apple,j616c\0apple,t6041\0apple,arm-platform\0", 1, 43, f);
		fclose(f);

		CHECK(cdn_config_load_sensor(&c, "imx958-uw", compat_m4, shipped, NULL, 0) == 0);
		CHECK(!strcmp(c.board, "sensor:imx958-uw"));
		CHECK(c.trig.enter_gain == 3584 && c.trig.exit_gain == 2816);
		CHECK(c.framing.supported && c.framing.minimum == 1.0f &&
		      c.framing.maximum == 6.0f && c.framing.default_zoom == 1.5f);

		/* User settings still override [sensor:imx958-uw] defaults. */
		f = fopen(user, "w");
		fputs("[denoise]\nexit-gain = 2400\nmode = on\n", f);
		fclose(f);
		const char *su[] = { user };
		CHECK(cdn_config_load_sensor(&c, "imx958-uw", compat_m4, shipped, su, 1) == 0);
		CHECK(!strcmp(c.board, "sensor:imx958-uw"));
		CHECK(c.mode == CDN_MODE_ON && c.trig.enter_gain == 3584 && c.trig.exit_gain == 2400);

		/*
		 * A kernel without the sensor attribute (before linux#2216), or one
		 * that has not detected the camera yet, gives no sensor: the J616s
		 * keeps the [apple,j616s] profile it had before sensor keys.
		 */
		{
			char compat_j616s[512];

			snprintf(compat_j616s, sizeof(compat_j616s), "%s/compat-j616s", dir);
			f = fopen(compat_j616s, "wb");
			fwrite("apple,j616s\0apple,t6040\0apple,arm-platform\0", 1, 43, f);
			fclose(f);
			CHECK(cdn_config_load_sensor(&c, NULL, compat_j616s, shipped, NULL, 0) == 0);
			CHECK(!strcmp(c.board, "apple,j616s"));
			CHECK(c.trig.enter_gain == 3584 && c.trig.exit_gain == 2816);
			CHECK(c.framing.supported && c.framing.maximum == 6.0f &&
			      c.framing.default_zoom == 1.5f);
			CHECK(cdn_config_load_sensor(&c, "", compat_j616s, shipped, NULL, 0) == 0);
			CHECK(!strcmp(c.board, "apple,j616s"));
		}

		/* Sensor section wins over [<compatible>] fallback; unknown sensor falls back. */
		CHECK(cdn_config_load_sensor(&c, "imx958-uw", compat, shipped, NULL, 0) == 0);
		CHECK(!strcmp(c.board, "sensor:imx958-uw") && c.trig.enter_gain == 3584);
		CHECK(cdn_config_load_sensor(&c, "imx558", compat, shipped, NULL, 0) == 0);
		CHECK(!strcmp(c.board, "apple,j700") && c.trig.enter_gain == 2304 && !c.framing.supported);

		/* Synthetic sysfs tree: deferred-start empty -> populated sensor, filtered by driver. */
		snprintf(sysfs, sizeof(sysfs), "%s/sysfs-test", dir);
		snprintf(v0_dev, sizeof(v0_dev), "%s/class/video4linux/video0/device", sysfs);
		snprintf(v0_drv, sizeof(v0_drv), "%s/driver", v0_dev);
		snprintf(v0_sensor, sizeof(v0_sensor), "%s/sensor", v0_dev);
		snprintf(v1_dev, sizeof(v1_dev), "%s/class/video4linux/video1/device", sysfs);
		snprintf(v1_drv, sizeof(v1_drv), "%s/driver", v1_dev);
		snprintf(v1_sensor, sizeof(v1_sensor), "%s/sensor", v1_dev);
		mkdir(sysfs, 0755);
		snprintf(tmp, sizeof(tmp), "%s/class", sysfs); mkdir(tmp, 0755);
		snprintf(tmp, sizeof(tmp), "%s/class/video4linux", sysfs); mkdir(tmp, 0755);
		snprintf(tmp, sizeof(tmp), "%s/class/video4linux/video0", sysfs); mkdir(tmp, 0755);
		mkdir(v0_dev, 0755);
		snprintf(tmp, sizeof(tmp), "%s/class/video4linux/video1", sysfs); mkdir(tmp, 0755);
		mkdir(v1_dev, 0755);
		remove(v0_drv); remove(v1_drv); remove(v0_sensor); remove(v1_sensor);
		CHECK(symlink("/sys/bus/platform/drivers/uvcvideo", v0_drv) == 0);
		CHECK(symlink("/sys/bus/platform/drivers/apple-isp", v1_drv) == 0);
		f = fopen(v0_sensor, "w"); fputs("other-sensor\n", f); fclose(f);

		/* video1 (apple-isp) has no sensor yet (deferred start); video0 (uvcvideo) is ignored. */
		CHECK(!cdn_v4l2_sensor("/dev/video1", sysfs, sensor, sizeof(sensor)));
		CHECK(!cdn_sysfs_find_sensor(sysfs, "apple-isp", sensor, sizeof(sensor)));
		CHECK(cdn_config_load_sensor(&c, sensor, compat, shipped, NULL, 0) == 0);
		CHECK(!strcmp(c.board, "apple,j700"));

		/* Stream start detects the camera and populates sysfs sensor. */
		f = fopen(v1_sensor, "w"); fputs("imx958-uw\n", f); fclose(f);
		CHECK(cdn_v4l2_sensor("/dev/video1", sysfs, sensor, sizeof(sensor)));
		CHECK(!strcmp(sensor, "imx958-uw"));
		sensor[0] = 0;
		CHECK(cdn_sysfs_find_sensor(sysfs, "apple-isp", sensor, sizeof(sensor)));
		CHECK(!strcmp(sensor, "imx958-uw"));
		CHECK(cdn_config_load_sensor(&c, sensor, compat, shipped, NULL, 0) == 0);
		CHECK(!strcmp(c.board, "sensor:imx958-uw") && c.framing.supported);

		remove(v0_sensor); remove(v1_sensor); remove(v0_drv); remove(v1_drv);
		rmdir(v0_dev); rmdir(v1_dev);
		snprintf(tmp, sizeof(tmp), "%s/class/video4linux/video0", sysfs); rmdir(tmp);
		snprintf(tmp, sizeof(tmp), "%s/class/video4linux/video1", sysfs); rmdir(tmp);
		snprintf(tmp, sizeof(tmp), "%s/class/video4linux", sysfs); rmdir(tmp);
		snprintf(tmp, sizeof(tmp), "%s/class", sysfs); rmdir(tmp);
		rmdir(sysfs);
		remove(compat_m4);
	}
	remove(compat); remove(boards); remove(user); remove(bad);
	printf(fails ? "FAIL\n" : "PASS\n");
	return fails != 0;
}
