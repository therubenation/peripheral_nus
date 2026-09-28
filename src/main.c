/*
 * Copyright (c) 2024 Croxel, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/services/nus.h>

#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>

#include "cmd_parser.h"

#define DEVICE_NAME     CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)

#define RX_BUF_SIZE             512     /* fits a 6-slot multi request (~340 bytes) */
#define CMD_TERMINATOR          '#'
#define TRACE_SEND_DELAY_MS     60
#define TRACE_SEND_RETRIES      3
#define TRACE_X_SCALE           1000
#define TRACE_Y_SCALE           1000000

/* Simulated per-slot acquisition time of a multi-channel request. */
#define ACQ_SIM_MS              2000
/* Added to the computed procedure bound t sent in the R frame. */
#define PROC_T_SLACK_S          5

/*
 * Failure injection for testing X<slot>,ACQ_FAIL handling: the simulated
 * acquisition of any slot on this channel fails. -1 disables it. Override
 * at build time, e.g. west build ... -- -DEXTRA_CFLAGS=-DSIM_FAIL_CHANNEL=4
 */
#ifndef SIM_FAIL_CHANNEL
#define SIM_FAIL_CHANNEL        (-1)
#endif

static char rx_buf[RX_BUF_SIZE];
static size_t rx_len = 0;

struct trace_point {
	int32_t voltage_mv_scaled;   /* int32_t: values reach ±340040, overflows int16_t */
	int32_t current_na_scaled;   /* int32_t: values reach 830894, overflows int16_t */
};

/*
 * Real IV-curve measurement data (result_001_scaled.json).
 *
 * Values are scaled integers. To recover physical units:
 *   real_voltage_mV = voltage_mv_scaled / TRACE_X_SCALE
 *   real_current_nA = current_na_scaled / TRACE_Y_SCALE
 *
 * Each point will be sent as one notification:
 *
 *   <index>,<voltage_mv_scaled>,<current_na_scaled>
 */
static const struct trace_point trace_data[] = {
	{ -340040, 406027 }, { -330040, 385046 }, { -320041, 374079 },
	{ -310040, 387907 }, { -300040, 367403 }, { -290040, 371695 },
	{ -280040, 387430 }, { -270041, 413179 }, { -260040, 455618 },
	{ -250040, 509024 }, { -240040, 583887 }, { -230040, 652080 },
	{ -220040, 734572 }, { -210040, 804191 }, { -200118, 817542 },
	{ -190118, 830894 }, { -180118, 796561 }, { -170117, 740294 },
	{ -160118, 673537 }, { -150118, 592470 }, { -140118, 535727 },
	{ -130117, 481367 }, { -120117, 446558 }, { -110118, 417948 },
	{ -100118, 395060 }, { -90117,  385046 }, { -80117,  382185 },
	{ -70117,  378370 }, { -60196,  382185 }, { -50196,  377417 },
	{ -40195,  378370 }, { -30195,  379801 }, { -20195,  393152 },
	{ -10196,  395536 }, { -196,    403643 },
};

static size_t trace_index = 0;
static struct bt_conn *trace_conn;
static bool trace_tx_busy = false;

struct measurement_channel {
	bool present;
	uint8_t value;
};

static struct measurement_channel current_channel;

/*
 * Multi-channel procedure state. Unused (multi == false) for legacy
 * single-channel commands.
 */
struct procedure {
	bool multi;
	struct multi_request req;
	uint8_t slot;
	uint8_t ok;
	uint8_t failed;
};

static struct procedure proc;

/* Parsed in the NUS RX callback; copied into proc only when a transfer starts. */
static struct multi_request pending_req;

enum trace_tx_phase {
	TRACE_TX_IDLE,
	TRACE_TX_ACCEPT,     /* multi: R<id>,<n>,<t> */
	TRACE_TX_ACQUIRE,    /* multi: simulated acquisition of proc.slot finished */
	TRACE_TX_CHAN_HDR,   /* multi: C<slot>,<channel> */
	TRACE_TX_BEGIN,
	TRACE_TX_SCALE,
	TRACE_TX_DATA,
	TRACE_TX_END,
	TRACE_TX_SLOT_FAIL,  /* multi: X<slot>,ACQ_FAIL */
	TRACE_TX_DONE,       /* multi: D<id>,<ok>,<failed> */
};

static enum trace_tx_phase trace_phase = TRACE_TX_IDLE;
static struct k_work_delayable trace_work;
static uint8_t tx_retries;

/*
 * Signalled from on_disconnected() to wake the main loop for advertising restart.
 * Avoids calling bt_le_adv_start() from a BT callback or a work handler,
 * both of which previously caused MPU faults on this target.
 */
K_SEM_DEFINE(adv_restart_sem, 0, 1);

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_NUS_SRV_VAL),
};

static void finish_trace_transfer(void);

static void on_connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		printk("Connection failed: %u\n", err);
		return;
	}
	printk("Connected\n");
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	printk("Disconnected (reason %u)\n", reason);

	finish_trace_transfer();

	k_sem_give(&adv_restart_sem);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected    = on_connected,
	.disconnected = on_disconnected,
};

static int send_text_notification(struct bt_conn *conn, const char *text)
{
	int err = bt_nus_send(conn, text, strlen(text));

	printk("Notify send: \"%s\" result: %d\n", text, err);

	return err;
}

static void finish_trace_transfer(void)
{
	if (trace_conn != NULL) {
		bt_conn_unref(trace_conn);
		trace_conn = NULL;
	}

	trace_index = 0;
	trace_phase = TRACE_TX_IDLE;
	trace_tx_busy = false;
	tx_retries = 0;
	current_channel = (struct measurement_channel){0};
	proc = (struct procedure){0};

	printk("Trace transfer finished\n");
}

static void schedule_phase(enum trace_tx_phase phase, k_timeout_t delay)
{
	trace_phase = phase;
	k_work_schedule(&trace_work, delay);
}

/**
 * @brief Send one frame of the current phase, retrying on failure.
 *
 * On failure (typically -ENOMEM when the BLE TX buffer pool is exhausted),
 * the same phase is rescheduled with a growing back-off and the frame is
 * rebuilt and resent. After TRACE_SEND_RETRIES failures the link is dropped,
 * so the client observes a disconnect instead of silence.
 *
 * @return true if the frame was sent and the caller may advance the phase;
 *         false if the caller must return without changing any state.
 */
static bool send_frame(const char *line)
{
	int err = send_text_notification(trace_conn, line);

	if (err == 0) {
		tx_retries = 0;
		return true;
	}

	if (tx_retries < TRACE_SEND_RETRIES) {
		tx_retries++;
		printk("Send failed (%d), retry %u/%u\n", err, tx_retries, TRACE_SEND_RETRIES);
		k_work_schedule(&trace_work, K_MSEC(TRACE_SEND_DELAY_MS * (tx_retries + 1)));
		return false;
	}

	printk("Send failed (%d), giving up and disconnecting\n", err);
	bt_conn_disconnect(trace_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	finish_trace_transfer();
	return false;
}

/*
 * Upper bound in seconds on the time from R to D, sent as t in the R frame.
 * Per slot: simulated acquisition plus C, B, S, E and the data frames.
 */
static unsigned int procedure_bound_s(uint8_t n)
{
	uint32_t per_slot_ms = ACQ_SIM_MS +
			       (ARRAY_SIZE(trace_data) + 4) * TRACE_SEND_DELAY_MS;
	uint32_t total_ms = n * per_slot_ms + 2 * TRACE_SEND_DELAY_MS;

	return DIV_ROUND_UP(total_ms, 1000) + PROC_T_SLACK_S;
}

static bool slot_acquisition_fails(const struct slot_cfg *slot)
{
	return (int)slot->channel == SIM_FAIL_CHANNEL;
}

/* Move to the next slot's acquisition, or to D after the last slot. */
static void advance_slot(void)
{
	proc.slot++;
	trace_index = 0;

	if (proc.slot < proc.req.n) {
		printk("Acquiring slot %u (channel %u)\n",
		       proc.slot, proc.req.slots[proc.slot].channel);
		schedule_phase(TRACE_TX_ACQUIRE, K_MSEC(ACQ_SIM_MS));
	} else {
		schedule_phase(TRACE_TX_DONE, K_MSEC(TRACE_SEND_DELAY_MS));
	}
}

/**
 * @brief Send the trace response as several NUS notifications.
 *
 * Compact protocol (all frames ≤ 18 bytes, fits default BLE payload):
 *
 *   B<count>
 *   S<x_scale>,<y_scale>
 *   <index>,<voltage_mv_scaled>,<current_na_scaled>
 *   ...
 *   E<count>
 *
 * The S frame carries the scale factors needed to reconstruct physical units:
 *   real_voltage_mV = voltage_mv_scaled / x_scale
 *   real_current_nA = current_na_scaled / y_scale
 *
 * For a multi-channel request the same block is sent once per slot, preceded
 * by C<slot>,<channel>, and the whole procedure is wrapped in R ... D:
 *
 *   R<id>,<n>,<t>
 *   C<slot>,<channel>  B..E       (or X<slot>,ACQ_FAIL)   -- per slot
 *   D<id>,<ok>,<failed>
 *
 * The dummy firmware "acquires" a slot by waiting ACQ_SIM_MS and streams its
 * block right after, so it buffers at most one dataset. The contract also
 * allows deferring blocks; see docs/ble/BLE_CHUNKED_TRACE_PROTOCOL.md.
 *
 * This work handler runs outside the NUS RX callback. That keeps command
 * reception and longer trace transmission structurally separated.
 */
static void trace_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	char line[48];

	if (trace_conn == NULL) {
		finish_trace_transfer();
		return;
	}

	switch (trace_phase) {
	case TRACE_TX_ACCEPT:
		snprintk(line, sizeof(line), "R%u,%u,%u\n",
			 proc.req.id, proc.req.n, procedure_bound_s(proc.req.n));
		if (!send_frame(line)) {
			return;
		}

		printk("Acquiring slot 0 (channel %u)\n", proc.req.slots[0].channel);
		schedule_phase(TRACE_TX_ACQUIRE, K_MSEC(ACQ_SIM_MS));
		break;

	case TRACE_TX_ACQUIRE:
		if (slot_acquisition_fails(&proc.req.slots[proc.slot])) {
			schedule_phase(TRACE_TX_SLOT_FAIL, K_NO_WAIT);
		} else {
			schedule_phase(TRACE_TX_CHAN_HDR, K_NO_WAIT);
		}
		break;

	case TRACE_TX_CHAN_HDR:
		snprintk(line, sizeof(line), "C%u,%u\n",
			 proc.slot, proc.req.slots[proc.slot].channel);
		if (!send_frame(line)) {
			return;
		}

		schedule_phase(TRACE_TX_BEGIN, K_MSEC(TRACE_SEND_DELAY_MS));
		break;

	case TRACE_TX_BEGIN:
		if (proc.multi) {
			printk("Starting trace for slot %u\n", proc.slot);
		} else if (current_channel.present) {
			printk("Starting trace on channel %u\n", current_channel.value);
		} else {
			printk("Starting trace (no channel specified)\n");
		}

		snprintk(line, sizeof(line), "B%u\n",
			 (unsigned int)ARRAY_SIZE(trace_data));
		if (!send_frame(line)) {
			return;
		}

		schedule_phase(TRACE_TX_SCALE, K_MSEC(TRACE_SEND_DELAY_MS));
		break;

	case TRACE_TX_SCALE:
		snprintk(line, sizeof(line), "S%d,%d\n",
			 TRACE_X_SCALE,
			 TRACE_Y_SCALE);
		if (!send_frame(line)) {
			return;
		}

		schedule_phase(TRACE_TX_DATA, K_MSEC(TRACE_SEND_DELAY_MS));
		break;

	case TRACE_TX_DATA:
		if (trace_index < ARRAY_SIZE(trace_data)) {
			const struct trace_point *point = &trace_data[trace_index];

			snprintk(line, sizeof(line),
				 "%u,%d,%d\n",
				 (unsigned int)trace_index,
				 point->voltage_mv_scaled,
				 point->current_na_scaled);
			if (!send_frame(line)) {
				return;
			}

			trace_index++;
			k_work_schedule(&trace_work, K_MSEC(TRACE_SEND_DELAY_MS));
		} else {
			schedule_phase(TRACE_TX_END, K_NO_WAIT);
		}
		break;

	case TRACE_TX_END:
		snprintk(line, sizeof(line), "E%u\n",
			 (unsigned int)ARRAY_SIZE(trace_data));
		if (!send_frame(line)) {
			return;
		}

		if (!proc.multi) {
			finish_trace_transfer();
			break;
		}

		proc.ok++;
		advance_slot();
		break;

	case TRACE_TX_SLOT_FAIL:
		snprintk(line, sizeof(line), "X%u,ACQ_FAIL\n", proc.slot);
		if (!send_frame(line)) {
			return;
		}

		proc.failed++;
		advance_slot();
		break;

	case TRACE_TX_DONE:
		snprintk(line, sizeof(line), "D%u,%u,%u\n",
			 proc.req.id, proc.ok, proc.failed);
		if (!send_frame(line)) {
			return;
		}

		finish_trace_transfer();
		break;

	case TRACE_TX_IDLE:
	default:
		finish_trace_transfer();
		break;
	}
}

/**
 * @brief Start sending a multi-notification trace response.
 *
 * Only one trace transfer is allowed at a time in this MVP.
 * If another complete command arrives while a trace is still being sent,
 * firmware responds with ERR=BUSY.
 */
static void start_trace_transfer(struct bt_conn *conn)
{
	if (trace_tx_busy) {
		send_text_notification(conn, "ERR=BUSY\n");
		return;
	}

	trace_conn = bt_conn_ref(conn);
	trace_index = 0;
	trace_tx_busy = true;

	printk("Starting trace transfer\n");

	schedule_phase(TRACE_TX_BEGIN, K_NO_WAIT);
}

/*
 * Level-3 (hardware capability) validation hook, run on every slot before
 * the request is accepted. Returns the name of the first field the hardware
 * cannot honour, or NULL. The dummy firmware has no hardware limits yet.
 */
static const char *hw_check_slot(const struct slot_cfg *slot)
{
	ARG_UNUSED(slot);

	return NULL;
}

/**
 * @brief Validate a multi-channel request and start its procedure.
 *
 * Everything is validated before R is sent, so a rejected request never
 * acquires anything. Rejections are a single terminal frame:
 *
 *   ERR=<code>               request-level (BUSY, SYNTAX, SLOT_COUNT)
 *   V<slot>,<key>,<code>     slot-level (SYNTAX, MISSING, DUP, UNKNOWN, RANGE, HW)
 */
static void handle_multi_request(struct bt_conn *conn, const char *cmd)
{
	struct request_error_info err;
	char line[32];

	if (trace_tx_busy) {
		send_text_notification(conn, "ERR=BUSY\n");
		return;
	}

	enum request_error result = parse_multi_request(cmd, &pending_req, &err);

	for (uint8_t i = 0; result == REQUEST_OK && i < pending_req.n; i++) {
		const char *key = hw_check_slot(&pending_req.slots[i]);

		if (key != NULL) {
			result = REQUEST_ERR_HW;
			err = (struct request_error_info){
				.code = REQUEST_ERR_HW, .slot = i, .key = key,
			};
		}
	}

	if (result == REQUEST_OK) {
		trace_conn = bt_conn_ref(conn);
		trace_index = 0;
		trace_tx_busy = true;
		proc = (struct procedure){ .multi = true, .req = pending_req };

		printk("Starting multi procedure REQ=%u N=%u\n",
		       pending_req.id, pending_req.n);

		schedule_phase(TRACE_TX_ACCEPT, K_NO_WAIT);
		return;
	}

	if (err.slot < 0) {
		snprintk(line, sizeof(line), "ERR=%s\n", request_error_str(err.code));
	} else {
		snprintk(line, sizeof(line), "V%d,%s,%s\n",
			 err.slot, err.key, request_error_str(err.code));
	}

	printk("Rejected multi request\n");
	send_text_notification(conn, line);
}

static void notif_enabled(bool enabled, void *ctx)
{
	ARG_UNUSED(ctx);

	printk("%s() - %s\n", __func__, enabled ? "Enabled" : "Disabled");
}

/**
 * @brief Handle incoming NUS RX writes and reconstruct terminated commands.
 *
 * One BLE write is treated as one fragment, not necessarily as one complete
 * command. Fragments are appended to rx_buf until CMD_TERMINATOR is received.
 *
 * Current manual-test terminator:
 *
 *   '#'
 *
 * Example app writes:
 *
 *   START=-800;
 *   END=0;
 *   FREQ=100;
 *   RANGE=10;
 *   CHANNEL=7#
 *
 * Reconstructed command:
 *
 *   START=-800;END=0;FREQ=100;RANGE=10;CHANNEL=7
 *
 * START, END, FREQ, and RANGE are reconstructed but not yet parsed or
 * validated. Field units (for when parsing is implemented): START/END in
 * mV, FREQ in Hz, RANGE in µA (changed from nA — do not confuse with the
 * unrelated nA unit used for measured trace-output current, see
 * trace_point below). CHANNEL is parsed and validated: it must be an integer in
 * 0..255, and it is optional (its absence is not an error and is not
 * treated as channel 0). See cmd_parser.h.
 *
 * Notification behavior:
 *
 * - Non-final fragment:
 *     FRAG_OK
 *
 * - Final fragment containing '#', with a valid or absent CHANNEL:
 *     starts multi-notification trace response
 *
 * - Final fragment containing '#', with an invalid CHANNEL value:
 *     ERR=CHANNEL_INVALID
 *
 * - Buffer overflow:
 *     ERR=RX_OVERFLOW
 *
 * A command containing a REQ= field is a multi-channel request instead
 * (REQ=<id>;N=<n>;SLOT=0;CHANNEL=..;START=..;END=..;FREQ=..;RANGE=..;SLOT=1;...),
 * handled by handle_multi_request(). The fragment/FRAG_OK/'#' handling is
 * the same for both; fragment boundaries are arbitrary.
 */
static void received(struct bt_conn *conn, const void *data, uint16_t len, void *ctx)
{
	const char *bytes = data;
	bool command_completed = false;

	ARG_UNUSED(ctx);

	printk("%s() - Len: %u, Fragment: %.*s\n",
	       __func__, (unsigned int)len, (int)len, bytes);

	for (uint16_t i = 0; i < len; i++) {
		char c = bytes[i];

		if (c == CMD_TERMINATOR) {
			uint8_t channel = 0;
			enum channel_parse_result channel_result;

			rx_buf[rx_len] = '\0';

			printk("Full command: %s\n", rx_buf);

			if (is_multi_request(rx_buf)) {
				handle_multi_request(conn, rx_buf);
				rx_len = 0;
				command_completed = true;
				continue;
			}

			channel_result = parse_channel_field(rx_buf, &channel);

			if (channel_result == CHANNEL_FIELD_INVALID) {
				printk("Rejected command: invalid CHANNEL\n");
				send_text_notification(conn, "ERR=CHANNEL_INVALID\n");
			} else {
				current_channel.present = (channel_result == CHANNEL_FIELD_VALID);
				current_channel.value = channel;

				start_trace_transfer(conn);
			}

			rx_len = 0;
			command_completed = true;
		} else {
			if (rx_len < sizeof(rx_buf) - 1) {
				rx_buf[rx_len++] = c;
			} else {
				printk("RX buffer overflow, clearing\n");

				rx_len = 0;
				command_completed = true;

				send_text_notification(conn, "ERR=RX_OVERFLOW\n");
			}
		}
	}

	if (!command_completed) {
		send_text_notification(conn, "FRAG_OK\n");
	}
}

static struct bt_nus_cb nus_listener = {
	.notif_enabled = notif_enabled,
	.received = received,
};

int main(void)
{
	int err;

	printk("Sample - Bluetooth Peripheral NUS Value-Pair Trace Protocol\n");

	k_work_init_delayable(&trace_work, trace_work_handler);

	/* Register NUS callbacks before bt_enable — safe because bt_nus_cb_register
	 * only appends to a statically-initialized slist and has no BT stack dependency.
	 * This matches the official Zephyr peripheral_nus sample pattern. */
	err = bt_nus_cb_register(&nus_listener, NULL);
	if (err) {
		printk("Failed to register NUS callback: %d\n", err);
		return err;
	}

	err = bt_enable(NULL);
	if (err) {
		printk("Failed to enable Bluetooth: %d\n", err);
		return err;
	}

	err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1,
			      ad, ARRAY_SIZE(ad),
			      sd, ARRAY_SIZE(sd));
	if (err) {
		printk("Failed to start advertising: %d\n", err);
		return err;
	}

	printk("Initialization complete\n");

	while (true) {
		k_sem_take(&adv_restart_sem, K_FOREVER);

		k_sleep(K_MSEC(200));

		err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1,
				      ad, ARRAY_SIZE(ad),
				      sd, ARRAY_SIZE(sd));
		if (err == -EALREADY) {
			printk("Advertising already active\n");
		} else if (err) {
			printk("Failed to restart advertising: %d\n", err);
		} else {
			printk("Advertising restarted\n");
		}
	}

	return 0;
}
