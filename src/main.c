/*
 * Copyright (c) 2024 Croxel, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/services/nus.h>

#include <string.h>
#include <stdbool.h>

#define DEVICE_NAME     CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)

#define RX_BUF_SIZE             128
#define CMD_TERMINATOR          '#'

/*
 * Conservative BLE notification payload target.
 *
 * We intentionally keep every notification <= 20 bytes for now.
 * This avoids depending on MTU negotiation while the protocol is still being
 * developed and tested manually with nRF Connect.
 */
#define TRACE_NOTIFY_MAX_BYTES  20

/*
 * Delay between trace notifications.
 *
 * This avoids pushing multiple notifications back-to-back from inside the RX
 * callback. It also makes packet flow easier to observe in nRF Connect.
 */
#define TRACE_SEND_DELAY_MS     30

static char rx_buf[RX_BUF_SIZE];
static size_t rx_len = 0;

static const int16_t fake_trace[] = {
	-12, -10, -9, -4, 3, 12, 30, 18, 5, -1,
	-3, -8, -11, -6, 2, 9, 14, 8, 1, -2
};

static size_t trace_index = 0;
static struct bt_conn *trace_conn;
static bool trace_tx_busy = false;

enum trace_tx_phase {
	TRACE_TX_IDLE,
	TRACE_TX_BEGIN,
	TRACE_TX_DATA,
	TRACE_TX_END,
};

static enum trace_tx_phase trace_phase = TRACE_TX_IDLE;
static struct k_work_delayable trace_work;

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_NUS_SRV_VAL),
};

static int send_text_notification(struct bt_conn *conn, const char *text)
{
	int err = bt_nus_send(conn, text, strlen(text));

	printk("Notify send: \"%s\" result: %d\n", text, err);

	return err;
}

/**
 * @brief Build one trace data notification line.
 *
 * The output format is:
 *
 *   T=<value>,<value>,...\n
 *
 * The function appends as many trace values as fit into TRACE_NOTIFY_MAX_BYTES.
 * It returns how many values were added to the line.
 *
 * Example output:
 *
 *   T=-12,-10,-9\n
 *
 * @param out         Output buffer.
 * @param out_size    Output buffer size including space for '\0'.
 * @param start_index Index of the first fake_trace value to include.
 *
 * @return Number of trace values encoded into this chunk.
 */
static size_t build_trace_chunk(char *out, size_t out_size, size_t start_index)
{
	size_t pos = 0;
	size_t values_added = 0;

	if (out_size < TRACE_NOTIFY_MAX_BYTES + 1) {
		return 0;
	}

	pos += snprintk(out + pos, out_size - pos, "T=");

	for (size_t i = start_index; i < ARRAY_SIZE(fake_trace); i++) {
		char value_buf[12];
		int value_len;

		value_len = snprintk(value_buf, sizeof(value_buf),
				     "%s%d",
				     values_added == 0 ? "" : ",",
				     fake_trace[i]);

		if (value_len < 0) {
			return 0;
		}

		/*
		 * +1 reserves space for the trailing '\n'.
		 * The final '\0' is handled by the out buffer size.
		 */
		if ((pos + (size_t)value_len + 1) > TRACE_NOTIFY_MAX_BYTES) {
			break;
		}

		memcpy(out + pos, value_buf, value_len);
		pos += (size_t)value_len;
		values_added++;
	}

	if (values_added == 0) {
		return 0;
	}

	out[pos++] = '\n';
	out[pos] = '\0';

	return values_added;
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

	printk("Trace transfer finished\n");
}

/**
 * @brief Send the trace response as several small NUS notifications.
 *
 * This work handler implements a tiny response state machine:
 *
 *   TRACE_TX_BEGIN -> send "T_BEGIN\n"
 *   TRACE_TX_DATA  -> send multiple "T=...\n" chunks
 *   TRACE_TX_END   -> send "T_END\n"
 *
 * It runs outside the NUS RX callback so that receiving a command and sending
 * a longer response are not tightly coupled inside one callback invocation.
 */
static void trace_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	int err;

	if (trace_conn == NULL) {
		finish_trace_transfer();
		return;
	}

	switch (trace_phase) {
	case TRACE_TX_BEGIN:
		err = send_text_notification(trace_conn, "T_BEGIN\n");
		if (err < 0) {
			printk("Failed to send trace begin: %d\n", err);
			finish_trace_transfer();
			return;
		}

		trace_phase = TRACE_TX_DATA;
		k_work_schedule(&trace_work, K_MSEC(TRACE_SEND_DELAY_MS));
		break;

	case TRACE_TX_DATA:
		if (trace_index < ARRAY_SIZE(fake_trace)) {
			char line[TRACE_NOTIFY_MAX_BYTES + 1];
			size_t values_sent;

			values_sent = build_trace_chunk(line, sizeof(line), trace_index);
			if (values_sent == 0) {
				printk("Failed to build trace chunk\n");
				finish_trace_transfer();
				return;
			}

			err = send_text_notification(trace_conn, line);
			if (err < 0) {
				printk("Failed to send trace chunk: %d\n", err);
				finish_trace_transfer();
				return;
			}

			trace_index += values_sent;
			k_work_schedule(&trace_work, K_MSEC(TRACE_SEND_DELAY_MS));
		} else {
			trace_phase = TRACE_TX_END;
			k_work_schedule(&trace_work, K_NO_WAIT);
		}
		break;

	case TRACE_TX_END:
		err = send_text_notification(trace_conn, "T_END\n");
		if (err < 0) {
			printk("Failed to send trace end: %d\n", err);
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
 * Only one trace transfer is allowed at a time in this MVP. If a second command
 * arrives while a trace is still being sent, the firmware responds with
 * "ERR=BUSY\n".
 */
static void start_trace_transfer(struct bt_conn *conn)
{
	if (trace_tx_busy) {
		send_text_notification(conn, "ERR=BUSY\n");
		return;
	}

	trace_conn = bt_conn_ref(conn);
	trace_index = 0;
	trace_phase = TRACE_TX_BEGIN;
	trace_tx_busy = true;

	printk("Starting trace transfer\n");

	k_work_schedule(&trace_work, K_NO_WAIT);
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
 * Example:
 *
 *   Write 1: "START=-800;"
 *   Write 2: "END=0;"
 *   Write 3: "FREQ=100;"
 *   Write 4: "RANGE=10#"
 *
 * Reconstructed command:
 *
 *   "START=-800;END=0;FREQ=100;RANGE=10"
 *
 * Notification behavior:
 *
 * - Non-final fragment:
 *     "FRAG_OK\n"
 *
 * - Final fragment containing '#':
 *     multi-notification trace response:
 *       "T_BEGIN\n"
 *       "T=...\n"
 *       ...
 *       "T_END\n"
 *
 * - Buffer overflow:
 *     "ERR=RX_OVERFLOW\n"
 */
static void received(struct bt_conn *conn, const void *data, uint16_t len, void *ctx)
{
	const char *bytes = data;
	bool command_completed = false;

	ARG_UNUSED(ctx);

	printk("%s() - Len: %d, Fragment: %.*s\n",
	       __func__, len, len, bytes);

	for (uint16_t i = 0; i < len; i++) {
		char c = bytes[i];

		if (c == CMD_TERMINATOR) {
			rx_buf[rx_len] = '\0';

			printk("Full command: %s\n", rx_buf);

			start_trace_transfer(conn);

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

	printk("Sample - Bluetooth Peripheral NUS Fragmented Command Chunked Trace\n");

	k_work_init_delayable(&trace_work, trace_work_handler);

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
		k_sleep(K_FOREVER);
	}

	return 0;
}