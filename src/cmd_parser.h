/*
 * Copyright (c) 2024 Croxel, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef CMD_PARSER_H_
#define CMD_PARSER_H_

#include <stdbool.h>
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

/*
 * Multi-channel measurement request (UC8), see
 * docs/ble/BLE_CHUNKED_TRACE_PROTOCOL.md:
 *
 *   REQ=<id>;N=<n>;SLOT=0;CHANNEL=..;START=..;END=..;FREQ=..;RANGE=..;SLOT=1;...
 */

#define MULTI_MAX_SLOTS 6

struct slot_cfg {
	uint8_t channel;
	int32_t start_mv;
	int32_t end_mv;
	int32_t freq_hz;
	int32_t range_ua;
};

struct multi_request {
	uint16_t id;
	uint8_t n;
	struct slot_cfg slots[MULTI_MAX_SLOTS];
};

enum request_error {
	REQUEST_OK = 0,
	/* Level 1: syntax */
	REQUEST_ERR_SYNTAX,
	REQUEST_ERR_SLOT_COUNT,
	REQUEST_ERR_MISSING,
	REQUEST_ERR_DUP,
	REQUEST_ERR_UNKNOWN,
	/* Level 2: hardware-independent range */
	REQUEST_ERR_RANGE,
	/* Level 3: hardware capability. Never produced by the parser; reported
	 * by the firmware's hardware check.
	 */
	REQUEST_ERR_HW,
};

struct request_error_info {
	enum request_error code;
	/* -1 for request-level errors (reported as ERR=<code>), otherwise the
	 * slot the error belongs to (reported as V<slot>,<key>,<code>).
	 */
	int slot;
	/* Field name for slot-level errors; "?" when the field is not a known key. */
	const char *key;
};

/**
 * @brief Whether @p cmd is a multi-channel request (contains a REQ= field).
 *
 * Commands without REQ= are legacy single-channel commands.
 */
bool is_multi_request(const char *cmd);

/**
 * @brief Parse and validate (levels 1 and 2) a multi-channel request.
 *
 * All level-1 (syntax) errors take precedence over level-2 (range) errors.
 * Within a level, the first error in request order is reported.
 *
 * @param cmd NUL-terminated command string, without the '#' terminator.
 * @param out Filled on REQUEST_OK; unspecified otherwise.
 * @param err Filled on failure; untouched on REQUEST_OK.
 * @return REQUEST_OK or the error code (also stored in @p err).
 */
enum request_error parse_multi_request(const char *cmd, struct multi_request *out,
				       struct request_error_info *err);

/** @brief Wire token for an error code, e.g. "MISSING". */
const char *request_error_str(enum request_error code);

#endif /* CMD_PARSER_H_ */
