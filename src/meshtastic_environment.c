/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

#include <errno.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <pb_encode.h>

#include "meshtastic_core.h"
#include "meshtastic_modules.h"
#include "meshtastic_telemetry_internal.h"

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(meshtastic, CONFIG_MESHTASTIC_LOG_LEVEL);

/*
 * Each EnvironmentMetrics quantity is compiled in only when its devicetree
 * alias exists and is "okay".  The alias names follow the in-tree sensor
 * conventions; several may point at the same physical device (for example a
 * single BME280 backing ambient_temp0, humidity0 and pressure_sensor).
 */
#if DT_NODE_HAS_STATUS(DT_ALIAS(ambient_temp0), okay)
#define MT_ENV_HAS_TEMP  1
#define MT_ENV_TEMP_NODE DT_ALIAS(ambient_temp0)
#elif DT_NODE_HAS_STATUS(DT_ALIAS(die_temp0), okay)
#define MT_ENV_HAS_TEMP  1
#define MT_ENV_TEMP_NODE DT_ALIAS(die_temp0)
#else
#define MT_ENV_HAS_TEMP 0
#endif

#if MT_ENV_HAS_TEMP
static const struct device *const mt_env_temp = DEVICE_DT_GET(MT_ENV_TEMP_NODE);
#endif

#if DT_NODE_HAS_STATUS(DT_ALIAS(humidity0), okay)
#define MT_ENV_HAS_HUM 1
static const struct device *const mt_env_hum = DEVICE_DT_GET(DT_ALIAS(humidity0));
#else
#define MT_ENV_HAS_HUM 0
#endif

#if DT_NODE_HAS_STATUS(DT_ALIAS(pressure_sensor), okay)
#define MT_ENV_HAS_PRESS 1
static const struct device *const mt_env_press = DEVICE_DT_GET(DT_ALIAS(pressure_sensor));
#else
#define MT_ENV_HAS_PRESS 0
#endif

#if DT_NODE_HAS_STATUS(DT_ALIAS(gas_sensor), okay)
#define MT_ENV_HAS_GAS 1
static const struct device *const mt_env_gas = DEVICE_DT_GET(DT_ALIAS(gas_sensor));
#else
#define MT_ENV_HAS_GAS 0
#endif

#if DT_NODE_HAS_STATUS(DT_ALIAS(light_sensor), okay)
#define MT_ENV_HAS_LIGHT 1
static const struct device *const mt_env_light = DEVICE_DT_GET(DT_ALIAS(light_sensor));
#else
#define MT_ENV_HAS_LIGHT 0
#endif

#define MT_ENV_ANY                                                                                 \
	(MT_ENV_HAS_TEMP || MT_ENV_HAS_HUM || MT_ENV_HAS_PRESS || MT_ENV_HAS_GAS ||                \
	 MT_ENV_HAS_LIGHT)

#if MT_ENV_ANY
static bool mt_env_read(const struct device *dev, enum sensor_channel chan, float scale, float *out)
{
	struct sensor_value v;

	if (!device_is_ready(dev)) {
		return false;
	}
	if (sensor_sample_fetch_chan(dev, chan) < 0) {
		return false;
	}
	if (sensor_channel_get(dev, chan, &v) < 0) {
		return false;
	}

	*out = sensor_value_to_float(&v) * scale;
	return true;
}
#endif /* MT_ENV_ANY */

int meshtastic_collect_environment(meshtastic_EnvironmentMetrics *m)
{
	if (m == NULL) {
		return -EINVAL;
	}

	*m = (meshtastic_EnvironmentMetrics)meshtastic_EnvironmentMetrics_init_zero;

#if MT_ENV_ANY
	int count = 0;

#if MT_ENV_HAS_TEMP
	/* Degrees C. Prefer ambient; SoC die sensors (e.g. esp32-temp) use DIE_TEMP. */
	if (mt_env_read(mt_env_temp, SENSOR_CHAN_AMBIENT_TEMP, 1.0f, &m->temperature) ||
	    mt_env_read(mt_env_temp, SENSOR_CHAN_DIE_TEMP, 1.0f, &m->temperature)) {
		m->has_temperature = true;
		LOG_DBG("Collected temperature: %f °C", (double)m->temperature);
		count++;
	}
#endif
#if MT_ENV_HAS_HUM
	/* Zephyr humidity is percent RH; Meshtastic field is percent RH. */
	if (mt_env_read(mt_env_hum, SENSOR_CHAN_HUMIDITY, 1.0f, &m->relative_humidity)) {
		m->has_relative_humidity = true;
		LOG_DBG("Collected relative humidity: %f %%", (double)m->relative_humidity);
		count++;
	}
#endif
#if MT_ENV_HAS_PRESS
	/* Zephyr pressure is kPa; Meshtastic barometric_pressure is hPa. */
	if (mt_env_read(mt_env_press, SENSOR_CHAN_PRESS, 10.0f, &m->barometric_pressure)) {
		m->has_barometric_pressure = true;
		LOG_DBG("Collected barometric pressure: %f hPa", (double)m->barometric_pressure);
		count++;
	}
#endif
#if MT_ENV_HAS_GAS
	/* Zephyr gas resistance is ohms; Meshtastic gas_resistance is MOhm. */
	if (mt_env_read(mt_env_gas, SENSOR_CHAN_GAS_RES, 1.0f / 1000000.0f, &m->gas_resistance)) {
		m->has_gas_resistance = true;
		LOG_DBG("Collected gas resistance: %f MOhm", (double)m->gas_resistance);
		count++;
	}
#endif
#if MT_ENV_HAS_LIGHT
	/* Zephyr illuminance is lux; Meshtastic lux field is Lux. */
	if (mt_env_read(mt_env_light, SENSOR_CHAN_LIGHT, 1.0f, &m->lux)) {
		m->has_lux = true;
		LOG_DBG("Collected lux: %f Lux", (double)m->lux);
		count++;
	}
#endif
	return (count == 0) ? -ENODEV : count;
#else
	return -ENODEV;
#endif /* MT_ENV_ANY */
}

int meshtastic_send_environment(uint32_t dest, k_timeout_t wait)
{
	meshtastic_EnvironmentMetrics env;
	meshtastic_Telemetry telemetry = meshtastic_Telemetry_init_zero;
	uint8_t payload[MESHTASTIC_MAX_PAYLOAD_LEN];
	pb_ostream_t stream;
	int ret;

	ret = meshtastic_collect_environment(&env);
	if (ret < 0) {
		meshtastic_emit_event(MESHTASTIC_EVENT_METRICS_ERROR, ret, NULL);
		return ret;
	}

	telemetry.which_variant = meshtastic_Telemetry_environment_metrics_tag;
	telemetry.variant.environment_metrics = env;

	stream = pb_ostream_from_buffer(payload, sizeof(payload));
	if (!pb_encode(&stream, meshtastic_Telemetry_fields, &telemetry)) {
		const char *err = PB_GET_ERROR(&stream);

		LOG_ERR("Environment telemetry encode failed: %s", err);
		meshtastic_emit_event(MESHTASTIC_EVENT_METRICS_ERROR, -ENOMEM, NULL);
		return -ENOMEM;
	}

	return meshtastic_send_data(dest, MESHTASTIC_PORT_TELEMETRY, payload, stream.bytes_written,
				    wait);
}

#if defined(CONFIG_MESHTASTIC_TELEMETRY_WANT_RESPONSE)

#define ENV_TELEMETRY_PEER_SLOTS 4

struct env_telemetry_peer {
	uint32_t from;
	int64_t last_reply_ms;
	bool reply_time_valid;
};

static struct {
	struct k_mutex lock;
	struct env_telemetry_peer peers[ENV_TELEMETRY_PEER_SLOTS];
} env_telemetry_state;

static bool interval_elapsed(bool valid, int64_t last_ms, int64_t now_ms, int64_t interval_ms)
{
	return !valid || (now_ms - last_ms) >= interval_ms;
}

static struct env_telemetry_peer *peer_get_locked(uint32_t from, int64_t now_ms)
{
	struct env_telemetry_peer *oldest = &env_telemetry_state.peers[0];
	int64_t oldest_ms = INT64_MAX;

	for (size_t i = 0; i < ARRAY_SIZE(env_telemetry_state.peers); i++) {
		struct env_telemetry_peer *p = &env_telemetry_state.peers[i];

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

static int meshtastic_module_environment_telemetry_alloc_reply(const struct meshtastic_packet *req,
							       struct meshtastic_packet *reply)
{
	static uint8_t payload[MESHTASTIC_MAX_PAYLOAD_LEN];
	meshtastic_Telemetry request = meshtastic_Telemetry_init_zero;
	meshtastic_Telemetry telemetry = meshtastic_Telemetry_init_zero;
	meshtastic_EnvironmentMetrics env;
	struct env_telemetry_peer *peer;
	int64_t now_ms;
	int ret;

	if (req == NULL || reply == NULL || req->from == 0U ||
	    req->from == meshtastic_get_node_id()) {
		return -EINVAL;
	}

	if (!meshtastic_telemetry_decode_request(req, &request)) {
		return -ENOENT;
	}

	if (request.which_variant != meshtastic_Telemetry_environment_metrics_tag) {
		return -ENOENT;
	}

	now_ms = k_uptime_get();
	k_mutex_lock(&env_telemetry_state.lock, K_FOREVER);
	peer = peer_get_locked(req->from, now_ms);
	if (!interval_elapsed(peer->reply_time_valid, peer->last_reply_ms, now_ms,
			      (int64_t)CONFIG_MESHTASTIC_TELEMETRY_REPLY_SUPPRESS_SEC *
				      MSEC_PER_SEC)) {
		k_mutex_unlock(&env_telemetry_state.lock);
		return -ENOENT;
	}
	peer->reply_time_valid = true;
	peer->last_reply_ms = now_ms;
	k_mutex_unlock(&env_telemetry_state.lock);

	ret = meshtastic_collect_environment(&env);
	if (ret < 0) {
		return -ENOENT;
	}

	telemetry.which_variant = meshtastic_Telemetry_environment_metrics_tag;
	telemetry.variant.environment_metrics = env;

	ret = meshtastic_telemetry_encode_packet(req->from, req->channel, req->id, &telemetry,
						 payload, reply);
	if (ret == 0) {
		LOG_INF("Environment telemetry request from 0x%08x, sending response", req->from);
	}

	return ret;
}

MESHTASTIC_MODULE_DEFINE(environment_telemetry, MESHTASTIC_PORT_TELEMETRY, 0, NULL,
			 meshtastic_module_environment_telemetry_alloc_reply);

#endif /* CONFIG_MESHTASTIC_TELEMETRY_WANT_RESPONSE */

#if MT_ENV_ANY && defined(CONFIG_MESHTASTIC_ENVIRONMENT_METRICS_AUTO_SEND) &&                      \
	!defined(CONFIG_MESHTASTIC_DEVICE_METRICS)
static K_THREAD_STACK_DEFINE(env_stack, 2048);
static struct k_thread env_thread;

static void env_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		k_sleep(K_SECONDS(CONFIG_MESHTASTIC_ENVIRONMENT_METRICS_INTERVAL_SEC));
		(void)meshtastic_send_environment(MESHTASTIC_NODE_BROADCAST, K_NO_WAIT);
	}
}
#endif

int meshtastic_environment_init(void)
{
#if defined(CONFIG_MESHTASTIC_TELEMETRY_WANT_RESPONSE)
	k_mutex_init(&env_telemetry_state.lock);
#endif

#if MT_ENV_ANY
#if defined(CONFIG_MESHTASTIC_ENVIRONMENT_METRICS_AUTO_SEND) &&                                    \
	!defined(CONFIG_MESHTASTIC_DEVICE_METRICS)
	k_thread_create(&env_thread, env_stack, K_THREAD_STACK_SIZEOF(env_stack), env_thread_fn,
			NULL, NULL, NULL, CONFIG_MESHTASTIC_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&env_thread, "meshtastic_env");
#endif
#else
	LOG_WRN("CONFIG_MESHTASTIC_ENVIRONMENT_METRICS enabled but no "
		"environment sensor alias exists");
#endif

	return 0;
}
