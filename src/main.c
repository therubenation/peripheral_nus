/*
 * Copyright (c) 2024 Croxel, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/services/nus.h>

#include <string.h>
#include <stdbool.h>

#define DEVICE_NAME     CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)

static char rx_buf[128];
static size_t rx_len = 0;

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_NUS_SRV_VAL),
};

static void notif_enabled(bool enabled, void *ctx)
{
	ARG_UNUSED(ctx);

	printk("%s() - %s\n", __func__, enabled ? "Enabled" : "Disabled");
}

/**
 * @brief Handle incoming NUS RX writes and reconstruct newline-terminated commands.
 *
 * One BLE write is treated as one fragment, not necessarily as one complete
 * command. Fragments are appended to rx_buf until a newline character ('\n')
 * is received. The newline marks the end of one logical command.
 *
 * Example:
 *   Write 1: "START=-800;"
 *   Write 2: "END=0;"
 *   Write 3: "FREQ=100;"
 *   Write 4: "RANGE=10\n"
 *
 * Reconstructed command:
 *   "START=-800;END=0;FREQ=100;RANGE=10"
 *
 * Notification behavior:
 * - Non-final fragment:
 *     "FRAG_OK\n"
 * - Final fragment containing '\n':
 *     "TRACE=1,2,3\n"
 * - Buffer overflow:
 *     "ERR=RX_OVERFLOW\n"
 *
 * Safety notes:
 * - Incoming BLE data is not assumed to be null-terminated.
 * - `%.*s` is used to print exactly len bytes.
 * - rx_buf reserves one byte for the final '\0'.
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

		if (c == '#') {
			rx_buf[rx_len] = '\0';

			printk("Full command: %s\n", rx_buf);

			const char *response = "TRACE=1,2,3\n";
			int err = bt_nus_send(conn, response, strlen(response));
			printk("Mini trace send - Result: %d\n", err);

			rx_len = 0;
			command_completed = true;
		} else {
			if (rx_len < sizeof(rx_buf) - 1) {
				rx_buf[rx_len++] = c;
			} else {
				printk("RX buffer overflow, clearing\n");

				rx_len = 0;
				command_completed = true;

				const char *error = "ERR=RX_OVERFLOW\n";
				int err = bt_nus_send(conn, error, strlen(error));
				printk("Overflow error send - Result: %d\n", err);
			}
		}
	}

	if (!command_completed) {
		const char *frag_ok = "FRAG_OK\n";
		int err = bt_nus_send(conn, frag_ok, strlen(frag_ok));
		printk("Fragment ACK send - Result: %d\n", err);
	}
}

static struct bt_nus_cb nus_listener = {
	.notif_enabled = notif_enabled,
	.received = received,
};

int main(void)
{
	int err;

	printk("Sample - Bluetooth Peripheral NUS Fragmented Command Mini Trace\n");

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