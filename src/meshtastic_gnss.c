/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gnss.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <pb_decode.h>
#include <pb_encode.h>

#include "meshtastic_gnss.h"
#include "meshtastic_modules.h"

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(meshtastic, CONFIG_MESHTASTIC_LOG_LEVEL);

#if DT_NODE_HAS_STATUS(DT_ALIAS(gnss), okay)
#define MESHTASTIC_HAS_GNSS_ALIAS 1
#define MESHTASTIC_GNSS_NODE      DT_ALIAS(gnss)
static const struct device *const gnss_dev = DEVICE_DT_GET(MESHTASTIC_GNSS_NODE);
#else
#define MESHTASTIC_HAS_GNSS_ALIAS 0
#endif

static struct {
	struct gnss_data data;
	struct k_mutex lock;
	bool has_fix;
	uint32_t seq;
	int64_t last_sent_ms;
	int64_t last_attempt_ms;
	int64_t last_reply_ms;
	bool reply_time_valid;
} gnss_state;

static uint32_t mdeg_to_centideg(uint32_t bearing_mdeg)
{
	return bearing_mdeg / 10U;
}

static uint32_t hdop_to_centidop(uint32_t hdop_milli)
{
	return hdop_milli / 10U;
}

static void fill_position(const struct gnss_data *data, meshtastic_Position *position)
{
	*position = (meshtastic_Position)meshtastic_Position_init_zero;

	position->has_latitude_i = true;
	position->latitude_i = (int32_t)(data->nav_data.latitude / 100);
	position->has_longitude_i = true;
	position->longitude_i = (int32_t)(data->nav_data.longitude / 100);
	position->has_altitude = true;
	position->altitude = data->nav_data.altitude / 1000;
	position->location_source = meshtastic_Position_LocSource_LOC_INTERNAL;
	position->altitude_source = meshtastic_Position_AltSource_ALT_INTERNAL;
	position->HDOP = hdop_to_centidop(data->info.hdop);
	position->fix_quality = data->info.fix_quality;
	position->fix_type = (data->info.fix_status == GNSS_FIX_STATUS_DGNSS_FIX) ? 3U
			     : (data->info.fix_status == GNSS_FIX_STATUS_NO_FIX)  ? 0U
										  : 2U;
	position->sats_in_view = data->info.satellites_cnt;
	position->next_update = CONFIG_MESHTASTIC_GNSS_SEND_INTERVAL_SEC;
	position->precision_bits = 32U;

	if (data->nav_data.speed != 0U) {
		position->has_ground_speed = true;
		position->ground_speed = data->nav_data.speed / 1000U;
	}

	if (data->nav_data.bearing != 0U) {
		position->has_ground_track = true;
		position->ground_track = mdeg_to_centideg(data->nav_data.bearing);
	}

	if (data->info.geoid_separation != 0) {
		position->has_altitude_geoidal_separation = true;
		position->altitude_geoidal_separation = data->info.geoid_separation / 1000;
	}
}

static int position_build_packet(uint32_t dest, bool want_response, uint8_t channel,
				 uint32_t response_to_id, uint8_t *payload,
				 struct meshtastic_packet *packet)
{
	meshtastic_Position position;
	struct gnss_data data;
	pb_ostream_t stream;
	uint32_t seq;

	k_mutex_lock(&gnss_state.lock, K_FOREVER);
	if (!gnss_state.has_fix) {
		k_mutex_unlock(&gnss_state.lock);
		return -ENODATA;
	}
	data = gnss_state.data;
	gnss_state.seq++;
	seq = gnss_state.seq;
	k_mutex_unlock(&gnss_state.lock);

	fill_position(&data, &position);
	position.seq_number = seq;

	stream = pb_ostream_from_buffer(payload, MESHTASTIC_MAX_PAYLOAD_LEN);
	if (!pb_encode(&stream, meshtastic_Position_fields, &position)) {
		const char *err = PB_GET_ERROR(&stream);

		LOG_ERR("Position encode failed: %s", err);
		return -ENOMEM;
	}

	*packet = (struct meshtastic_packet){
		.to = dest,
		.portnum = MESHTASTIC_PORT_POSITION,
		.payload = payload,
		.payload_len = stream.bytes_written,
		.channel = channel,
		.want_response = want_response,
		.request_id = response_to_id,
	};

	return 0;
}

int meshtastic_gnss_get_last_position(meshtastic_Position *position)
{
	if (position == NULL) {
		return -EINVAL;
	}

#if !MESHTASTIC_HAS_GNSS_ALIAS
	return -ENOTSUP;
#else
	struct gnss_data data;

	k_mutex_lock(&gnss_state.lock, K_FOREVER);
	if (!gnss_state.has_fix) {
		k_mutex_unlock(&gnss_state.lock);
		return -ENODATA;
	}
	data = gnss_state.data;
	k_mutex_unlock(&gnss_state.lock);

	fill_position(&data, position);
	return 0;
#endif
}

int meshtastic_send_position(uint32_t dest)
{
	uint8_t payload[MESHTASTIC_MAX_PAYLOAD_LEN];
	struct meshtastic_packet packet;
	int ret;

	ret = position_build_packet(dest, false, 0U, 0U, payload, &packet);
	if (ret < 0) {
		return ret;
	}

	return meshtastic_send_packet(&packet, K_FOREVER);
}

static bool packet_decode_position(const struct meshtastic_packet *packet,
				   meshtastic_Position *position)
{
	pb_istream_t stream;

	if (packet->payload == NULL || packet->payload_len == 0U) {
		return false;
	}

	stream = pb_istream_from_buffer(packet->payload, packet->payload_len);
	if (!pb_decode(&stream, meshtastic_Position_fields, position)) {
		const char *err = PB_GET_ERROR(&stream);

		LOG_DBG("Position decode failed: %s", err);
		return false;
	}

	return true;
}

static void log_position(uint32_t from, uint32_t request_id, const meshtastic_Position *position)
{
	if (request_id != 0U) {
		LOG_INF("Position reply from 0x%08x (request_id 0x%08x): lat=%d lon=%d alt=%d "
			"sats=%u",
			from, request_id, position->has_latitude_i ? position->latitude_i : 0,
			position->has_longitude_i ? position->longitude_i : 0,
			position->has_altitude ? position->altitude : 0, position->sats_in_view);
	} else {
		LOG_INF("Position from 0x%08x: lat=%d lon=%d alt=%d sats=%u", from,
			position->has_latitude_i ? position->latitude_i : 0,
			position->has_longitude_i ? position->longitude_i : 0,
			position->has_altitude ? position->altitude : 0, position->sats_in_view);
	}
}

static void meshtastic_module_position_on_packet(const struct meshtastic_packet *packet)
{
	meshtastic_Position position = meshtastic_Position_init_zero;

	if (packet == NULL || packet->from == 0U || packet->from == meshtastic_get_node_id()) {
		return;
	}

	/*
	 * want_response Position packets are requests (often empty payload), not
	 * position updates — stock clients must not treat them as lat/lon data.
	 */
	if (packet->want_response) {
		return;
	}

	if (!packet_decode_position(packet, &position)) {
		if (packet->request_id != 0U) {
			LOG_WRN("Position reply from 0x%08x (request_id 0x%08x) decode failed",
				packet->from, packet->request_id);
		}
		return;
	}

	log_position(packet->from, packet->request_id, &position);
}

static bool interval_elapsed(bool valid, int64_t last_ms, int64_t now_ms, int64_t interval_ms)
{
	return !valid || (now_ms - last_ms) >= interval_ms;
}

static int meshtastic_module_position_alloc_reply(const struct meshtastic_packet *req,
						  struct meshtastic_packet *reply)
{
	static uint8_t payload[MESHTASTIC_MAX_PAYLOAD_LEN];
	int64_t now_ms;
	int ret;

	if (req == NULL || reply == NULL || req->from == 0U ||
	    req->from == meshtastic_get_node_id()) {
		return -EINVAL;
	}

	now_ms = k_uptime_get();
	k_mutex_lock(&gnss_state.lock, K_FOREVER);
	if (!interval_elapsed(gnss_state.reply_time_valid, gnss_state.last_reply_ms, now_ms,
			      (int64_t)CONFIG_MESHTASTIC_GNSS_REPLY_SUPPRESS_SEC * MSEC_PER_SEC)) {
		k_mutex_unlock(&gnss_state.lock);
		return -ENOENT;
	}
	gnss_state.reply_time_valid = true;
	gnss_state.last_reply_ms = now_ms;
	k_mutex_unlock(&gnss_state.lock);

	ret = position_build_packet(req->from, false, req->channel, req->id, payload, reply);
	if (ret == -ENODATA) {
		LOG_DBG("Position request from 0x%08x ignored (no fix)", req->from);
		return -ENOENT;
	}

	if (ret == 0) {
		LOG_INF("Position request from 0x%08x, sending response", req->from);
	}

	return ret;
}

MESHTASTIC_MODULE_DEFINE(position, MESHTASTIC_PORT_POSITION, 0,
			 meshtastic_module_position_on_packet,
			 meshtastic_module_position_alloc_reply);

#if MESHTASTIC_HAS_GNSS_ALIAS
#if defined(CONFIG_MESHTASTIC_GNSS_AUTO_SEND)
K_THREAD_STACK_DEFINE(gnss_send_wq_stack, CONFIG_MESHTASTIC_GNSS_SEND_WORK_STACK_SIZE);
static struct k_work_q gnss_send_wq;
#endif

static void position_work_handler(struct k_work *work)
{
	int ret;

	ARG_UNUSED(work);

	ret = meshtastic_send_position(MESHTASTIC_NODE_BROADCAST);
	if (ret == -ENODATA) {
		return;
	}
	if (ret < 0) {
		LOG_ERR("Position TX failed (%d)", ret);
		return;
	}

	k_mutex_lock(&gnss_state.lock, K_FOREVER);
	gnss_state.last_sent_ms = k_uptime_get();
	k_mutex_unlock(&gnss_state.lock);
}

static K_WORK_DEFINE(position_send_work, position_work_handler);

static void gnss_data_cb(const struct device *dev, const struct gnss_data *data)
{
	int64_t now;
	int64_t send_interval_ms;
	int64_t retry_interval_ms;
	bool due;
	bool can_retry;

	if (dev != gnss_dev || data == NULL || data->info.fix_status == GNSS_FIX_STATUS_NO_FIX) {
		return;
	}

	send_interval_ms = (int64_t)CONFIG_MESHTASTIC_GNSS_SEND_INTERVAL_SEC * MSEC_PER_SEC;
	retry_interval_ms = (int64_t)CONFIG_MESHTASTIC_GNSS_RETRY_INTERVAL_SEC * MSEC_PER_SEC;

	k_mutex_lock(&gnss_state.lock, K_FOREVER);
	gnss_state.data = *data;
	gnss_state.has_fix = true;
	now = k_uptime_get();

	due = (now - gnss_state.last_sent_ms) >= send_interval_ms;
	can_retry = (now - gnss_state.last_attempt_ms) >= retry_interval_ms;

	if (IS_ENABLED(CONFIG_MESHTASTIC_GNSS_AUTO_SEND) && due && can_retry &&
	    !k_work_busy_get(&position_send_work)) {
		gnss_state.last_attempt_ms = now;
		k_work_submit_to_queue(&gnss_send_wq, &position_send_work);
	}
	k_mutex_unlock(&gnss_state.lock);

	meshtastic_emit_event(MESHTASTIC_EVENT_GNSS_FIX, 0, NULL);
}

GNSS_DT_DATA_CALLBACK_DEFINE(MESHTASTIC_GNSS_NODE, gnss_data_cb);
#endif

int meshtastic_gnss_init(void)
{
	k_mutex_init(&gnss_state.lock);
	gnss_state.last_sent_ms =
		-((int64_t)CONFIG_MESHTASTIC_GNSS_SEND_INTERVAL_SEC * MSEC_PER_SEC);
	gnss_state.last_attempt_ms =
		-((int64_t)CONFIG_MESHTASTIC_GNSS_RETRY_INTERVAL_SEC * MSEC_PER_SEC);

#if MESHTASTIC_HAS_GNSS_ALIAS
	if (IS_ENABLED(CONFIG_MESHTASTIC_GNSS_AUTO_SEND)) {
		k_work_queue_start(&gnss_send_wq, gnss_send_wq_stack,
				   K_THREAD_STACK_SIZEOF(gnss_send_wq_stack),
				   CONFIG_MESHTASTIC_GNSS_SEND_WORK_PRIORITY, NULL);
	}

	if (!device_is_ready(gnss_dev)) {
		LOG_WRN("GNSS alias exists but device is not ready");
		return 0;
	}

	LOG_INF("Meshtastic position module using %s", gnss_dev->name);
#else
	LOG_WRN("CONFIG_MESHTASTIC_GNSS enabled but no ready gnss alias exists");
#endif

	return 0;
}
