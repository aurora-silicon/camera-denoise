// SPDX-License-Identifier: MIT
#include <stdio.h>

#include "../src/ave/mctf_level.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void)
{
	struct cdn_mctf_level l;
	int v;

	cdn_mctf_level_init(&l, 2304, 1792);
	/* a stream starts high, then settles at the room level */
	for (int i = 0; i < CDN_MCTF_START_FRAMES; i++)
		CHECK(cdn_mctf_level_step(&l, 400) == CDN_MCTF_LEVEL_START);
	CHECK(cdn_mctf_level_step(&l, 400) == CDN_MCTF_LEVEL_ROOM);
	/* dark at the enter gain, and it stays dark until below the exit gain */
	CHECK(cdn_mctf_level_step(&l, 2304) == CDN_MCTF_LEVEL_DARK);
	CHECK(cdn_mctf_level_step(&l, 2000) == CDN_MCTF_LEVEL_DARK);
	CHECK(cdn_mctf_level_step(&l, 0) == CDN_MCTF_LEVEL_DARK);
	CHECK(cdn_mctf_level_step(&l, 1791) == CDN_MCTF_LEVEL_ROOM);
	CHECK(cdn_mctf_level_step(&l, 2303) == CDN_MCTF_LEVEL_ROOM);
	/* a reset (new stream) restarts the start frames but keeps the light */
	cdn_mctf_level_step(&l, 3000);
	cdn_mctf_level_reset(&l);
	v = cdn_mctf_level_step(&l, 3000);
	CHECK(v == CDN_MCTF_LEVEL_START);
	for (int i = 1; i < CDN_MCTF_START_FRAMES; i++)
		cdn_mctf_level_step(&l, 3000);
	CHECK(cdn_mctf_level_step(&l, 3000) == CDN_MCTF_LEVEL_DARK);
	/* exit above enter is clamped */
	cdn_mctf_level_init(&l, 1000, 2000);
	CHECK(l.dark_exit_gain == 1000);
	if (!fails)
		printf("mctf_level: ok\n");
	return fails != 0;
}
