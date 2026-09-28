/*
 * Copyright (c) 2024 Croxel, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "cmd_parser.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define CHANNEL_KEY     "CHANNEL="
#define CHANNEL_KEY_LEN (sizeof(CHANNEL_KEY) - 1)
#define CHANNEL_MIN     0
#define CHANNEL_MAX     255
#define REQ_ID_MIN      1
#define REQ_ID_MAX      65535

/*
 * Strict base-10 parse of [value, value_end): optional leading '-', digits
 * only, no partial/trailing garbage, result must fit in int32_t.
 * Returns 0 and writes *out on success, -1 otherwise.
 */
static int parse_int32_strict(const char *value, const char *value_end, int32_t *out)
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

	int64_t result = 0;

	for (; p < value_end; p++) {
		if (*p < '0' || *p > '9') {
			return -1;
		}

		result = result * 10 + (*p - '0');

		/* Bail out as soon as the magnitude can no longer fit, which
		 * also avoids int64_t overflow on long digit strings.
		 */
		if (result > (int64_t)INT32_MAX + 1) {
			return -1;
		}
	}

	if (negative) {
		result = -result;
	}

	if (result < INT32_MIN || result > INT32_MAX) {
		return -1;
	}

	*out = (int32_t)result;
	return 0;
}

static int parse_channel_value(const char *value, const char *value_end, uint8_t *out_channel)
{
	int32_t result;

	if (parse_int32_strict(value, value_end, &result) != 0 ||
	    result < CHANNEL_MIN || result > CHANNEL_MAX) {
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

/* ---- Multi-channel request ---------------------------------------------- */

enum slot_key {
	KEY_CHANNEL,
	KEY_START,
	KEY_END,
	KEY_FREQ,
	KEY_RANGE,
	KEY_COUNT,
};

static const char *const slot_key_names[KEY_COUNT] = {
	[KEY_CHANNEL] = "CHANNEL",
	[KEY_START]   = "START",
	[KEY_END]     = "END",
	[KEY_FREQ]    = "FREQ",
	[KEY_RANGE]   = "RANGE",
};

#define ALL_KEYS_SEEN ((1u << KEY_COUNT) - 1)

struct field {
	const char *key;
	size_t key_len;
	const char *value;
	const char *value_end;
};

/*
 * Advance *pos to the next ';'-delimited segment [*seg, *seg_end).
 * Returns false once the string is exhausted, so a single trailing ';'
 * is tolerated while an empty segment in the middle (";;") is not.
 */
static bool next_segment(const char **pos, const char **seg, const char **seg_end)
{
	if (**pos == '\0') {
		return false;
	}

	const char *semi = strchr(*pos, ';');

	*seg = *pos;
	*seg_end = (semi != NULL) ? semi : *pos + strlen(*pos);
	*pos = (semi != NULL) ? semi + 1 : *seg_end;

	return true;
}

static bool split_field(const char *seg, const char *seg_end, struct field *f)
{
	const char *eq = memchr(seg, '=', (size_t)(seg_end - seg));

	if (eq == NULL || eq == seg) {
		return false;
	}

	f->key = seg;
	f->key_len = (size_t)(eq - seg);
	f->value = eq + 1;
	f->value_end = seg_end;

	return true;
}

static bool key_is(const struct field *f, const char *name)
{
	return f->key_len == strlen(name) && strncmp(f->key, name, f->key_len) == 0;
}

static int find_slot_key(const struct field *f)
{
	for (int k = 0; k < KEY_COUNT; k++) {
		if (key_is(f, slot_key_names[k])) {
			return k;
		}
	}

	return -1;
}

static enum request_error fail(struct request_error_info *err, enum request_error code,
			       int slot, const char *key)
{
	err->code = code;
	err->slot = slot;
	err->key = (key != NULL) ? key : "?";

	return code;
}

/* First missing key of a slot in table order, or NULL if complete. */
static const char *first_missing_key(uint8_t seen)
{
	for (int k = 0; k < KEY_COUNT; k++) {
		if ((seen & (1u << k)) == 0) {
			return slot_key_names[k];
		}
	}

	return NULL;
}

bool is_multi_request(const char *cmd)
{
	const char *pos = cmd;
	const char *seg;
	const char *seg_end;

	while (next_segment(&pos, &seg, &seg_end)) {
		struct field f;

		if (split_field(seg, seg_end, &f) && key_is(&f, "REQ")) {
			return true;
		}
	}

	return false;
}

enum request_error parse_multi_request(const char *cmd, struct multi_request *out,
				       struct request_error_info *err)
{
	int32_t values[MULTI_MAX_SLOTS][KEY_COUNT];
	uint8_t seen[MULTI_MAX_SLOTS] = {0};
	bool have_id = false;
	bool have_n = false;
	int32_t id = 0;
	int32_t n = 0;
	int slot = -1; /* -1 while still in the header */
	const char *pos = cmd;
	const char *seg;
	const char *seg_end;

	/* Level 1: syntax, structure, keys. */
	while (next_segment(&pos, &seg, &seg_end)) {
		struct field f;
		int32_t v;

		if (!split_field(seg, seg_end, &f)) {
			return fail(err, REQUEST_ERR_SYNTAX, slot, NULL);
		}

		if (key_is(&f, "SLOT")) {
			if (!have_id || !have_n) {
				return fail(err, REQUEST_ERR_SYNTAX, -1, NULL);
			}
			if (parse_int32_strict(f.value, f.value_end, &v) != 0 || v != slot + 1) {
				return fail(err, REQUEST_ERR_SYNTAX, -1, NULL);
			}
			if (v >= n) {
				return fail(err, REQUEST_ERR_SLOT_COUNT, -1, NULL);
			}
			if (slot >= 0 && seen[slot] != ALL_KEYS_SEEN) {
				return fail(err, REQUEST_ERR_MISSING, slot,
					    first_missing_key(seen[slot]));
			}

			slot = v;
			continue;
		}

		if (key_is(&f, "REQ")) {
			if (slot >= 0 || have_id ||
			    parse_int32_strict(f.value, f.value_end, &id) != 0 ||
			    id < REQ_ID_MIN || id > REQ_ID_MAX) {
				return fail(err, REQUEST_ERR_SYNTAX, -1, NULL);
			}

			have_id = true;
			continue;
		}

		if (key_is(&f, "N")) {
			if (slot >= 0 || have_n ||
			    parse_int32_strict(f.value, f.value_end, &n) != 0) {
				return fail(err, REQUEST_ERR_SYNTAX, -1, NULL);
			}
			if (n < 1 || n > MULTI_MAX_SLOTS) {
				return fail(err, REQUEST_ERR_SLOT_COUNT, -1, NULL);
			}

			have_n = true;
			continue;
		}

		if (slot < 0) {
			/* Only REQ and N may precede the first SLOT. */
			return fail(err, REQUEST_ERR_SYNTAX, -1, NULL);
		}

		int k = find_slot_key(&f);

		if (k < 0) {
			return fail(err, REQUEST_ERR_UNKNOWN, slot, NULL);
		}
		if (seen[slot] & (1u << k)) {
			return fail(err, REQUEST_ERR_DUP, slot, slot_key_names[k]);
		}
		if (parse_int32_strict(f.value, f.value_end, &values[slot][k]) != 0) {
			return fail(err, REQUEST_ERR_SYNTAX, slot, slot_key_names[k]);
		}

		seen[slot] |= (uint8_t)(1u << k);
	}

	if (!have_id || !have_n) {
		return fail(err, REQUEST_ERR_SYNTAX, -1, NULL);
	}
	if (slot >= 0 && seen[slot] != ALL_KEYS_SEEN) {
		return fail(err, REQUEST_ERR_MISSING, slot, first_missing_key(seen[slot]));
	}
	if (slot + 1 != n) {
		return fail(err, REQUEST_ERR_SLOT_COUNT, -1, NULL);
	}

	/* Level 2: hardware-independent ranges. */
	for (int s = 0; s < n; s++) {
		if (values[s][KEY_CHANNEL] < CHANNEL_MIN || values[s][KEY_CHANNEL] > CHANNEL_MAX) {
			return fail(err, REQUEST_ERR_RANGE, s, slot_key_names[KEY_CHANNEL]);
		}
		if (values[s][KEY_FREQ] <= 0) {
			return fail(err, REQUEST_ERR_RANGE, s, slot_key_names[KEY_FREQ]);
		}
		if (values[s][KEY_RANGE] <= 0) {
			return fail(err, REQUEST_ERR_RANGE, s, slot_key_names[KEY_RANGE]);
		}
	}

	out->id = (uint16_t)id;
	out->n = (uint8_t)n;

	for (int s = 0; s < n; s++) {
		out->slots[s] = (struct slot_cfg){
			.channel  = (uint8_t)values[s][KEY_CHANNEL],
			.start_mv = values[s][KEY_START],
			.end_mv   = values[s][KEY_END],
			.freq_hz  = values[s][KEY_FREQ],
			.range_ua = values[s][KEY_RANGE],
		};
	}

	return REQUEST_OK;
}

const char *request_error_str(enum request_error code)
{
	switch (code) {
	case REQUEST_OK:             return "OK";
	case REQUEST_ERR_SYNTAX:     return "SYNTAX";
	case REQUEST_ERR_SLOT_COUNT: return "SLOT_COUNT";
	case REQUEST_ERR_MISSING:    return "MISSING";
	case REQUEST_ERR_DUP:        return "DUP";
	case REQUEST_ERR_UNKNOWN:    return "UNKNOWN";
	case REQUEST_ERR_RANGE:      return "RANGE";
	case REQUEST_ERR_HW:         return "HW";
	}

	return "?";
}
