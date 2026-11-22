/* SPDX-License-Identifier: MIT */
#ifndef CDN_APPLE_TUNING_H
#define CDN_APPLE_TUNING_H
/*
 * Effect tuning read at run time from the user's install (install-apple-effects puts it in <models>/tuning/
 * next to the networks; nothing of it is in this tree). A file is "key = value" lines, '#' starts a comment.
 * Every key a caller asks for must be there: an effect whose tuning is missing stays off.
 */
#include <stddef.h>

struct apple_tuning_key {
	const char *key;
	float *value;
};

/* 0 when every key was read; -1 with a reason in err (a missing file, key or a bad value). */
int apple_tuning_read(const char *path, const struct apple_tuning_key *keys, size_t n, char *err, size_t errlen);

#endif
