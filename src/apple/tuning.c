// SPDX-License-Identifier: MIT
#include "tuning.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int apple_tuning_read(const char *path, const struct apple_tuning_key *keys, size_t n, char *err, size_t errlen)
{
	bool seen[64] = { 0 };
	char line[256];
	FILE *f;

	if (n > sizeof(seen) / sizeof(seen[0])) {
		snprintf(err, errlen, "too many keys");
		return -1;
	}
	f = fopen(path, "re");
	if (!f) {
		snprintf(err, errlen, "%s: %s", path, strerror(errno));
		return -1;
	}
	while (fgets(line, sizeof(line), f)) {
		char *hash = strchr(line, '#'), *eq, *k, *v, *end;

		if (hash)
			*hash = 0;
		eq = strchr(line, '=');
		if (!eq)
			continue;
		*eq = 0;
		k = trim(line);
		v = trim(eq + 1);
		for (size_t i = 0; i < n; i++) {
			if (strcmp(k, keys[i].key))
				continue;
			errno = 0;
			float d = strtof(v, &end);

			if (errno || end == v || *end) {
				snprintf(err, errlen, "%s: %s: not a number", path, k);
				fclose(f);
				return -1;
			}
			*keys[i].value = d;
			seen[i] = true;
		}
	}
	fclose(f);
	for (size_t i = 0; i < n; i++)
		if (!seen[i]) {
			snprintf(err, errlen, "%s: no %s", path, keys[i].key);
			return -1;
		}
	return 0;
}
