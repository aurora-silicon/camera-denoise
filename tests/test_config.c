// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <string.h>

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
	remove(compat); remove(boards); remove(user); remove(bad);
	printf(fails ? "FAIL\n" : "PASS\n");
	return fails != 0;
}
