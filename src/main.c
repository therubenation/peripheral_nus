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
#include <stdint.h>

#define DEVICE_NAME     CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)

#define RX_BUF_SIZE             128
#define CMD_TERMINATOR          '#'
#define TRACE_SEND_DELAY_MS     30

static char rx_buf[RX_BUF_SIZE];
static size_t rx_len = 0;

struct trace_point {
	int16_t voltage_mv;
	int16_t current_na;
};

/*
 * Fake CV-like trace data.
 *
 * Protocol meaning:
 * V = voltage in millivolts
 * I = current in nanoamps
 *
 * Each point will be sent as one notification:
 *
 *   P<index>;V=<voltage_mV>;I=<current_nA>
 */
static const struct trace_point fake_trace[] = {
	{ -800, -21 },
	{ -770, -18 },
	{ -740, -14 },
	{ -710, -10 },
	{ -680, -7  },
	{ -650, -5  },
	{ -620, -3  },
	{ -590, -2  },
	{ -560, -1  },
	{ -530,  0  },
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
static struct k_work_delayable adv_restart_work;

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_NUS_SRV_VAL),
};

static void finish_trace_transfer(void);

static void adv_restart_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1,
				   ad, ARRAY_SIZE(ad),
				   sd, ARRAY_SIZE(sd));
	if (err) {
		printk("Failed to restart advertising: %d\n", err);
	} else {
		printk("Advertising restarted\n");
	}
}

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

	k_work_schedule(&adv_restart_work, K_MSEC(100));
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

	printk("Trace transfer finished\n");
}

/**
 * @brief Send the trace response as several NUS notifications.
 *
 * Current value-pair protocol:
 *
 *   TB;N=<point_count>
 *   P<index>;V=<voltage_mV>;I=<current_nA>
 *   P<index>;V=<voltage_mV>;I=<current_nA>
 *   ...
 *   TE;N=<point_count>
 *
 * Example:
 *
 *   TB;N=3
 *   P0;V=-800;I=-21
 *   P1;V=-770;I=-18
 *   P2;V=-740;I=-14
 *   TE;N=3
 *
 * This work handler runs outside the NUS RX callback. That keeps command
 * reception and longer trace transmission structurally separated.
 */
static void trace_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	int err;
	char line[32];

	if (trace_conn == NULL) {
		finish_trace_transfer();
		return;
	}

	switch (trace_phase) {
	case TRACE_TX_BEGIN:
		snprintk(line, sizeof(line), "TB;N=%d\n", ARRAY_SIZE(fake_trace));

		err = send_text_notification(trace_conn, line);
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
			const struct trace_point *point = &fake_trace[trace_index];

			snprintk(line, sizeof(line),
				 "P%d;V=%d;I=%d\n",
				 trace_index,
				 point->voltage_mv,
				 point->current_na);

			err = send_text_notification(trace_conn, line);
			if (err < 0) {
				printk("Failed to send trace point %d: %d\n",
				       trace_index, err);
				finish_trace_transfer();
				return;
			}

			trace_index++;
			k_work_schedule(&trace_work, K_MSEC(TRACE_SEND_DELAY_MS));
		} else {
			trace_phase = TRACE_TX_END;
			k_work_schedule(&trace_work, K_NO_WAIT);
		}
		break;

	case TRACE_TX_END:
		snprintk(line, sizeof(line), "TE;N=%d\n", ARRAY_SIZE(fake_trace));

		err = send_text_notification(trace_conn, line);
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
 * Example app writes:
 *
 *   START=-800;
 *   END=0;
 *   FREQ=100;
 *   RANGE=10#
 *
 * Reconstructed command:
 *
 *   START=-800;END=0;FREQ=100;RANGE=10
 *
 * Notification behavior:
 *
 * - Non-final fragment:
 *     FRAG_OK
 *
 * - Final fragment containing '#':
 *     starts multi-notification trace response:
 *       TB;N=<point_count>
 *       P<index>;V=<voltage_mV>;I=<current_nA>
 *       ...
 *       TE;N=<point_count>
 *
 * - Buffer overflow:
 *     ERR=RX_OVERFLOW
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

	printk("Sample - Bluetooth Peripheral NUS Value-Pair Trace Protocol\n");

	k_work_init_delayable(&trace_work, trace_work_handler);
	k_work_init_delayable(&adv_restart_work, adv_restart_handler);

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