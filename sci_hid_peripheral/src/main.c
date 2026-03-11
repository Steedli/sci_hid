/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <string.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <bluetooth/services/hids.h>
#include <zephyr/drivers/gpio.h>

LOG_MODULE_REGISTER(app_main, LOG_LEVEL_INF);

#define DEVICE_NAME     CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)

#define INTERVAL_INITIAL_US  10000 /* 10 ms */

/** @brief UUID of the SCI Min Interval Service. **/
#define BT_UUID_VS_SCI_MIN_INTERVAL_SERVICE_VAL \
	BT_UUID_128_ENCODE(0x1c840001, 0x49ac, 0x4905, 0x9702, 0x6e836da4cadd)

#define BT_UUID_VS_SCI_MIN_INTERVAL_SERVICE \
	BT_UUID_DECLARE_128(BT_UUID_VS_SCI_MIN_INTERVAL_SERVICE_VAL)

/** @brief UUID of the SCI Min Interval Characteristic. **/
#define BT_UUID_VS_SCI_MIN_INTERVAL_CHAR_VAL \
	BT_UUID_128_ENCODE(0x1c840002, 0x49ac, 0x4905, 0x9702, 0x6e836da4cadd)

#define BT_UUID_VS_SCI_MIN_INTERVAL_CHAR \
	BT_UUID_DECLARE_128(BT_UUID_VS_SCI_MIN_INTERVAL_CHAR_VAL)

/* Simple 3-byte mouse HID report descriptor (no report ID):
 * Byte 0: buttons  [3 bits used + 5 padding]
 * Byte 1: X        [-127..127]
 * Byte 2: Y        [-127..127]
 */
static const uint8_t mouse_report_map[] = {
	0x05, 0x01,     /* Usage Page (Generic Desktop) */
	0x09, 0x02,     /* Usage (Mouse) */
	0xA1, 0x01,     /* Collection (Application) */
	0x09, 0x01,     /* Usage (Pointer) */
	0xA1, 0x00,     /* Collection (Physical) */
	/* 3 buttons */
	0x05, 0x09,     /* Usage Page (Buttons) */
	0x19, 0x01,     /* Usage Minimum (01) */
	0x29, 0x03,     /* Usage Maximum (03) */
	0x15, 0x00,     /* Logical Minimum (0) */
	0x25, 0x01,     /* Logical Maximum (1) */
	0x75, 0x01,     /* Report Size (1) */
	0x95, 0x03,     /* Report Count (3) */
	0x81, 0x02,     /* Input (Data, Variable, Absolute) */
	/* 5-bit padding */
	0x75, 0x05,     /* Report Size (5) */
	0x95, 0x01,     /* Report Count (1) */
	0x81, 0x01,     /* Input (Constant) */
	/* X, Y relative movement */
	0x05, 0x01,     /* Usage Page (Generic Desktop) */
	0x09, 0x30,     /* Usage (X) */
	0x09, 0x31,     /* Usage (Y) */
	0x15, 0x81,     /* Logical Minimum (-127) */
	0x25, 0x7F,     /* Logical Maximum (127) */
	0x75, 0x08,     /* Report Size (8) */
	0x95, 0x02,     /* Report Count (2) */
	0x81, 0x06,     /* Input (Data, Variable, Relative) */
	0xC0,           /* End Collection (Physical) */
	0xC0,           /* End Collection (Application) */
};

#define MOUSE_REPORT_SIZE 3

/* 16-step circular movement table (radius ~10 pixels) */
static const int8_t circle_dx[16] = {-1, -2, -3, -4, -4, -3, -2, -1,
				       1,  2,  3,  4,  4,  3,  2,  1};
static const int8_t circle_dy[16] = { 4,  3,  2,  1, -1, -2, -3, -4,
				      -4, -3, -2, -1,  1,  2,  3,  4};

/* HIDS instance - one 3-byte input report */
BT_HIDS_DEF(hids_obj, MOUSE_REPORT_SIZE);

static const struct gpio_dt_spec button0 = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static struct gpio_callback button0_cb_data;
static bool circle_running;

static struct bt_conn *default_conn;
static uint16_t local_min_interval_us;
static bool hid_notify_enabled;
static uint8_t circle_idx;

static K_SEM_DEFINE(phy_updated, 0, 1);
static K_SEM_DEFINE(frame_space_updated_sem, 0, 1);

/* Advertising data: HIDS UUID (16-bit) so PC/central can discover a HID device.
 * GAP Appearance = Mouse (0x03C2). SCI 128-bit UUID goes in scan response. */
static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_GAP_APPEARANCE,
		      (CONFIG_BT_DEVICE_APPEARANCE >> 0) & 0xff,
		      (CONFIG_BT_DEVICE_APPEARANCE >> 8) & 0xff),
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL, BT_UUID_16_ENCODE(BT_UUID_HIDS_VAL)),
};

static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
	// BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_VS_SCI_MIN_INTERVAL_SERVICE_VAL),
};

/* SCI service: expose local minimum interval to the peer */

static ssize_t read_min_interval(struct bt_conn *conn,
				 const struct bt_gatt_attr *attr,
				 void *buf, uint16_t len, uint16_t offset)
{
	uint16_t value = sys_cpu_to_le16(local_min_interval_us);

	return bt_gatt_attr_read(conn, attr, buf, len, offset,
				 &value, sizeof(value));
}

BT_GATT_SERVICE_DEFINE(sci_min_interval_svc,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_VS_SCI_MIN_INTERVAL_SERVICE),
	BT_GATT_CHARACTERISTIC(BT_UUID_VS_SCI_MIN_INTERVAL_CHAR,
			       BT_GATT_CHRC_READ, BT_GATT_PERM_READ,
			       read_min_interval, NULL, NULL));

/* ???? HID service callbacks ???????????????????????????????????????????????????????????????????????????????????????????????? */

static void hids_notify_handler(enum bt_hids_notify_evt evt)
{
	hid_notify_enabled = (evt == BT_HIDS_CCCD_EVT_NOTIFY_ENABLED);
	LOG_INF("HID notification %s",
		hid_notify_enabled ? "enabled" : "disabled");
}

static void hids_pm_evt_handler(enum bt_hids_pm_evt evt, struct bt_conn *conn)
{
	LOG_DBG("HID protocol mode event: %d", evt);
}

static void hid_init(void)
{
	int err;
	struct bt_hids_init_param hids_init_param = { 0 };
	struct bt_hids_inp_rep *hids_inp_rep;

	hids_init_param.rep_map.data = mouse_report_map;
	hids_init_param.rep_map.size = sizeof(mouse_report_map);

	hids_init_param.info.bcd_hid = 0x0101;
	hids_init_param.info.b_country_code = 0x00;
	hids_init_param.info.flags = BT_HIDS_NORMALLY_CONNECTABLE;

	hids_inp_rep = &hids_init_param.inp_rep_group_init.reports[0];
	hids_inp_rep->size    = MOUSE_REPORT_SIZE;
	hids_inp_rep->id      = 0;   /* No report ID */
	hids_inp_rep->handler = hids_notify_handler;
	hids_init_param.inp_rep_group_init.cnt = 1;

	hids_init_param.is_mouse = true;
	hids_init_param.pm_evt_handler = hids_pm_evt_handler;

	err = bt_hids_init(&hids_obj, &hids_init_param);
	if (err) {
		LOG_ERR("HIDS init failed: %d", err);
	}
}

/* ???? Mouse circle generator ?????????????????????????????????????????????????????????????????????????????????????????????? */

static void mouse_work_handler(struct k_work *work);
static K_WORK_DEFINE(mouse_work, mouse_work_handler);

static void mouse_timer_handler(struct k_timer *timer)
{
	k_work_submit(&mouse_work);
}

static K_TIMER_DEFINE(mouse_timer, mouse_timer_handler, NULL);

static void button0_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	circle_running = !circle_running;
	LOG_INF("Circle %s", circle_running ? "started" : "stopped");
}

static void mouse_work_handler(struct k_work *work)
{
	if (!default_conn || !hid_notify_enabled || !circle_running) {
		return;
	}

	uint8_t report[MOUSE_REPORT_SIZE] = {
		0,                              /* buttons (none pressed) */
		(uint8_t)circle_dx[circle_idx],
		(uint8_t)circle_dy[circle_idx],
	};

	circle_idx = (circle_idx + 1) % ARRAY_SIZE(circle_dx);

	int err = bt_hids_inp_rep_send(&hids_obj, default_conn, 0,
				       report, sizeof(report), NULL);
	if (err && err != -ENOTCONN && err != -EAGAIN) {
		LOG_WRN("HID send failed: %d", err);
	}
}

/* ???? Advertising ???????????????????????????????????????????????????????????????????????????????????????????????????????????????????? */

static void adv_start(void)
{
	int err;

	err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, ad, ARRAY_SIZE(ad),
			      sd, ARRAY_SIZE(sd));
	if (err) {
		LOG_ERR("Advertising failed to start (err %d)", err);
		return;
	}

	LOG_INF("Advertising started");
}

/* ???? BT connection callbacks ???????????????????????????????????????????????????????????????????????????????????????????? */

static void connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (err) {
		LOG_WRN("Connection to %s failed (err 0x%02x)", addr, err);
		return;
	}

	LOG_INF("Connected: %s", addr);

	default_conn = bt_conn_ref(conn);
	hid_notify_enabled = false;

	bt_hids_connected(&hids_obj, conn);

	/* Begin generating mouse circle data every 750 us (matches SCI interval) */
	k_timer_start(&mouse_timer, K_MSEC(100), K_USEC(750));
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("Disconnected from %s (reason 0x%02x)", addr, reason);

	k_timer_stop(&mouse_timer);
	hid_notify_enabled = false;

	bt_hids_disconnected(&hids_obj, conn);

	bt_conn_unref(default_conn);
	default_conn = NULL;

	adv_start();
}

/* Reject peer-initiated parameter updates; let the central control the rate */
static bool le_param_req(struct bt_conn *conn, struct bt_le_conn_param *param)
{
	return false;
}

static void le_phy_updated(struct bt_conn *conn,
			   struct bt_conn_le_phy_info *param)
{
	LOG_INF("PHY updated: TX PHY %u, RX PHY %u",
		param->tx_phy, param->rx_phy);
	k_sem_give(&phy_updated);
}

static void frame_space_updated(struct bt_conn *conn,
				const struct bt_conn_le_frame_space_updated *params)
{
	if (params->status == 0) {
		LOG_INF("Frame space updated: %u us", params->frame_space);
	} else {
		LOG_WRN("Frame space update failed: 0x%02x", params->status);
	}

	k_sem_give(&frame_space_updated_sem);
}

static void conn_rate_changed(struct bt_conn *conn, uint8_t status,
			      const struct bt_conn_le_conn_rate_changed *params)
{
	if (status == 0) {
		LOG_INF("Connection rate changed: interval %u us",
			params->interval_us);
	} else {
		LOG_WRN("Connection rate change failed: 0x%02x", status);
	}
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected         = connected,
	.disconnected      = disconnected,
	.le_param_req      = le_param_req,
	.le_phy_updated    = le_phy_updated,
	.conn_rate_changed = conn_rate_changed,
	.frame_space_updated = frame_space_updated,
};

/* ???? main ?????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????? */

int main(void)
{
	int err;

	LOG_INF("Starting SCI HID Peripheral");

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("Bluetooth init failed (err %d)", err);
		return 0;
	}

	LOG_INF("Bluetooth initialized");

	/* Read the local hardware minimum connection interval */
	err = bt_conn_le_read_min_conn_interval(&local_min_interval_us);
	if (err) {
		LOG_WRN("Failed to read local min interval (err %d) ??using 750 us", err);
		local_min_interval_us = 750;
	}

	LOG_INF("Local minimum connection interval: %u us", local_min_interval_us);

	if (!gpio_is_ready_dt(&button0)) {
		LOG_ERR("Button GPIO not ready");
		return 0;
	}
	gpio_pin_configure_dt(&button0, GPIO_INPUT);
	gpio_pin_interrupt_configure_dt(&button0, GPIO_INT_EDGE_TO_ACTIVE);
	gpio_init_callback(&button0_cb_data, button0_pressed, BIT(button0.pin));
	gpio_add_callback(button0.port, &button0_cb_data);

	hid_init();
	adv_start();

	/* The main thread can now idle; all work is event-driven */
	return 0;
}
