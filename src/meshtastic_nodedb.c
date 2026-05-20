/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <pb_decode.h>

#include <zephyr/meshtastic/meshtastic.h>
#include <zephyr/meshtastic/nodedb.h>

#include "meshtastic_modules.h"

#include "meshtastic/deviceonly.pb.h"

LOG_MODULE_DECLARE(meshtastic, CONFIG_MESHTASTIC_LOG_LEVEL);

/* NodeInfoLite.bitfield bit indices (Meshtastic upstream layout). */
#define NODEINFO_BITFIELD_VIA_MQTT_BIT            2
#define NODEINFO_BITFIELD_HAS_USER_BIT            5
#define NODEINFO_BITFIELD_IS_LICENSED_BIT         6
#define NODEINFO_BITFIELD_IS_UNMESSAGABLE_BIT     7
#define NODEINFO_BITFIELD_HAS_IS_UNMESSAGABLE_BIT 8

struct nodedb_entry {
	bool used;
	meshtastic_NodeInfoLite node;
	bool has_position;
	meshtastic_PositionLite position;
	bool has_device_metrics;
	meshtastic_DeviceMetrics device_metrics;
	bool has_environment_metrics;
	meshtastic_EnvironmentMetrics environment_metrics;
	bool has_status;
	meshtastic_StatusMessage status;
};

static K_MUTEX_DEFINE(nodedb_lock);
static struct nodedb_entry nodedb_entries[CONFIG_MESHTASTIC_NODEDB_MAX_NODES];
static size_t nodedb_entry_count;

static uint32_t uptime_seconds(void)
{
	return (uint32_t)(k_uptime_get() / MSEC_PER_SEC);
}

static void copy_string(char *dst, size_t dst_len, const char *src)
{
	if (dst_len == 0U) {
		return;
	}

	dst[0] = '\0';
	if (src != NULL) {
		strncpy(dst, src, dst_len - 1U);
		dst[dst_len - 1U] = '\0';
	}
}

static void apply_user(struct nodedb_entry *entry, const meshtastic_User *user)
{
	meshtastic_NodeInfoLite *node = &entry->node;
	size_t key_len;

	copy_string(node->long_name, sizeof(node->long_name), user->long_name);
	copy_string(node->short_name, sizeof(node->short_name), user->short_name);
	node->hw_model = (uint8_t)user->hw_model;
	node->role = (uint8_t)user->role;

	key_len = MIN((size_t)user->public_key.size, sizeof(node->public_key.bytes));
	node->public_key.size = (pb_size_t)key_len;
	if (key_len > 0U) {
		memcpy(node->public_key.bytes, user->public_key.bytes, key_len);
	}

	WRITE_BIT(node->bitfield, NODEINFO_BITFIELD_HAS_USER_BIT, true);
	WRITE_BIT(node->bitfield, NODEINFO_BITFIELD_IS_LICENSED_BIT, user->is_licensed);
	WRITE_BIT(node->bitfield, NODEINFO_BITFIELD_HAS_IS_UNMESSAGABLE_BIT,
		  user->has_is_unmessagable);
	WRITE_BIT(node->bitfield, NODEINFO_BITFIELD_IS_UNMESSAGABLE_BIT,
		  user->has_is_unmessagable && user->is_unmessagable);
}

static void apply_position(struct nodedb_entry *entry, const meshtastic_Position *position)
{
	entry->has_position = true;
	entry->position.latitude_i = position->has_latitude_i ? position->latitude_i : 0;
	entry->position.longitude_i = position->has_longitude_i ? position->longitude_i : 0;
	entry->position.altitude = position->has_altitude ? position->altitude : 0;
	entry->position.time = position->time;
	entry->position.location_source = position->location_source;
	entry->position.precision_bits = position->precision_bits;
}

static struct nodedb_entry *find_entry_locked(uint32_t node_num)
{
	for (size_t i = 0U; i < nodedb_entry_count; i++) {
		if (nodedb_entries[i].used && nodedb_entries[i].node.num == node_num) {
			return &nodedb_entries[i];
		}
	}

	return NULL;
}

static size_t oldest_evictable_index_locked(void)
{
	uint32_t local = meshtastic_get_node_id();
	uint32_t oldest = UINT32_MAX;
	size_t oldest_index = SIZE_MAX;

	for (size_t i = 0U; i < nodedb_entry_count; i++) {
		if (!nodedb_entries[i].used || nodedb_entries[i].node.num == local) {
			continue;
		}

		if (nodedb_entries[i].node.last_heard < oldest) {
			oldest = nodedb_entries[i].node.last_heard;
			oldest_index = i;
		}
	}

	return oldest_index;
}

static struct nodedb_entry *get_or_create_entry_locked(uint32_t node_num)
{
	struct nodedb_entry *entry;
	size_t index;

	if (node_num == 0U) {
		return NULL;
	}

	entry = find_entry_locked(node_num);
	if (entry != NULL) {
		return entry;
	}

	if (nodedb_entry_count < ARRAY_SIZE(nodedb_entries)) {
		entry = &nodedb_entries[nodedb_entry_count++];
	} else {
		index = oldest_evictable_index_locked();
		if (index == SIZE_MAX) {
			return NULL;
		}

		entry = &nodedb_entries[index];
		LOG_DBG("NodeDB evicting 0x%08x", entry->node.num);
	}

	*entry = (struct nodedb_entry){0};
	entry->used = true;
	entry->node = (meshtastic_NodeInfoLite)meshtastic_NodeInfoLite_init_zero;
	entry->node.num = node_num;

	return entry;
}

static bool decode_user_payload(const struct meshtastic_packet *packet, meshtastic_User *user)
{
	pb_istream_t stream;

	if (packet->payload == NULL || packet->payload_len == 0U) {
		return false;
	}

	stream = pb_istream_from_buffer(packet->payload, packet->payload_len);
	if (!pb_decode(&stream, meshtastic_User_fields, user)) {
		LOG_DBG("NodeDB User decode failed: %s", PB_GET_ERROR(&stream));
		return false;
	}

	return true;
}

static bool decode_position_payload(const struct meshtastic_packet *packet,
				    meshtastic_Position *position)
{
	pb_istream_t stream;

	if (packet->payload == NULL || packet->payload_len == 0U) {
		return false;
	}

	stream = pb_istream_from_buffer(packet->payload, packet->payload_len);
	if (!pb_decode(&stream, meshtastic_Position_fields, position)) {
		LOG_DBG("NodeDB Position decode failed: %s", PB_GET_ERROR(&stream));
		return false;
	}

	return true;
}

static bool decode_telemetry_payload(const struct meshtastic_packet *packet,
				     meshtastic_Telemetry *telemetry)
{
	pb_istream_t stream;

	if (packet->payload == NULL || packet->payload_len == 0U) {
		return false;
	}

	stream = pb_istream_from_buffer(packet->payload, packet->payload_len);
	if (!pb_decode(&stream, meshtastic_Telemetry_fields, telemetry)) {
		LOG_DBG("NodeDB Telemetry decode failed: %s", PB_GET_ERROR(&stream));
		return false;
	}

	return true;
}

static bool decode_status_payload(const struct meshtastic_packet *packet,
				  meshtastic_StatusMessage *status)
{
	pb_istream_t stream;

	if (packet->payload == NULL || packet->payload_len == 0U) {
		return false;
	}

	stream = pb_istream_from_buffer(packet->payload, packet->payload_len);
	if (!pb_decode(&stream, meshtastic_StatusMessage_fields, status)) {
		LOG_DBG("NodeDB Status decode failed: %s", PB_GET_ERROR(&stream));
		return false;
	}

	return true;
}

static bool packet_hops_away(const struct meshtastic_packet *packet, uint8_t *hops_away)
{
	if (packet->hop_start == 0U || packet->hop_start < packet->hop_limit) {
		return false;
	}

	*hops_away = packet->hop_start - packet->hop_limit;
	return true;
}

static void apply_basic_packet(struct nodedb_entry *entry, const struct meshtastic_packet *packet,
			       uint32_t now_sec)
{
	uint8_t hops_away;

	entry->node.last_heard = now_sec;
	entry->node.snr = (float)packet->snr;
	entry->node.channel = (packet->channel_index != MESHTASTIC_CHANNEL_INDEX_INVALID)
				      ? packet->channel_index
				      : 0U;
	entry->node.next_hop = packet->next_hop;
	WRITE_BIT(entry->node.bitfield, NODEINFO_BITFIELD_VIA_MQTT_BIT, packet->via_mqtt);

	if (packet_hops_away(packet, &hops_away)) {
		entry->node.has_hops_away = true;
		entry->node.hops_away = hops_away;
	}
}

static void meshtastic_module_nodedb_on_packet(const struct meshtastic_packet *packet)
{
	meshtastic_User user = meshtastic_User_init_zero;
	meshtastic_Position position = meshtastic_Position_init_zero;
	meshtastic_Telemetry telemetry = meshtastic_Telemetry_init_zero;
	meshtastic_StatusMessage status = meshtastic_StatusMessage_init_zero;
	bool has_user = false;
	bool has_position = false;
	bool has_telemetry = false;
	bool has_status = false;
	struct nodedb_entry *entry;
	uint32_t now_sec;

	if (packet == NULL || packet->from == 0U || packet->from == meshtastic_get_node_id()) {
		return;
	}

	switch (packet->portnum) {
	case MESHTASTIC_PORT_NODEINFO:
		has_user = decode_user_payload(packet, &user);
		break;
	case MESHTASTIC_PORT_POSITION:
		if (!packet->want_response) {
			has_position = decode_position_payload(packet, &position);
		}
		break;
	case MESHTASTIC_PORT_TELEMETRY:
		has_telemetry = decode_telemetry_payload(packet, &telemetry);
		break;
	case MESHTASTIC_PORT_NODE_STATUS:
		has_status = decode_status_payload(packet, &status);
		break;
	default:
		break;
	}

	now_sec = uptime_seconds();

	k_mutex_lock(&nodedb_lock, K_FOREVER);
	entry = get_or_create_entry_locked(packet->from);
	if (entry == NULL) {
		k_mutex_unlock(&nodedb_lock);
		return;
	}

	apply_basic_packet(entry, packet, now_sec);

	if (has_user) {
		apply_user(entry, &user);
	}

	if (has_position) {
		apply_position(entry, &position);
	}

	if (has_telemetry) {
		if (telemetry.which_variant == meshtastic_Telemetry_device_metrics_tag) {
			entry->has_device_metrics = true;
			entry->device_metrics = telemetry.variant.device_metrics;
		} else if (telemetry.which_variant ==
			   meshtastic_Telemetry_environment_metrics_tag) {
			entry->has_environment_metrics = true;
			entry->environment_metrics = telemetry.variant.environment_metrics;
		}
	}

	if (has_status) {
		entry->has_status = true;
		entry->status = status;
	}

	k_mutex_unlock(&nodedb_lock);
}

MESHTASTIC_MODULE_DEFINE(nodedb, 0, MESHTASTIC_MODULE_ALL_PACKETS,
			 meshtastic_module_nodedb_on_packet, NULL);

static void copy_device_metrics(struct meshtastic_nodedb_device_metrics *dst,
				const meshtastic_DeviceMetrics *src)
{
	dst->has_battery_level = src->has_battery_level;
	dst->battery_level = src->battery_level;
	dst->has_voltage = src->has_voltage;
	dst->voltage = src->voltage;
	dst->has_channel_utilization = src->has_channel_utilization;
	dst->channel_utilization = src->channel_utilization;
	dst->has_air_util_tx = src->has_air_util_tx;
	dst->air_util_tx = src->air_util_tx;
	dst->has_uptime_seconds = src->has_uptime_seconds;
	dst->uptime_seconds = src->uptime_seconds;
}

static void copy_environment_metrics(struct meshtastic_nodedb_environment_metrics *dst,
				     const meshtastic_EnvironmentMetrics *src)
{
	size_t count;

	dst->has_temperature = src->has_temperature;
	dst->temperature = src->temperature;
	dst->has_relative_humidity = src->has_relative_humidity;
	dst->relative_humidity = src->relative_humidity;
	dst->has_barometric_pressure = src->has_barometric_pressure;
	dst->barometric_pressure = src->barometric_pressure;
	dst->has_gas_resistance = src->has_gas_resistance;
	dst->gas_resistance = src->gas_resistance;
	dst->has_voltage = src->has_voltage;
	dst->voltage = src->voltage;
	dst->has_current = src->has_current;
	dst->current = src->current;
	dst->has_iaq = src->has_iaq;
	dst->iaq = src->iaq;
	dst->has_distance = src->has_distance;
	dst->distance = src->distance;
	dst->has_lux = src->has_lux;
	dst->lux = src->lux;
	dst->has_white_lux = src->has_white_lux;
	dst->white_lux = src->white_lux;
	dst->has_ir_lux = src->has_ir_lux;
	dst->ir_lux = src->ir_lux;
	dst->has_uv_lux = src->has_uv_lux;
	dst->uv_lux = src->uv_lux;
	dst->has_wind_direction = src->has_wind_direction;
	dst->wind_direction = src->wind_direction;
	dst->has_wind_speed = src->has_wind_speed;
	dst->wind_speed = src->wind_speed;
	dst->has_weight = src->has_weight;
	dst->weight = src->weight;
	dst->has_wind_gust = src->has_wind_gust;
	dst->wind_gust = src->wind_gust;
	dst->has_wind_lull = src->has_wind_lull;
	dst->wind_lull = src->wind_lull;
	dst->has_radiation = src->has_radiation;
	dst->radiation = src->radiation;
	dst->has_rainfall_1h = src->has_rainfall_1h;
	dst->rainfall_1h = src->rainfall_1h;
	dst->has_rainfall_24h = src->has_rainfall_24h;
	dst->rainfall_24h = src->rainfall_24h;
	dst->has_soil_moisture = src->has_soil_moisture;
	dst->soil_moisture = src->soil_moisture;
	dst->has_soil_temperature = src->has_soil_temperature;
	dst->soil_temperature = src->soil_temperature;

	count = MIN((size_t)src->one_wire_temperature_count, ARRAY_SIZE(dst->one_wire_temperature));
	dst->one_wire_temperature_count = count;
	if (count > 0U) {
		memcpy(dst->one_wire_temperature, src->one_wire_temperature,
		       count * sizeof(dst->one_wire_temperature[0]));
	}
}

static void fill_snapshot(const struct nodedb_entry *entry, struct meshtastic_nodedb_node *out)
{
	const meshtastic_NodeInfoLite *node = &entry->node;
	size_t key_len;

	*out = (struct meshtastic_nodedb_node){0};
	out->num = node->num;
	out->last_heard_uptime_sec = node->last_heard;
	out->snr = node->snr;
	out->channel = node->channel;
	out->next_hop = node->next_hop;
	out->via_mqtt = IS_BIT_SET(node->bitfield, NODEINFO_BITFIELD_VIA_MQTT_BIT);
	out->has_hops_away = node->has_hops_away;
	out->hops_away = node->hops_away;

	out->has_user = IS_BIT_SET(node->bitfield, NODEINFO_BITFIELD_HAS_USER_BIT);
	copy_string(out->long_name, sizeof(out->long_name), node->long_name);
	copy_string(out->short_name, sizeof(out->short_name), node->short_name);
	out->hw_model = node->hw_model;
	out->role = node->role;
	out->is_licensed = IS_BIT_SET(node->bitfield, NODEINFO_BITFIELD_IS_LICENSED_BIT);
	out->has_is_unmessagable =
		IS_BIT_SET(node->bitfield, NODEINFO_BITFIELD_HAS_IS_UNMESSAGABLE_BIT);
	out->is_unmessagable = IS_BIT_SET(node->bitfield, NODEINFO_BITFIELD_IS_UNMESSAGABLE_BIT);

	key_len = MIN((size_t)node->public_key.size, sizeof(out->public_key));
	out->public_key_len = key_len;
	if (key_len > 0U) {
		memcpy(out->public_key, node->public_key.bytes, key_len);
	}

	out->has_position = entry->has_position;
	if (entry->has_position) {
		out->position.latitude_i = entry->position.latitude_i;
		out->position.longitude_i = entry->position.longitude_i;
		out->position.altitude = entry->position.altitude;
		out->position.time = entry->position.time;
		out->position.location_source = (uint8_t)entry->position.location_source;
		out->position.precision_bits = entry->position.precision_bits;
	}

	out->has_device_metrics = entry->has_device_metrics;
	if (entry->has_device_metrics) {
		copy_device_metrics(&out->device_metrics, &entry->device_metrics);
	}

	out->has_environment_metrics = entry->has_environment_metrics;
	if (entry->has_environment_metrics) {
		copy_environment_metrics(&out->environment_metrics, &entry->environment_metrics);
	}

	out->has_status = entry->has_status;
	if (entry->has_status) {
		copy_string(out->status, sizeof(out->status), entry->status.status);
	}
}

size_t meshtastic_nodedb_count(void)
{
	size_t count;

	k_mutex_lock(&nodedb_lock, K_FOREVER);
	count = nodedb_entry_count;
	k_mutex_unlock(&nodedb_lock);

	return count;
}

int meshtastic_nodedb_get(uint32_t node_num, struct meshtastic_nodedb_node *out)
{
	struct nodedb_entry *entry;

	if (out == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&nodedb_lock, K_FOREVER);
	entry = find_entry_locked(node_num);
	if (entry == NULL) {
		k_mutex_unlock(&nodedb_lock);
		return -ENOENT;
	}

	fill_snapshot(entry, out);
	k_mutex_unlock(&nodedb_lock);

	return 0;
}

int meshtastic_nodedb_get_by_index(size_t index, struct meshtastic_nodedb_node *out)
{
	if (out == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&nodedb_lock, K_FOREVER);
	if (index >= nodedb_entry_count || !nodedb_entries[index].used) {
		k_mutex_unlock(&nodedb_lock);
		return -ENOENT;
	}

	fill_snapshot(&nodedb_entries[index], out);
	k_mutex_unlock(&nodedb_lock);

	return 0;
}

int meshtastic_nodedb_init(void)
{
	meshtastic_User user;
	struct nodedb_entry *entry;

	meshtastic_fill_user(&user);

	k_mutex_lock(&nodedb_lock, K_FOREVER);
	memset(nodedb_entries, 0, sizeof(nodedb_entries));
	nodedb_entry_count = 0U;

	entry = get_or_create_entry_locked(meshtastic_get_node_id());
	if (entry != NULL) {
		entry->node.last_heard = uptime_seconds();
		apply_user(entry, &user);
	}
	k_mutex_unlock(&nodedb_lock);

	return (entry == NULL) ? -ENOMEM : 0;
}
