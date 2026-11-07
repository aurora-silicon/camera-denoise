// SPDX-License-Identifier: MIT
/* The published output formats: NV12, no DMA-BUF modifier, one per camera size. */
#include <stdio.h>
#include <stdlib.h>
#include <spa/param/format.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include "../src/pipewire-frame.h"
static int fails;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); fails++; } } while (0)

static struct spa_pod *camera(struct spa_pod_builder *b, uint32_t fmt, bool modifier, bool choice)
{
	struct spa_pod_frame f;

	spa_pod_builder_push_object(b, &f, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
	spa_pod_builder_add(b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
			    SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);
	if (choice)
		spa_pod_builder_add(b, SPA_FORMAT_VIDEO_format,
				    SPA_POD_CHOICE_ENUM_Id(3, SPA_VIDEO_FORMAT_YUY2,
							   SPA_VIDEO_FORMAT_YUY2, fmt), 0);
	else
		spa_pod_builder_add(b, SPA_FORMAT_VIDEO_format, SPA_POD_Id(fmt), 0);
	if (modifier) {
		spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY);
		spa_pod_builder_long(b, 0);
	}
	spa_pod_builder_add(b, SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&SPA_RECTANGLE(1920, 1080)),
			    SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&SPA_FRACTION(30, 1)), 0);
	return spa_pod_builder_pop(b, &f);
}

int main(void)
{
	uint8_t buf[4096];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	struct spa_pod *with = camera(&b, SPA_VIDEO_FORMAT_NV12, true, false);
	struct spa_pod *plain = camera(&b, SPA_VIDEO_FORMAT_NV12, false, false);
	struct spa_pod *listed = camera(&b, SPA_VIDEO_FORMAT_NV12, true, true);
	struct spa_pod *yuy2 = camera(&b, SPA_VIDEO_FORMAT_YUY2, false, false);
	struct spa_pod *a = cdn_output_format(with), *p = cdn_output_format(plain);
	struct spa_pod *l = cdn_output_format(listed);
	struct spa_video_info_raw r = { 0 };

	CHECK(a && p && l);
	if (!a || !p || !l)
		return 1;
	/* A modifier makes the client fixate DMA-BUF buffers we cannot fill. */
	CHECK(!spa_pod_find_prop(a, NULL, SPA_FORMAT_VIDEO_modifier));
	CHECK(spa_format_video_raw_parse(a, &r) >= 0);
	CHECK(r.format == SPA_VIDEO_FORMAT_NV12 && !(r.flags & SPA_VIDEO_FLAG_MODIFIER));
	CHECK(r.size.width == 1920 && r.size.height == 1080);
	/* With and without a modifier are one output format. */
	CHECK(cdn_format_equal(a, p));
	/* A format choice that includes NV12 publishes NV12 alone. */
	CHECK(cdn_format_equal(l, p));
	CHECK(!cdn_output_format(yuy2));
	free(a); free(p); free(l);
	if (fails)
		fprintf(stderr, "%d failures\n", fails);
	return fails != 0;
}
