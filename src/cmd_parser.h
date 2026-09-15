/*
 * Copyright (c) 2024 Croxel, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef CMD_PARSER_H_
#define CMD_PARSER_H_

#include <stdint.h>

/*
 * Deliberately dependency-free (no Zephyr/BT headers) so this module can be
 * unit-tested as a plain host binary. Keep it that way.
 */

enum channel_parse_result {
	CHANNEL_FIELD_ABSENT,
	CHANNEL_FIELD_VALID,
	CHANNEL_FIELD_INVALID,
};

/**
 * @brief Find and validate a CHANNEL=<0..255> field in a ';'-delimited command.
 *
 * Scans ';'-delimited segments of @p cmd for one starting with "CHANNEL=".
 * The value must be a strict base-10 integer (optional leading '-', digits
 * only, no partial/trailing garbage) in the range 0..255. If more than one
 * CHANNEL= segment is present, only the first is considered.
 *
 * @param cmd NUL-terminated command string (may be empty).
 * @param out_channel Set to the parsed value on CHANNEL_FIELD_VALID; left
 *        untouched otherwise.
 * @return CHANNEL_FIELD_ABSENT if no CHANNEL= segment is found,
 *         CHANNEL_FIELD_VALID if found and valid,
 *         CHANNEL_FIELD_INVALID if found but malformed or out of range.
 */
enum channel_parse_result parse_channel_field(const char *cmd, uint8_t *out_channel);

#endif /* CMD_PARSER_H_ */
