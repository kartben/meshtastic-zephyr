/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <pb_encode.h>

#include <zephyr/drivers/fuel_gauge.h>

#include "meshtastic_modules.h"
#include "meshtastic_telemetry_internal.h"
#include "meshtastic_airtime.h"

#include <zephyr/meshtastic/telemetry.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(meshtastic, CONFIG_MESHTASTIC_LOG_LEVEL);

#if defined(CONFIG_MESHTASTIC_FUEL_GAUGE) && DT_NODE_EXISTS(DT_ALIAS(fuel_gauge0))
#define MESHTASTIC_HAS_FUEL_GAUGE0 1
static const struct device *const fuel_gauge_dev = DEVICE_DT_GET(DT_ALIAS(fuel_gauge0));
#else
#define MESHTASTIC_HAS_FUEL_GAUGE0 0
#endif

static void collect_fuel_gauge(meshtastic_DeviceMetrics *metrics)
{
#if MESHTASTIC_HAS_FUEL_GAUGE0
	union fuel_gauge_prop_val val;
	int ret;

	if (!device_is_ready(fuel_gauge_dev)) {
		return;
	}

	ret = fuel_gauge_get_prop(fuel_gauge_dev, FUEL_GAUGE_RELATIVE_STATE_OF_CHARGE, &val);
	if (ret == 0) {
		metrics->has_battery_level = true;
		metrics->battery_level = MIN((uint32_t)val.relative_state_of_charge, 100U);
	} else {
		ret = fuel_gauge_get_prop(fuel_gauge_dev, FUEL_GAUGE_ABSOLUTE_STATE_OF_CHARGE,
					  &val);
		if (ret == 0) {
			metrics->has_battery_level = true;
			metrics->battery_level = MIN((uint32_t)val.absolute_state_of_charge, 100U);
		}
	}

	ret = fuel_gauge_get_prop(fuel_gauge_dev, FUEL_GAUGE_VOLTAGE, &val);
	if (ret == 0) {
		metrics->has_voltage = true;
		metrics->voltage = (float)val.voltage / 1000000.0f;
	}
#else
	ARG_UNUSED(metrics);
#endif
}

int meshtastic_collect_device_metrics(meshtastic_DeviceMetrics *metrics)
{
	if (metrics == NULL) {
		return -EINVAL;
	}

	*metrics = (meshtastic_DeviceMetrics)meshtastic_DeviceMetrics_init_zero;
	metrics->has_uptime_seconds = true;
	metrics->uptime_seconds = k_uptime_seconds();

	collect_fuel_gauge(metrics);

#if defined(CONFIG_MESHTASTIC_AIRTIME)
	metrics->has_channel_utilization = true;
	metrics->channel_utilization = meshtastic_airtime_channel_util_percent();
	metrics->has_air_util_tx = true;
	metrics->air_util_tx = meshtastic_airtime_tx_util_percent();
#endif

	return 0;
}

int meshtastic_send_device_metrics(uint32_t dest, k_timeout_t wait)
{
	meshtastic_DeviceMetrics metrics;
	meshtastic_Telemetry telemetry = meshtastic_Telemetry_init_zero;
	uint8_t payload[MESHTASTIC_MAX_PAYLOAD_LEN];
	pb_ostream_t stream;
	int ret;

	ret = meshtastic_collect_device_metrics(&metrics);
	if (ret < 0) {
		meshtastic_emit_event(MESHTASTIC_EVENT_METRICS_ERROR, ret, NULL);
		return ret;
	}

	telemetry.which_variant = meshtastic_Telemetry_device_metrics_tag;
	telemetry.variant.device_metrics = metrics;

	stream = pb_ostream_from_buffer(payload, sizeof(payload));
	if (!pb_encode(&stream, meshtastic_Telemetry_fields, &telemetry)) {
		const char *err = PB_GET_ERROR(&stream);

		LOG_ERR("Telemetry encode failed: %s", err);
		meshtastic_emit_event(MESHTASTIC_EVENT_METRICS_ERROR, -ENOMEM, NULL);
		return -ENOMEM;
	}

	return meshtastic_send_data(dest, MESHTASTIC_PORT_TELEMETRY, payload, stream.bytes_written,
				    wait);
}

#if defined(CONFIG_MESHTASTIC_TELEMETRY_WANT_RESPONSE)

#define DEVICE_TELEMETRY_PEER_SLOTS 4

struct device_telemetry_peer {
	uint32_t from;
	int64_t last_reply_ms;
	bool reply_time_valid;
};

static struct {
	struct k_mutex lock;
	struct device_telemetry_peer peers[DEVICE_TELEMETRY_PEER_SLOTS];
} device_telemetry_state;

static bool interval_elapsed(bool valid, int64_t last_ms, int64_t now_ms, int64_t interval_ms)
{
	return !valid || (now_ms - last_ms) >= interval_ms;
}

static struct device_telemetry_peer *peer_get_locked(uint32_t from, int64_t now_ms)
{
	struct device_telemetry_peer *oldest = &device_telemetry_state.peers[0];
	int64_t oldest_ms = INT64_MAX;

	for (size_t i = 0; i < ARRAY_SIZE(device_telemetry_state.peers); i++) {
		struct device_telemetry_peer *p = &device_telemetry_state.peers[i];

		if (p->from == from) {
			return p;
		}

		if (!p->reply_time_valid) {
			p->from = from;
			p->reply_time_valid = false;
			return p;
		}

		if (p->last_reply_ms < oldest_ms) {
			oldest_ms = p->last_reply_ms;
			oldest = p;
		}
	}

	oldest->from = from;
	oldest->reply_time_valid = false;
	return oldest;
}

static int meshtastic_module_device_telemetry_alloc_reply(const struct meshtastic_packet *req,
							  struct meshtastic_packet *reply)
{
	static uint8_t payload[MESHTASTIC_MAX_PAYLOAD_LEN];
	meshtastic_Telemetry request = meshtastic_Telemetry_init_zero;
	meshtastic_Telemetry telemetry = meshtastic_Telemetry_init_zero;
	meshtastic_DeviceMetrics metrics;
	struct device_telemetry_peer *peer;
	int64_t now_ms;
	int ret;

	if (req == NULL || reply == NULL || req->from == 0U ||
	    req->from == meshtastic_get_node_id()) {
		return -EINVAL;
	}

	if (!meshtastic_telemetry_decode_request(req, &request)) {
		return -ENOENT;
	}

	if (request.which_variant != meshtastic_Telemetry_device_metrics_tag &&
	    request.which_variant != 0U) {
		return -ENOENT;
	}

	now_ms = k_uptime_get();
	k_mutex_lock(&device_telemetry_state.lock, K_FOREVER);
	peer = peer_get_locked(req->from, now_ms);
	if (!interval_elapsed(peer->reply_time_valid, peer->last_reply_ms, now_ms,
			      (int64_t)CONFIG_MESHTASTIC_TELEMETRY_REPLY_SUPPRESS_SEC *
				      MSEC_PER_SEC)) {
		k_mutex_unlock(&device_telemetry_state.lock);
		return -ENOENT;
	}
	peer->reply_time_valid = true;
	peer->last_reply_ms = now_ms;
	k_mutex_unlock(&device_telemetry_state.lock);

	ret = meshtastic_collect_device_metrics(&metrics);
	if (ret < 0) {
		return ret;
	}

	telemetry.which_variant = meshtastic_Telemetry_device_metrics_tag;
	telemetry.variant.device_metrics = metrics;

	ret = meshtastic_telemetry_encode_packet(req->from, req->channel, req->id, &telemetry,
						 payload, reply);
	if (ret == 0) {
		LOG_INF("Device telemetry request from 0x%08x, sending response", req->from);
	}

	return ret;
}

MESHTASTIC_MODULE_DEFINE(device_telemetry, MESHTASTIC_PORT_TELEMETRY, 0, NULL,
			 meshtastic_module_device_telemetry_alloc_reply);

#endif /* CONFIG_MESHTASTIC_TELEMETRY_WANT_RESPONSE */

#if defined(CONFIG_MESHTASTIC_DEVICE_METRICS_AUTO_SEND) ||                                         \
	(defined(CONFIG_MESHTASTIC_ENVIRONMENT_METRICS) &&                                         \
	 defined(CONFIG_MESHTASTIC_ENVIRONMENT_METRICS_AUTO_SEND))
static K_THREAD_STACK_DEFINE(telemetry_stack, 2048);
static struct k_thread telemetry_thread;

static uint32_t telemetry_period_sec(void)
{
	uint32_t period = UINT32_MAX;

#if defined(CONFIG_MESHTASTIC_DEVICE_METRICS_AUTO_SEND)
	period = MIN(period, (uint32_t)CONFIG_MESHTASTIC_DEVICE_METRICS_INTERVAL_SEC);
#endif
#if defined(CONFIG_MESHTASTIC_ENVIRONMENT_METRICS) &&                                              \
	defined(CONFIG_MESHTASTIC_ENVIRONMENT_METRICS_AUTO_SEND)
	period = MIN(period, (uint32_t)CONFIG_MESHTASTIC_ENVIRONMENT_METRICS_INTERVAL_SEC);
#endif

	return period;
}

static void telemetry_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		k_sleep(K_SECONDS(telemetry_period_sec()));

#if defined(CONFIG_MESHTASTIC_DEVICE_METRICS_AUTO_SEND)
		(void)meshtastic_send_device_metrics(MESHTASTIC_NODE_BROADCAST, K_NO_WAIT);
#endif
#if defined(CONFIG_MESHTASTIC_ENVIRONMENT_METRICS) &&                                              \
	defined(CONFIG_MESHTASTIC_ENVIRONMENT_METRICS_AUTO_SEND)
		(void)meshtastic_send_environment(MESHTASTIC_NODE_BROADCAST, K_NO_WAIT);
#endif
	}
}
#endif

int meshtastic_metrics_init(void)
{
#if defined(CONFIG_MESHTASTIC_AIRTIME)
	int ret;

	ret = meshtastic_airtime_init();
	if (ret < 0) {
		return ret;
	}
#endif

#if defined(CONFIG_MESHTASTIC_TELEMETRY_WANT_RESPONSE)
	k_mutex_init(&device_telemetry_state.lock);
#endif

#if defined(CONFIG_MESHTASTIC_DEVICE_METRICS_AUTO_SEND) ||                                         \
	(defined(CONFIG_MESHTASTIC_ENVIRONMENT_METRICS) &&                                         \
	 defined(CONFIG_MESHTASTIC_ENVIRONMENT_METRICS_AUTO_SEND))
	k_thread_create(&telemetry_thread, telemetry_stack, K_THREAD_STACK_SIZEOF(telemetry_stack),
			telemetry_thread_fn, NULL, NULL, NULL, CONFIG_MESHTASTIC_THREAD_PRIORITY, 0,
			K_NO_WAIT);
	k_thread_name_set(&telemetry_thread, "meshtastic_telemetry");
#endif

	return 0;
}
