/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <string.h>
#include <stdint.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <bluetooth/scan.h>
#include <bluetooth/gatt_dm.h>
#include <bluetooth/services/hogp.h>

#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_hid.h>
#include <zephyr/drivers/usb/udc_buf.h>
#include <sample_usbd.h>

LOG_MODULE_REGISTER(app_main, LOG_LEVEL_INF);

#define DEVICE_NAME     CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)

#define INTERVAL_INITIAL_US  10000 /* 10 ms */
#define TARGET_INTERVAL_US     750 /* 750 us = shortest SCI interval */

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

/* Simple 3-byte mouse HID report descriptor (no report ID).
 * Must match what the peripheral advertises. */
static const uint8_t mouse_report_desc[] = {
0x05, 0x01,     /* Usage Page (Generic Desktop) */
0x09, 0x02,     /* Usage (Mouse) */
0xA1, 0x01,     /* Collection (Application) */
0x09, 0x01,     /* Usage (Pointer) */
0xA1, 0x00,     /* Collection (Physical) */
/* 3 buttons */
0x05, 0x09,     /* Usage Page (Buttons) */
0x19, 0x01,
0x29, 0x03,
0x15, 0x00,
0x25, 0x01,
0x75, 0x01,
0x95, 0x03,
0x81, 0x02,
/* 5-bit padding */
0x75, 0x05,
0x95, 0x01,
0x81, 0x01,
/* X, Y relative */
0x05, 0x01,
0x09, 0x30,
0x09, 0x31,
0x15, 0x81,
0x25, 0x7F,
0x75, 0x08,
0x95, 0x02,
0x81, 0x06,
0xC0,           /* End Collection (Physical) */
0xC0,           /* End Collection (Application) */
};

#define MOUSE_REPORT_SIZE 3

/* USB HID ----------------------------------------------------------------- */

static bool usb_hid_ready;

static void mouse_iface_ready(const struct device *dev, const bool ready)
{
LOG_INF("USB HID interface %s", ready ? "ready" : "not ready");
usb_hid_ready = ready;
}

static int mouse_get_report(const struct device *dev,
    const uint8_t type, const uint8_t id,
    const uint16_t len, uint8_t *const buf)
{
return 0;
}

static void mouse_set_idle(const struct device *dev,
    const uint8_t id, const uint32_t duration)
{
/* Ignore idle rate requests — we send reports continuously */
}

static struct hid_device_ops mouse_ops = {
.iface_ready = mouse_iface_ready,
.get_report  = mouse_get_report,
.set_idle    = mouse_set_idle,
};

static const struct device *hid_dev;

/* Message queue for USB HID reports: submit from BT thread, send from main */
K_MSGQ_DEFINE(usb_hid_msgq, MOUSE_REPORT_SIZE, 4, 4);

static void usb_forward_report(const uint8_t *data, uint8_t len)
{
	if (!usb_hid_ready || !hid_dev) {
		return;
	}

	uint8_t report[MOUSE_REPORT_SIZE] = {0};

	memcpy(report, data, MIN(len, MOUSE_REPORT_SIZE));

	if (k_msgq_put(&usb_hid_msgq, report, K_NO_WAIT) != 0) {
		LOG_DBG("USB HID msgq full");
	}
}

/* Bluetooth BLE HID (HOGP) ------------------------------------------------ */

static struct bt_hogp hogp;
static struct bt_conn *default_conn;
static uint16_t local_min_interval_us;
static uint16_t remote_min_interval_us;
static uint16_t remote_min_interval_handle;

static K_SEM_DEFINE(phy_updated, 0, 1);
static K_SEM_DEFINE(frame_space_updated_sem, 0, 1);
static K_SEM_DEFINE(discovery_complete_sem, 0, 1);
static K_SEM_DEFINE(min_interval_read_sem, 0, 1);

static void hids_on_ready(struct k_work *work);
static K_WORK_DEFINE(hids_ready_work, hids_on_ready);

static uint8_t hogp_notify_cb(struct bt_hogp *hogp_ctx,
      struct bt_hogp_rep_info *rep,
      uint8_t err,
      const uint8_t *data)
{
if (!data) {
return BT_GATT_ITER_STOP;
}

uint8_t size = bt_hogp_rep_size(rep);

LOG_DBG("HID notify id=%u size=%u", bt_hogp_rep_id(rep), size);

usb_forward_report(data, size);

return BT_GATT_ITER_CONTINUE;
}

static void hids_on_ready(struct k_work *work)
{
struct bt_hogp_rep_info *rep = NULL;

LOG_INF("HOGP ready  subscribing to input reports");

while (NULL != (rep = bt_hogp_rep_next(&hogp, rep))) {
if (bt_hogp_rep_type(rep) == BT_HIDS_REPORT_TYPE_INPUT) {
int err = bt_hogp_rep_subscribe(&hogp, rep, hogp_notify_cb);

if (err) {
LOG_WRN("Subscribe error: %d", err);
} else {
LOG_INF("Subscribed to report id=%u",
bt_hogp_rep_id(rep));
}
}
}
}

static void hogp_ready_cb(struct bt_hogp *hogp_ctx)
{
k_work_submit(&hids_ready_work);
}

static void hogp_prep_fail_cb(struct bt_hogp *hogp_ctx, int err)
{
LOG_ERR("HOGP preparation failed: %d", err);
}

static void hogp_pm_update_cb(struct bt_hogp *hogp_ctx)
{
LOG_INF("HOGP protocol mode: %s",
bt_hogp_pm_get(hogp_ctx) == BT_HIDS_PM_BOOT ? "BOOT" : "REPORT");
}

static const struct bt_hogp_init_params hogp_init_params = {
.ready_cb      = hogp_ready_cb,
.prep_error_cb = hogp_prep_fail_cb,
.pm_update_cb  = hogp_pm_update_cb,
};

/* SCI min-interval discovery ---------------------------------------------- */

static uint8_t read_min_interval_cb(struct bt_conn *conn, uint8_t err,
    struct bt_gatt_read_params *params,
    const void *data, uint16_t length)
{
if (!err && data && length == sizeof(uint16_t)) {
remote_min_interval_us = sys_get_le16(data);
LOG_INF("Remote min interval: %u us", remote_min_interval_us);
} else if (err) {
LOG_WRN("Failed to read remote min interval (err %u)", err);
}

k_sem_give(&min_interval_read_sem);
return BT_GATT_ITER_STOP;
}

static int read_remote_min_interval(void)
{
static struct bt_gatt_read_params read_params;

read_params.func             = read_min_interval_cb;
read_params.handle_count     = 1;
read_params.single.handle    = remote_min_interval_handle;
read_params.single.offset    = 0;

int err = bt_gatt_read(default_conn, &read_params);

if (err) {
LOG_WRN("Read request failed (err %d)", err);
}

return err;
}

/* GATT discovery sequence:
 *   1. Discover HID service (HOGP)
 *   2. Then discover SCI min-interval service
 */

static void hid_discovery_complete(struct bt_gatt_dm *dm, void *context);
static void sci_discovery_complete(struct bt_gatt_dm *dm, void *context);

static void sci_discovery_service_not_found(struct bt_conn *conn, void *context)
{
LOG_INF("SCI service not found (peer may not support it)");
/* Still signal completion so SCI interval update is skipped */
k_sem_give(&discovery_complete_sem);
}

static void sci_discovery_error(struct bt_conn *conn, int err, void *context)
{
LOG_WRN("SCI discovery error: %d", err);
k_sem_give(&discovery_complete_sem);
}

static struct bt_gatt_dm_cb sci_discovery_cb = {
.completed         = sci_discovery_complete,
.service_not_found = sci_discovery_service_not_found,
.error_found       = sci_discovery_error,
};

static void sci_discovery_complete(struct bt_gatt_dm *dm, void *context)
{
const struct bt_gatt_dm_attr *chrc;
const struct bt_gatt_dm_attr *val;

LOG_INF("SCI service discovered");

chrc = bt_gatt_dm_char_by_uuid(dm, BT_UUID_VS_SCI_MIN_INTERVAL_CHAR);
if (chrc) {
val = bt_gatt_dm_attr_next(dm, chrc);
if (val) {
remote_min_interval_handle = val->handle;
LOG_INF("SCI min interval handle: 0x%04x",
remote_min_interval_handle);
}
}

bt_gatt_dm_data_release(dm);
k_sem_give(&discovery_complete_sem);
}

static void hid_discovery_complete(struct bt_gatt_dm *dm, void *context)
{
int err;

LOG_INF("HID service discovered");

err = bt_hogp_handles_assign(dm, &hogp);
if (err) {
LOG_ERR("Could not assign HOGP handles: %d", err);
}

err = bt_gatt_dm_data_release(dm);
if (err) {
LOG_WRN("Could not release discovery data: %d", err);
}

/* Chain to SCI service discovery */
err = bt_gatt_dm_start(default_conn, BT_UUID_VS_SCI_MIN_INTERVAL_SERVICE,
       &sci_discovery_cb, NULL);
if (err) {
LOG_WRN("SCI discovery start failed: %d  skip", err);
k_sem_give(&discovery_complete_sem);
}
}

static void hid_discovery_service_not_found(struct bt_conn *conn, void *context)
{
LOG_WRN("HID service not found");
k_sem_give(&discovery_complete_sem);
}

static void hid_discovery_error(struct bt_conn *conn, int err, void *context)
{
LOG_WRN("HID discovery error: %d", err);
k_sem_give(&discovery_complete_sem);
}

static const struct bt_gatt_dm_cb hid_discovery_cb = {
.completed         = hid_discovery_complete,
.service_not_found = hid_discovery_service_not_found,
.error_found       = hid_discovery_error,
};

static void gatt_discover(struct bt_conn *conn)
{
int err = bt_gatt_dm_start(conn, BT_UUID_HIDS, &hid_discovery_cb, NULL);

if (err) {
LOG_WRN("GATT discovery start failed (err %d)", err);
}
}

/* BT scanning ------------------------------------------------------------- */

static struct bt_le_conn_param *conn_param =
BT_LE_CONN_PARAM(INTERVAL_INITIAL_US / 1250,
 INTERVAL_INITIAL_US / 1250,
 0, 400);

static void scan_filter_match(struct bt_scan_device_info *device_info,
      struct bt_scan_filter_match *filter_match,
      bool connectable)
{
char addr[BT_ADDR_LE_STR_LEN];

bt_addr_le_to_str(device_info->recv_info->addr, addr, sizeof(addr));
LOG_INF("Filter matched: %s", addr);
}

static void scan_connecting_error(struct bt_scan_device_info *device_info)
{
LOG_WRN("Connecting failed");
}

static void scan_connecting(struct bt_scan_device_info *device_info,
    struct bt_conn *conn)
{
default_conn = bt_conn_ref(conn);
}

BT_SCAN_CB_INIT(scan_cb, scan_filter_match, NULL, scan_connecting_error, scan_connecting);

static void scan_init(void)
{
int err;
struct bt_le_scan_param scan_param = {
.type    = BT_LE_SCAN_TYPE_PASSIVE,
.options = BT_LE_SCAN_OPT_FILTER_DUPLICATE,
.interval = 0x0020,
.window   = 0x0010,
};

struct bt_scan_init_param scan_init_param = {
.connect_if_match = true,
.scan_param       = &scan_param,
.conn_param       = conn_param,
};

bt_scan_init(&scan_init_param);
bt_scan_cb_register(&scan_cb);

/* Scan for devices advertising HIDS (16-bit UUID in primary AD) */
err = bt_scan_filter_add(BT_SCAN_FILTER_TYPE_UUID,
 BT_UUID_HIDS);
if (err) {
LOG_WRN("Scan filter add failed: %d", err);
return;
}

err = bt_scan_filter_enable(BT_SCAN_UUID_FILTER, false);
if (err) {
LOG_WRN("Scan filter enable failed: %d", err);
}
}

static void scan_start(void)
{
int err = bt_scan_start(BT_SCAN_TYPE_SCAN_PASSIVE);

if (err) {
LOG_WRN("Scanning failed to start (err %d)", err);
return;
}

LOG_INF("Scanning started");
}

/* SCI interval helpers ---------------------------------------------------- */

static int update_to_2m_phy(void)
{
struct bt_conn_le_phy_param phy = {
.options    = BT_CONN_LE_PHY_OPT_NONE,
.pref_rx_phy = BT_GAP_LE_PHY_2M,
.pref_tx_phy = BT_GAP_LE_PHY_2M,
};

int err = bt_conn_le_phy_update(default_conn, &phy);

if (err) {
LOG_WRN("PHY update failed: %d", err);
return err;
}

k_sem_take(&phy_updated, K_FOREVER);
return 0;
}

static int select_lowest_frame_space(void)
{
const struct bt_conn_le_frame_space_update_param params = {
.phys          = BT_HCI_LE_FRAME_SPACE_UPDATE_PHY_2M_MASK,
.spacing_types = BT_CONN_LE_FRAME_SPACE_TYPES_MASK_ACL_IFS,
.frame_space_min = 0,
.frame_space_max = 150,
};

int err = bt_conn_le_frame_space_update(default_conn, &params);

if (err) {
LOG_WRN("Frame space update failed: %d", err);
return err;
}

k_sem_take(&frame_space_updated_sem, K_FOREVER);
return 0;
}

static int conn_rate_request(uint32_t interval_min_us, uint32_t interval_max_us)
{
const struct bt_conn_le_conn_rate_param params = {
.interval_min_125us       = interval_min_us / 125,
.interval_max_125us       = interval_max_us / 125,
.subrate_min              = 1,
.subrate_max              = 1,
.max_latency              = 0,
.continuation_number      = 0,
.supervision_timeout_10ms = 400,
.min_ce_len_125us         = BT_HCI_LE_SCI_CE_LEN_MIN_125US,
.max_ce_len_125us         = BT_HCI_LE_SCI_CE_LEN_MAX_125US,
};

int err = bt_conn_le_conn_rate_request(default_conn, &params);

if (err) {
LOG_WRN("conn_rate_request failed: %d", err);
}

return err;
}

/* BT connection callbacks ------------------------------------------------- */

static void connected(struct bt_conn *conn, uint8_t err)
{
char addr[BT_ADDR_LE_STR_LEN];

bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

if (err) {
LOG_WRN("Connection to %s failed (err 0x%02x)", addr, err);
bt_conn_unref(default_conn);
default_conn = NULL;
scan_start();
return;
}

LOG_INF("Connected: %s", addr);

bt_scan_stop();
gatt_discover(conn);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
char addr[BT_ADDR_LE_STR_LEN];

bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
LOG_INF("Disconnected from %s (reason 0x%02x)", addr, reason);

if (bt_hogp_assign_check(&hogp)) {
bt_hogp_release(&hogp);
}

bt_conn_unref(default_conn);
default_conn = NULL;

scan_start();
}

static bool le_param_req(struct bt_conn *conn, struct bt_le_conn_param *param)
{
/* Central controls the rate  ignore peripheral requests */
return false;
}

static void le_phy_updated(struct bt_conn *conn,
   struct bt_conn_le_phy_info *param)
{
LOG_INF("PHY updated: TX %u RX %u", param->tx_phy, param->rx_phy);
k_sem_give(&phy_updated);
}

static void frame_space_updated(struct bt_conn *conn,
const struct bt_conn_le_frame_space_updated *params)
{
if (params->status == 0) {
LOG_INF("Frame space: %u us", params->frame_space);
} else {
LOG_WRN("Frame space update failed: 0x%02x", params->status);
}

k_sem_give(&frame_space_updated_sem);
}

static void conn_rate_changed(struct bt_conn *conn, uint8_t status,
      const struct bt_conn_le_conn_rate_changed *params)
{
if (status == 0) {
LOG_INF("Connection rate: %u us", params->interval_us);
} else {
LOG_WRN("Connection rate change failed: 0x%02x", status);
}
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
.connected          = connected,
.disconnected       = disconnected,
.le_param_req       = le_param_req,
.le_phy_updated     = le_phy_updated,
.conn_rate_changed  = conn_rate_changed,
.frame_space_updated = frame_space_updated,
};

/* SCI set-defaults -------------------------------------------------------- */

static int set_conn_rate_defaults(uint32_t interval_min_us,
  uint32_t interval_max_us)
{
const struct bt_conn_le_conn_rate_param params = {
.interval_min_125us       = interval_min_us / 125,
.interval_max_125us       = interval_max_us / 125,
.subrate_min              = 1,
.subrate_max              = 1,
.max_latency              = 5,
.continuation_number      = 0,
.supervision_timeout_10ms = 400,
.min_ce_len_125us         = BT_HCI_LE_SCI_CE_LEN_MIN_125US,
.max_ce_len_125us         = BT_HCI_LE_SCI_CE_LEN_MAX_125US,
};

int err = bt_conn_le_conn_rate_set_defaults(&params);

if (err) {
LOG_WRN("set_conn_rate_defaults failed: %d", err);
}

return err;
}

/* SCI negotiation thread -------------------------------------------------- */

static void sci_negotiate(void)
{
int err;

/* Step 1: upgrade to 2M PHY */
err = update_to_2m_phy();
if (err) {
LOG_WRN("Failed to upgrade to 2M PHY");
return;
}

/* Step 2: select smallest IFS */
err = select_lowest_frame_space();
if (err) {
LOG_WRN("Failed to update frame space");
return;
}

/* Step 3: read peer's minimum supported interval */
uint32_t target_us = TARGET_INTERVAL_US;

if (remote_min_interval_handle != 0) {
if (read_remote_min_interval() == 0) {
k_sem_take(&min_interval_read_sem, K_SECONDS(2));
uint32_t common = MAX(local_min_interval_us,
      remote_min_interval_us);

LOG_INF("Common min interval: %u us", common);
target_us = MAX(target_us, common);
}
}

	/* Step 4: request target interval.
	 * nRF54LM20 minimum SCI support is 750 us, so pin min == max. */
	LOG_INF("Requesting interval: %u us", target_us);
	conn_rate_request(target_us, target_us);
}

/* Main -------------------------------------------------------------------- */

int main(void)
{
int err;

LOG_INF("Starting SCI HID Central");

/*  USB HID setup  */
hid_dev = DEVICE_DT_GET_ONE(zephyr_hid_device);
if (!device_is_ready(hid_dev)) {
LOG_INF("USB HID device not ready");
return -EIO;
}

err = hid_device_register(hid_dev,
  mouse_report_desc,
  sizeof(mouse_report_desc),
  &mouse_ops);
if (err) {
LOG_INF("Failed to register USB HID device: %d", err);
return err;
}

struct usbd_context *usbd = sample_usbd_init_device(NULL);

if (!usbd) {
LOG_INF("Failed to init USB device");
return -ENODEV;
}

err = usbd_enable(usbd);
if (err) {
LOG_INF("Failed to enable USB: %d", err);
return err;
}

LOG_INF("USB HID device enabled");

/*  Bluetooth setup  */
bt_hogp_init(&hogp, &hogp_init_params);

err = bt_enable(NULL);
if (err) {
LOG_ERR("Bluetooth init failed: %d", err);
return 0;
}

LOG_INF("Bluetooth initialized");

err = bt_conn_le_read_min_conn_interval(&local_min_interval_us);
if (err) {
LOG_WRN("Failed to read local min interval (err %d)", err);
local_min_interval_us = TARGET_INTERVAL_US;
}

LOG_INF("Local min interval: %u us", local_min_interval_us);

err = set_conn_rate_defaults(local_min_interval_us, INTERVAL_INITIAL_US);
if (err) {
LOG_WRN("set_conn_rate_defaults failed");
}

scan_init();
scan_start();

	/* ── Main loop ─────────────────────────────────────────────────── */
	for (;;) {
		UDC_STATIC_BUF_DEFINE(usb_report, MOUSE_REPORT_SIZE);

		/* Try to send a queued USB HID report with a short timeout */
		if (k_msgq_get(&usb_hid_msgq, usb_report, K_MSEC(50)) == 0) {
			if (usb_hid_ready && hid_dev) {
				int err2 = hid_device_submit_report(
					hid_dev, MOUSE_REPORT_SIZE, usb_report);

				if (err2) {
					LOG_DBG("USB HID submit err: %d", err2);
				}
			}
			continue;
		}

		/* If discovery just completed, run SCI negotiation */
		if (k_sem_take(&discovery_complete_sem, K_NO_WAIT) == 0) {
			if (default_conn) {
				sci_negotiate();
			}
		}
	}
}
