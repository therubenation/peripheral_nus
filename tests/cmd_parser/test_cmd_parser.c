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
#include <string.h>

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

#define SLOT0 "SLOT=0;CHANNEL=1;START=-800;END=0;FREQ=100;RANGE=10;"
#define SLOT1 "SLOT=1;CHANNEL=4;START=-600;END=-100;FREQ=50;RANGE=5;"
#define SLOT2 "SLOT=2;CHANNEL=7;START=-400;END=0;FREQ=25;RANGE=1;"

static void expect_request_ok(const char *cmd, int expected_n)
{
	struct multi_request req;
	struct request_error_info err = {0};
	enum request_error result = parse_multi_request(cmd, &req, &err);

	CHECK(is_multi_request(cmd));
	CHECK(result == REQUEST_OK);
	if (result != REQUEST_OK) {
		printf("  -> %s: got %s slot %d key %s\n", cmd,
		       request_error_str(err.code), err.slot, err.key);
	}
	CHECK(req.n == expected_n);
}

/* expected_slot -1 means a request-level ERR=; key NULL skips the key check. */
static void expect_request_err(const char *cmd, enum request_error expected,
			       int expected_slot, const char *expected_key)
{
	struct multi_request req;
	struct request_error_info err = {0};
	enum request_error result = parse_multi_request(cmd, &req, &err);

	CHECK(result == expected);
	CHECK(err.code == expected);
	CHECK(err.slot == expected_slot);
	if (expected_key != NULL) {
		CHECK(err.key != NULL && strcmp(err.key, expected_key) == 0);
	}
	if (result != expected || err.slot != expected_slot) {
		printf("  -> %s: got %s slot %d key %s\n", cmd,
		       request_error_str(err.code), err.slot, err.key);
	}
}

static void test_multi_request(void)
{
	struct multi_request req;
	struct request_error_info err = {0};

	/* Detection: REQ= field marks a multi request; legacy commands don't have it */
	CHECK(is_multi_request("REQ=1;N=1;" SLOT0));
	CHECK(!is_multi_request("START=-800;END=0;FREQ=100;RANGE=10;CHANNEL=7"));
	CHECK(!is_multi_request(""));
	CHECK(!is_multi_request("MYREQ=1;"));

	/* Valid requests, values carried through */
	CHECK(parse_multi_request("REQ=42;N=3;" SLOT0 SLOT1 SLOT2, &req, &err) == REQUEST_OK);
	CHECK(req.id == 42);
	CHECK(req.n == 3);
	CHECK(req.slots[0].channel == 1);
	CHECK(req.slots[0].start_mv == -800);
	CHECK(req.slots[1].channel == 4);
	CHECK(req.slots[1].end_mv == -100);
	CHECK(req.slots[1].freq_hz == 50);
	CHECK(req.slots[2].channel == 7);
	CHECK(req.slots[2].range_ua == 1);

	/* N boundaries, key order free within a slot, no trailing ';' */
	expect_request_ok("REQ=1;N=1;SLOT=0;RANGE=10;FREQ=100;END=0;START=-800;CHANNEL=0", 1);
	expect_request_ok("REQ=65535;N=6;"
			  "SLOT=0;CHANNEL=0;START=-800;END=0;FREQ=100;RANGE=10;"
			  "SLOT=1;CHANNEL=1;START=-800;END=0;FREQ=100;RANGE=10;"
			  "SLOT=2;CHANNEL=2;START=-800;END=0;FREQ=100;RANGE=10;"
			  "SLOT=3;CHANNEL=3;START=-800;END=0;FREQ=100;RANGE=10;"
			  "SLOT=4;CHANNEL=4;START=-800;END=0;FREQ=100;RANGE=10;"
			  "SLOT=5;CHANNEL=255;START=-800;END=0;FREQ=100;RANGE=10;", 6);

	/* Header order may vary; duplicate channels are allowed by the contract */
	expect_request_ok("N=2;REQ=7;SLOT=0;CHANNEL=3;START=-800;END=0;FREQ=100;RANGE=10;"
			  "SLOT=1;CHANNEL=3;START=-800;END=0;FREQ=100;RANGE=10;", 2);

	/* Voltage sign/direction are not checked here (hardware-dependent) */
	expect_request_ok("REQ=1;N=1;SLOT=0;CHANNEL=1;START=500;END=-500;FREQ=1;RANGE=1;", 1);

	/* Level 1, request-level (ERR=) */
	expect_request_err("REQ=1;" SLOT0, REQUEST_ERR_SYNTAX, -1, NULL);          /* N missing */
	expect_request_err("N=1;" SLOT0 "REQ=1;", REQUEST_ERR_SYNTAX, -1, NULL);   /* REQ after SLOT */
	expect_request_err("REQ=0;N=1;" SLOT0, REQUEST_ERR_SYNTAX, -1, NULL);      /* REQ range */
	expect_request_err("REQ=65536;N=1;" SLOT0, REQUEST_ERR_SYNTAX, -1, NULL);
	expect_request_err("REQ=x;N=1;" SLOT0, REQUEST_ERR_SYNTAX, -1, NULL);
	expect_request_err("REQ=1;REQ=2;N=1;" SLOT0, REQUEST_ERR_SYNTAX, -1, NULL);
	expect_request_err("REQ=1;N=1;N=1;" SLOT0, REQUEST_ERR_SYNTAX, -1, NULL);
	expect_request_err("REQ=1;N=1;CHANNEL=1;" SLOT0, REQUEST_ERR_SYNTAX, -1, NULL);
	expect_request_err("REQ=1;N=2;" SLOT1 SLOT0, REQUEST_ERR_SYNTAX, -1, NULL); /* order */
	expect_request_err("REQ=1;N=3;" SLOT0 SLOT2, REQUEST_ERR_SYNTAX, -1, NULL); /* gap */
	expect_request_err("REQ=1;N=1;" SLOT0 SLOT0, REQUEST_ERR_SYNTAX, -1, NULL); /* repeat */
	expect_request_err("REQ=1;N=1;;" SLOT0, REQUEST_ERR_SYNTAX, -1, NULL);      /* empty */
	expect_request_err("REQ=1;N=0;", REQUEST_ERR_SLOT_COUNT, -1, NULL);
	expect_request_err("REQ=1;N=7;" SLOT0, REQUEST_ERR_SLOT_COUNT, -1, NULL);
	expect_request_err("REQ=1;N=2;" SLOT0, REQUEST_ERR_SLOT_COUNT, -1, NULL);  /* too few */
	expect_request_err("REQ=1;N=1;" SLOT0 SLOT1, REQUEST_ERR_SLOT_COUNT, -1, NULL); /* too many */
	expect_request_err("REQ=1;N=1;", REQUEST_ERR_SLOT_COUNT, -1, NULL);        /* no slots */

	/* Level 1, slot-level (V<slot>,<key>,<code>) */
	expect_request_err("REQ=1;N=1;SLOT=0;START=-800;END=0;FREQ=100;RANGE=10;",
			   REQUEST_ERR_MISSING, 0, "CHANNEL");
	expect_request_err("REQ=1;N=1;SLOT=0;CHANNEL=1;END=0;FREQ=100;RANGE=10;",
			   REQUEST_ERR_MISSING, 0, "START");
	expect_request_err("REQ=1;N=1;SLOT=0;CHANNEL=1;START=-800;FREQ=100;RANGE=10;",
			   REQUEST_ERR_MISSING, 0, "END");
	expect_request_err("REQ=1;N=1;SLOT=0;CHANNEL=1;START=-800;END=0;RANGE=10;",
			   REQUEST_ERR_MISSING, 0, "FREQ");
	expect_request_err("REQ=1;N=1;SLOT=0;CHANNEL=1;START=-800;END=0;FREQ=100;",
			   REQUEST_ERR_MISSING, 0, "RANGE");
	expect_request_err("REQ=1;N=2;SLOT=0;CHANNEL=1;START=-800;END=0;FREQ=100;" SLOT1,
			   REQUEST_ERR_MISSING, 0, "RANGE");                    /* not last slot */
	expect_request_err("REQ=1;N=2;" SLOT0 "SLOT=1;CHANNEL=1;CHANNEL=2;START=-800;END=0;"
			   "FREQ=100;RANGE=10;", REQUEST_ERR_DUP, 1, "CHANNEL");
	expect_request_err("REQ=1;N=1;" SLOT0 "TEMP=37;", REQUEST_ERR_UNKNOWN, 0, "?");
	expect_request_err("REQ=1;N=1;" SLOT0 "REQ=2;", REQUEST_ERR_SYNTAX, -1, NULL);
	expect_request_err("REQ=1;N=1;SLOT=0;CHANNEL=1;START=-8x;END=0;FREQ=100;RANGE=10;",
			   REQUEST_ERR_SYNTAX, 0, "START");
	expect_request_err("REQ=1;N=1;SLOT=0;CHANNEL=1;START=-800;END=;FREQ=100;RANGE=10;",
			   REQUEST_ERR_SYNTAX, 0, "END");
	expect_request_err("REQ=1;N=1;SLOT=0;CHANNEL=1;START=+800;END=0;FREQ=100;RANGE=10;",
			   REQUEST_ERR_SYNTAX, 0, "START");
	expect_request_err("REQ=1;N=1;SLOT=0;CHANNEL=1;START=2147483648;END=0;FREQ=100;RANGE=10;",
			   REQUEST_ERR_SYNTAX, 0, "START");                     /* int32 overflow */
	expect_request_err("REQ=1;N=1;" SLOT0 "garbage;", REQUEST_ERR_SYNTAX, 0, "?");

	/* Level 2, slot-level range */
	expect_request_err("REQ=1;N=1;SLOT=0;CHANNEL=256;START=-800;END=0;FREQ=100;RANGE=10;",
			   REQUEST_ERR_RANGE, 0, "CHANNEL");
	expect_request_err("REQ=1;N=1;SLOT=0;CHANNEL=-1;START=-800;END=0;FREQ=100;RANGE=10;",
			   REQUEST_ERR_RANGE, 0, "CHANNEL");
	expect_request_err("REQ=1;N=2;" SLOT0 "SLOT=1;CHANNEL=4;START=-600;END=-100;FREQ=0;RANGE=5;",
			   REQUEST_ERR_RANGE, 1, "FREQ");
	expect_request_err("REQ=1;N=1;SLOT=0;CHANNEL=1;START=-800;END=0;FREQ=100;RANGE=-3;",
			   REQUEST_ERR_RANGE, 0, "RANGE");

	/* Level 1 errors take precedence over level 2, even in a later slot */
	expect_request_err("REQ=1;N=2;SLOT=0;CHANNEL=999;START=-800;END=0;FREQ=100;RANGE=10;"
			   "SLOT=1;CHANNEL=4;START=-600;END=-100;FREQ=50;",
			   REQUEST_ERR_MISSING, 1, "RANGE");
}

int main(void)
{
	test_multi_request();

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
