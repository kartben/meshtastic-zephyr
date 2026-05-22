/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "meshtastic_phoneapi.h"

#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

LOG_MODULE_DECLARE(meshtastic, CONFIG_MESHTASTIC_LOG_LEVEL);

#define BT_UUID_MESHTASTIC_SERVICE_VAL                                                             \
	BT_UUID_128_ENCODE(0x6ba1b218, 0x15a8, 0x461f, 0x9fa8, 0x5dcae273eafd)
#define BT_UUID_MESHTASTIC_TORADIO_VAL                                                             \
	BT_UUID_128_ENCODE(0xf75c76d2, 0x129e, 0x4dad, 0xa1dd, 0x7866124401e7)
#define BT_UUID_MESHTASTIC_FROMRADIO_VAL                                                           \
	BT_UUID_128_ENCODE(0x2c55e69e, 0x4993, 0x11ed, 0xb878, 0x0242ac120002)
#define BT_UUID_MESHTASTIC_FROMNUM_VAL                                                             \
	BT_UUID_128_ENCODE(0xed9da18c, 0xa800, 0x4f66, 0xa670, 0xaa7547e34453)
#define BT_UUID_MESHTASTIC_LOGRADIO_VAL                                                            \
	BT_UUID_128_ENCODE(0x5a3d6e49, 0x06e6, 0x4423, 0x9944, 0xe9de8cdf9547)

static struct bt_uuid_128 meshtastic_service_uuid =
	BT_UUID_INIT_128(BT_UUID_MESHTASTIC_SERVICE_VAL);
static struct bt_uuid_128 toradio_uuid = BT_UUID_INIT_128(BT_UUID_MESHTASTIC_TORADIO_VAL);
static struct bt_uuid_128 fromradio_uuid = BT_UUID_INIT_128(BT_UUID_MESHTASTIC_FROMRADIO_VAL);
static struct bt_uuid_128 fromnum_uuid = BT_UUID_INIT_128(BT_UUID_MESHTASTIC_FROMNUM_VAL);
static struct bt_uuid_128 logradio_uuid = BT_UUID_INIT_128(BT_UUID_MESHTASTIC_LOGRADIO_VAL);

extern const struct bt_gatt_attr attr_meshtastic_svc[];

static struct {
	struct meshtastic_phoneapi api;
	struct meshtastic_phoneapi_frame queue[CONFIG_MESHTASTIC_BLE_FROMRADIO_QUEUE_SIZE];
	struct k_mutex lock;
	bool fromnum_notify_enabled;
	bool log_notify_enabled;
	struct bt_conn *conn;
	uint8_t to_radio_buf[MESHTASTIC_API_FRAME_MAX];
	uint16_t to_radio_len;
	struct k_work_q work_q;
	struct k_work to_radio_work;
	struct k_work fromradio_work;
} ble;

static K_THREAD_STACK_DEFINE(ble_work_stack, CONFIG_MESHTASTIC_BLE_WORK_STACK_SIZE);

static struct meshtastic_phoneapi_frame fromradio_staged;
static uint8_t fromradio_buf[MESHTASTIC_API_FRAME_MAX];
static uint16_t fromradio_len;
static bool fromradio_ready;

#if defined(CONFIG_MESHTASTIC_BLE_FIXED_PASSKEY)
/*
 * Match official Meshtastic NimBLE (PROPERTY_*_ENC / *_AUTHEN) and nrf54l15
 * MESH_PERM_*_AUTHEN: MITM passkey pairing before any mesh GATT access.
 */
#define MT_GATT_PERM_READ  (BT_GATT_PERM_READ | BT_GATT_PERM_READ_AUTHEN)
#define MT_GATT_PERM_WRITE (BT_GATT_PERM_WRITE | BT_GATT_PERM_WRITE_AUTHEN)
#elif defined(CONFIG_MESHTASTIC_BLE_PAIRING)
/* Bonding with Just Works; no MITM (official NO_PIN uses open GATT). */
#define MT_GATT_PERM_READ  BT_GATT_PERM_READ
#define MT_GATT_PERM_WRITE BT_GATT_PERM_WRITE
#else
#define MT_GATT_PERM_READ  BT_GATT_PERM_READ
#define MT_GATT_PERM_WRITE BT_GATT_PERM_WRITE
#endif

enum {
	MESHTASTIC_ATTR_SERVICE = 0,
	MESHTASTIC_ATTR_TORADIO_CHRC,
	MESHTASTIC_ATTR_TORADIO_VALUE,
	MESHTASTIC_ATTR_FROMRADIO_CHRC,
	MESHTASTIC_ATTR_FROMRADIO_VALUE,
	MESHTASTIC_ATTR_FROMNUM_CHRC,
	MESHTASTIC_ATTR_FROMNUM_VALUE,
	MESHTASTIC_ATTR_FROMNUM_CCC,
	MESHTASTIC_ATTR_LOGRADIO_CHRC,
	MESHTASTIC_ATTR_LOGRADIO_VALUE,
	MESHTASTIC_ATTR_LOGRADIO_CCC,
};

static void notify_fromnum(void);
static void schedule_fromradio_prepare(void);

static void ble_invalidate_delivery(struct meshtastic_phoneapi *api)
{
	ARG_UNUSED(api);

	fromradio_ready = false;
	fromradio_len = 0U;
	meshtastic_phoneapi_current_frame_reset(&ble.api);
}

static void ble_data_ready(struct meshtastic_phoneapi *api)
{
	ARG_UNUSED(api);

	schedule_fromradio_prepare();
}

static void fromradio_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (fromradio_ready) {
		return;
	}

	if (!meshtastic_phoneapi_current_frame(&ble.api, &fromradio_staged)) {
		return;
	}

	k_mutex_lock(&ble.api.lock, K_FOREVER);
	ble.api.from_num++;
	k_mutex_unlock(&ble.api.lock);

	fromradio_ready = true;
	LOG_DBG("BLE FromRadio staged len=%u from_num=%u", fromradio_staged.len,
		meshtastic_phoneapi_from_num(&ble.api));
	notify_fromnum();
}

static void ble_disconnect(struct meshtastic_phoneapi *api)
{
	ARG_UNUSED(api);

	if (ble.conn != NULL) {
		(void)bt_conn_disconnect(ble.conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	}
}

static void schedule_fromradio_prepare(void)
{
	(void)k_work_submit_to_queue(&ble.work_q, &ble.fromradio_work);
}

static void notify_fromnum(void)
{
	uint8_t value[4];
	struct bt_conn *conn;

	k_mutex_lock(&ble.lock, K_FOREVER);
	if (!ble.fromnum_notify_enabled || ble.conn == NULL) {
		k_mutex_unlock(&ble.lock);
		return;
	}
	conn = bt_conn_ref(ble.conn);
	sys_put_le32(meshtastic_phoneapi_from_num(&ble.api), value);
	k_mutex_unlock(&ble.lock);

	(void)bt_gatt_notify(conn, &attr_meshtastic_svc[MESHTASTIC_ATTR_FROMNUM_VALUE], value,
			     sizeof(value));
	bt_conn_unref(conn);
}

static ssize_t read_fromnum(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			    uint16_t len, uint16_t offset)
{
	uint8_t value[4];

	ARG_UNUSED(conn);
	ARG_UNUSED(attr);

	sys_put_le32(meshtastic_phoneapi_from_num(&ble.api), value);
	return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(value));
}

static ssize_t read_fromradio(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			      uint16_t len, uint16_t offset)
{
	ssize_t ret;

	ARG_UNUSED(conn);
	ARG_UNUSED(attr);

	if (offset == 0U) {
		if (fromradio_ready) {
			memcpy(fromradio_buf, fromradio_staged.data, fromradio_staged.len);
			fromradio_len = fromradio_staged.len;
		} else {
			fromradio_len = 0U;
		}
	}

	ret = bt_gatt_attr_read(conn, attr, buf, len, offset, fromradio_buf, fromradio_len);
	if (ret >= 0 && fromradio_len > 0U && offset + ret >= fromradio_len) {
		meshtastic_phoneapi_release_current_frame(&ble.api);
		fromradio_ready = false;
		schedule_fromradio_prepare();
	}

	return ret;
}

static ssize_t read_logradio(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			     uint16_t len, uint16_t offset)
{
	static const char msg[] = "Zephyr Meshtastic\n";

	return bt_gatt_attr_read(conn, attr, buf, len, offset, msg, sizeof(msg) - 1U);
}

static void fromnum_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);

	k_mutex_lock(&ble.lock, K_FOREVER);
	ble.fromnum_notify_enabled = (value == BT_GATT_CCC_NOTIFY);
	k_mutex_unlock(&ble.lock);
}

static void logradio_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);

	k_mutex_lock(&ble.lock, K_FOREVER);
	ble.log_notify_enabled = (value & (BT_GATT_CCC_NOTIFY | BT_GATT_CCC_INDICATE)) != 0U;
	k_mutex_unlock(&ble.lock);
}

static void to_radio_work_handler(struct k_work *work)
{
	uint8_t buf[MESHTASTIC_API_FRAME_MAX];
	uint16_t buf_len;

	ARG_UNUSED(work);

	k_mutex_lock(&ble.lock, K_FOREVER);
	buf_len = ble.to_radio_len;
	memcpy(buf, ble.to_radio_buf, buf_len);
	ble.to_radio_len = 0U;
	k_mutex_unlock(&ble.lock);

	meshtastic_phoneapi_handle_toradio(&ble.api, buf, buf_len);
}

static ssize_t write_toradio(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
			     uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);

	if (offset + len > sizeof(ble.to_radio_buf)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	k_mutex_lock(&ble.lock, K_FOREVER);
	if (offset == 0U && (flags & BT_GATT_WRITE_FLAG_PREPARE) == 0U) {
		ble.to_radio_len = 0U;
	}
	memcpy(ble.to_radio_buf + offset, buf, len);
	ble.to_radio_len = MAX(ble.to_radio_len, offset + len);
	k_mutex_unlock(&ble.lock);

	if ((flags & BT_GATT_WRITE_FLAG_PREPARE) == 0U) {
		k_work_submit_to_queue(&ble.work_q, &ble.to_radio_work);
	}

	return len;
}

BT_GATT_SERVICE_DEFINE(meshtastic_svc, BT_GATT_PRIMARY_SERVICE(&meshtastic_service_uuid.uuid),
		       BT_GATT_CHARACTERISTIC(&toradio_uuid.uuid,
					      BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
					      MT_GATT_PERM_WRITE, NULL, write_toradio, NULL),
		       BT_GATT_CHARACTERISTIC(&fromradio_uuid.uuid, BT_GATT_CHRC_READ,
					      MT_GATT_PERM_READ, read_fromradio, NULL, NULL),
		       BT_GATT_CHARACTERISTIC(&fromnum_uuid.uuid,
					      BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
					      MT_GATT_PERM_READ, read_fromnum, NULL, NULL),
		       BT_GATT_CCC(fromnum_ccc_changed, MT_GATT_PERM_READ | MT_GATT_PERM_WRITE),
		       BT_GATT_CHARACTERISTIC(&logradio_uuid.uuid,
					      BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY |
						      BT_GATT_CHRC_INDICATE,
					      MT_GATT_PERM_READ, read_logradio, NULL, NULL),
		       BT_GATT_CCC(logradio_ccc_changed, MT_GATT_PERM_READ | MT_GATT_PERM_WRITE));

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err != 0U) {
		LOG_WRN("BLE connection failed: 0x%02x", err);
		return;
	}

	k_mutex_lock(&ble.lock, K_FOREVER);
	if (ble.conn == NULL) {
		ble.conn = bt_conn_ref(conn);
	}
	k_mutex_unlock(&ble.lock);

	meshtastic_set_ble_connected(true);
	meshtastic_emit_event(MESHTASTIC_EVENT_BLE_CONNECTED, 0, NULL);

	/*
	 * Do not call bt_conn_set_security() here. Official Meshtastic firmware
	 * lets the phone initiate SMP when it needs encrypted GATT access.
	 * A peripheral-initiated pairing races the app and often ends with MIC
	 * failure (0x3d) or "pairing has been deleted".
	 */
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	if (ble.conn == conn) {
		k_mutex_lock(&ble.lock, K_FOREVER);
		bt_conn_unref(ble.conn);
		ble.conn = NULL;
		k_mutex_unlock(&ble.lock);
	}

	ble_invalidate_delivery(&ble.api);
	meshtastic_phoneapi_reset(&ble.api);
	(void)k_work_cancel(&ble.fromradio_work);

	meshtastic_set_ble_connected(false);
	meshtastic_emit_event(MESHTASTIC_EVENT_BLE_DISCONNECTED, 0, NULL);
	LOG_INF("BLE disconnected: 0x%02x", reason);
}

#if defined(CONFIG_BT_SMP)
static struct bt_gatt_exchange_params mtu_exchange;

static void mtu_exchange_cb(struct bt_conn *conn, uint8_t err,
			    struct bt_gatt_exchange_params *params)
{
	ARG_UNUSED(params);

	if (err != 0U) {
		LOG_WRN("BLE MTU exchange failed: 0x%02x", err);
		return;
	}

	LOG_INF("BLE MTU %u", bt_gatt_get_mtu(conn));
}

static void security_changed(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
	if (err != BT_SECURITY_ERR_SUCCESS) {
		LOG_WRN("BLE security failed: level %u err %u (%s)", (unsigned int)level,
			(unsigned int)err, bt_security_err_to_str(err));

		if (err == BT_SECURITY_ERR_PIN_OR_KEY_MISSING) {
			/*
			 * Stale phone bond after reflash (official nrf54l15 path).
			 * Do not bt_unpair on generic failures — that races SMP and
			 * triggers "The in-progress pairing has been deleted!".
			 */
			LOG_WRN("BLE stale bond (key missing); forget device in Meshtastic app");
			(void)bt_unpair(BT_ID_DEFAULT, bt_conn_get_dst(conn));
		}
		return;
	}

	LOG_INF("BLE security level %u", (unsigned int)level);

	mtu_exchange.func = mtu_exchange_cb;
	if (bt_gatt_exchange_mtu(conn, &mtu_exchange) < 0) {
		LOG_WRN("BLE MTU exchange request failed");
	}
}

#if defined(CONFIG_MESHTASTIC_BLE_FIXED_PASSKEY)
static void auth_passkey_display(struct bt_conn *conn, unsigned int passkey)
{
	ARG_UNUSED(conn);

	LOG_INF("BLE pairing passkey %06u (enter in Meshtastic app)", passkey);
}

static uint32_t auth_app_passkey(struct bt_conn *conn)
{
	ARG_UNUSED(conn);

	return CONFIG_MESHTASTIC_BLE_PASSKEY;
}
#endif

#if defined(CONFIG_MESHTASTIC_BLE_PAIRING) && !defined(CONFIG_MESHTASTIC_BLE_FIXED_PASSKEY)
static void pairing_confirm(struct bt_conn *conn)
{
	LOG_INF("BLE pairing confirm (Just Works)");
	(void)bt_conn_auth_pairing_confirm(conn);
}
#endif

static void pairing_cancel(struct bt_conn *conn)
{
	ARG_UNUSED(conn);

	LOG_INF("BLE pairing cancelled");
}

#if defined(CONFIG_BT_SMP_APP_PAIRING_ACCEPT)
static enum bt_security_err pairing_accept(struct bt_conn *conn,
					   const struct bt_conn_pairing_feat *const feat)
{
	ARG_UNUSED(conn);

	LOG_DBG("BLE pairing request auth_req=0x%02x io=0x%02x", feat->auth_req,
		feat->io_capability);

	return BT_SECURITY_ERR_SUCCESS;
}
#endif

static const struct bt_conn_auth_cb auth_callbacks = {
#if defined(CONFIG_BT_SMP_APP_PAIRING_ACCEPT)
	.pairing_accept = pairing_accept,
#endif
#if defined(CONFIG_MESHTASTIC_BLE_FIXED_PASSKEY)
	.passkey_display = auth_passkey_display,
	.app_passkey = auth_app_passkey,
#elif defined(CONFIG_MESHTASTIC_BLE_PAIRING)
	.pairing_confirm = pairing_confirm,
#endif
	.cancel = pairing_cancel,
};

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	ARG_UNUSED(conn);

	LOG_INF("BLE pairing complete, bonded=%u", bonded);
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	LOG_WRN("BLE pairing failed: err %u (%s)", (unsigned int)reason,
		bt_security_err_to_str(reason));

	if (reason == BT_SECURITY_ERR_PIN_OR_KEY_MISSING || reason == BT_SECURITY_ERR_AUTH_FAIL) {
		LOG_WRN("Forget this device in the Meshtastic app, then reconnect");
	}
}

static struct bt_conn_auth_info_cb auth_info_callbacks = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed,
};
#endif

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
#if defined(CONFIG_BT_SMP)
	.security_changed = security_changed,
#endif
};

static int start_advertising(void)
{
	static const uint8_t flags[] = {
		BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR,
	};
	static const uint8_t service_uuid[] = {
		BT_UUID_MESHTASTIC_SERVICE_VAL,
	};
	static const struct bt_data ad[] = {
		BT_DATA(BT_DATA_FLAGS, flags, sizeof(flags)),
		BT_DATA(BT_DATA_UUID128_ALL, service_uuid, sizeof(service_uuid)),
	};
	static const struct bt_data sd[] = {
		BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_MESHTASTIC_BLE_DEVICE_NAME,
			sizeof(CONFIG_MESHTASTIC_BLE_DEVICE_NAME) - 1U),
	};

	return bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
}

int meshtastic_ble_init(void)
{
	int ret;

	k_mutex_init(&ble.lock);
	meshtastic_phoneapi_init(&ble.api, "ble", ble.queue, ARRAY_SIZE(ble.queue), ble_data_ready,
				 ble_disconnect, ble_invalidate_delivery, NULL);
	meshtastic_phoneapi_register(&ble.api);

	k_work_queue_start(&ble.work_q, ble_work_stack, K_THREAD_STACK_SIZEOF(ble_work_stack),
			   CONFIG_MESHTASTIC_BLE_WORK_PRIORITY, NULL);
	k_work_init(&ble.to_radio_work, to_radio_work_handler);
	k_work_init(&ble.fromradio_work, fromradio_work_handler);

	if (!bt_is_ready()) {
		ret = bt_enable(NULL);
		if (ret < 0) {
			LOG_ERR("bt_enable failed (%d)", ret);
			return ret;
		}
	}

#if defined(CONFIG_SETTINGS)
	settings_load();
#endif

#if defined(CONFIG_BT_SMP)
	if (IS_ENABLED(CONFIG_MESHTASTIC_BLE_PAIRING)) {
		ret = bt_conn_auth_cb_register(&auth_callbacks);
		if (ret < 0 && ret != -EALREADY) {
			LOG_WRN("BLE auth callback registration failed (%d)", ret);
		}

		ret = bt_conn_auth_info_cb_register(&auth_info_callbacks);
		if (ret < 0 && ret != -EALREADY) {
			LOG_WRN("BLE auth info callback registration failed (%d)", ret);
		}
	}
#endif

	// #if defined(CONFIG_MESHTASTIC_BLE_FIXED_PASSKEY)
	// 	LOG_INF("BLE fixed PIN %06u", (unsigned int)CONFIG_MESHTASTIC_BLE_PASSKEY);
	// #endif

	if (IS_ENABLED(CONFIG_MESHTASTIC_BLE_ADV)) {
		ret = start_advertising();
		if (ret < 0 && ret != -EALREADY) {
			LOG_ERR("Meshtastic BLE advertising failed (%d)", ret);
			return ret;
		}
	}

	LOG_INF("Meshtastic BLE service ready");
	return 0;
}
