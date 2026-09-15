/*
 * Copyright (c) 2024 Croxel, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "cmd_parser.h"

#include <stdbool.h>
#include <string.h>

#define CHANNEL_KEY     "CHANNEL="
#define CHANNEL_KEY_LEN (sizeof(CHANNEL_KEY) - 1)
#define CHANNEL_MIN     0
#define CHANNEL_MAX     255

/*
 * Strict base-10 parse of [value, value_end): optional leading '-', digits
 * only, no partial/trailing garbage, result in [CHANNEL_MIN, CHANNEL_MAX].
 * Returns 0 and writes *out_channel on success, -1 otherwise.
 */
static int parse_channel_value(const char *value, const char *value_end, uint8_t *out_channel)
{
	if (value == value_end) {
		return -1;
	}

	const char *p = value;
	bool negative = false;

	if (*p == '-') {
		negative = true;
		p++;
	}

	if (p == value_end) {
		return -1;
	}

	long result = 0;

	for (; p < value_end; p++) {
		if (*p < '0' || *p > '9') {
			return -1;
		}

		result = result * 10 + (*p - '0');

		if (result > CHANNEL_MAX + 1) {
			/* Already out of range; clamp to avoid unbounded growth
			 * on pathologically long digit strings.
			 */
			result = CHANNEL_MAX + 1;
		}
	}

	if (negative) {
		result = -result;
	}

	if (result < CHANNEL_MIN || result > CHANNEL_MAX) {
		return -1;
	}

	*out_channel = (uint8_t)result;
	return 0;
}

enum channel_parse_result parse_channel_field(const char *cmd, uint8_t *out_channel)
{
	const char *seg_start = cmd;

	while (*seg_start != '\0') {
		const char *seg_end = strchr(seg_start, ';');

		if (seg_end == NULL) {
			seg_end = seg_start + strlen(seg_start);
		}

		size_t seg_len = (size_t)(seg_end - seg_start);

		if (seg_len >= CHANNEL_KEY_LEN &&
		    strncmp(seg_start, CHANNEL_KEY, CHANNEL_KEY_LEN) == 0) {
			const char *value = seg_start + CHANNEL_KEY_LEN;

			if (parse_channel_value(value, seg_end, out_channel) == 0) {
				return CHANNEL_FIELD_VALID;
			}

			return CHANNEL_FIELD_INVALID;
		}

		if (*seg_end == '\0') {
			break;
		}

		seg_start = seg_end + 1;
	}

	return CHANNEL_FIELD_ABSENT;
}
