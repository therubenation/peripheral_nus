/*
 * Copyright (c) 2024 Croxel, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host-compiled unit test for cmd_parser.c. No Zephyr toolchain needed:
 *
 *   cc -I../../src -o /tmp/test_cmd_parser test_cmd_parser.c ../../src/cmd_parser.c
 *   /tmp/test_cmd_parser
 */

#include "cmd_parser.h"

#include <assert.h>
#include <stdio.h>

static int failures;

#define CHECK(cond)                                                                   \
	do {                                                                           \
		if (!(cond)) {                                                        \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
			failures++;                                                   \
		}                                                                      \
	} while (0)

static void expect_valid(const char *cmd, uint8_t expected)
{
	uint8_t channel = 0xFF;
	enum channel_parse_result result = parse_channel_field(cmd, &channel);

	CHECK(result == CHANNEL_FIELD_VALID);
	CHECK(channel == expected);
}

static void expect_invalid(const char *cmd)
{
	uint8_t channel = 0xFF;
	enum channel_parse_result result = parse_channel_field(cmd, &channel);

	CHECK(result == CHANNEL_FIELD_INVALID);
}

static void expect_absent(const char *cmd)
{
	uint8_t channel = 0xFF;
	enum channel_parse_result result = parse_channel_field(cmd, &channel);

	CHECK(result == CHANNEL_FIELD_ABSENT);
}

int main(void)
{
	/* Boundaries */
	expect_valid("CHANNEL=0;", 0);
	expect_valid("CHANNEL=255;", 255);

	/* Out of range */
	expect_invalid("CHANNEL=-1;");
	expect_invalid("CHANNEL=256;");

	/* Non-numeric / malformed */
	expect_invalid("CHANNEL=foo;");
	expect_invalid("CHANNEL=7x;");
	expect_invalid("CHANNEL=;");

	/* Missing field entirely */
	expect_absent("");
	expect_absent("START=-800;END=0;FREQ=100;RANGE=10");

	/* CHANNEL alongside other fields, in the confirmed contract order */
	expect_valid("START=-800;END=0;FREQ=100;RANGE=10;CHANNEL=7;", 7);

	/* No position dependency: CHANNEL first still works */
	expect_valid("CHANNEL=7;START=-800;END=0;FREQ=100;RANGE=10;", 7);

	/* No trailing ';' on the last field (matches how rx_buf reconstructs
	 * a command with no ';' right before the '#' terminator).
	 */
	expect_valid("START=-800;END=0;FREQ=100;RANGE=10;CHANNEL=7", 7);

	/* No false-positive substring match against a similarly-named field */
	expect_absent("MYCHANNEL=5;");

	/* Duplicate CHANNEL= fields: first occurrence wins */
	expect_valid("CHANNEL=7;CHANNEL=9;", 7);

	/* Leading zeros are accepted */
	expect_valid("CHANNEL=007;", 7);

	if (failures == 0) {
		printf("All tests passed.\n");
		return 0;
	}

	printf("%d test(s) failed.\n", failures);
	return 1;
}
