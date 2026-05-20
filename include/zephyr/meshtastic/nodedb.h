/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

/**
 * @file
 * @brief Meshtastic in-RAM NodeDB public API.
 */

#ifndef ZEPHYR_INCLUDE_MESHTASTIC_NODEDB_H_
#define ZEPHYR_INCLUDE_MESHTASTIC_NODEDB_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MESHTASTIC_NODEDB_LONG_NAME_LEN            25U
#define MESHTASTIC_NODEDB_SHORT_NAME_LEN           5U
#define MESHTASTIC_NODEDB_PUBLIC_KEY_MAX_LEN       32U
#define MESHTASTIC_NODEDB_STATUS_LEN               80U
#define MESHTASTIC_NODEDB_ONE_WIRE_TEMPERATURE_MAX 8U

struct meshtastic_nodedb_position {
	int32_t latitude_i;
	int32_t longitude_i;
	int32_t altitude;
	uint32_t time;
	uint8_t location_source;
	uint32_t precision_bits;
};

struct meshtastic_nodedb_device_metrics {
	bool has_battery_level;
	uint32_t battery_level;
	bool has_voltage;
	float voltage;
	bool has_channel_utilization;
	float channel_utilization;
	bool has_air_util_tx;
	float air_util_tx;
	bool has_uptime_seconds;
	uint32_t uptime_seconds;
};

struct meshtastic_nodedb_environment_metrics {
	bool has_temperature;
	float temperature;
	bool has_relative_humidity;
	float relative_humidity;
	bool has_barometric_pressure;
	float barometric_pressure;
	bool has_gas_resistance;
	float gas_resistance;
	bool has_voltage;
	float voltage;
	bool has_current;
	float current;
	bool has_iaq;
	uint16_t iaq;
	bool has_distance;
	float distance;
	bool has_lux;
	float lux;
	bool has_white_lux;
	float white_lux;
	bool has_ir_lux;
	float ir_lux;
	bool has_uv_lux;
	float uv_lux;
	bool has_wind_direction;
	uint16_t wind_direction;
	bool has_wind_speed;
	float wind_speed;
	bool has_weight;
	float weight;
	bool has_wind_gust;
	float wind_gust;
	bool has_wind_lull;
	float wind_lull;
	bool has_radiation;
	float radiation;
	bool has_rainfall_1h;
	float rainfall_1h;
	bool has_rainfall_24h;
	float rainfall_24h;
	bool has_soil_moisture;
	uint8_t soil_moisture;
	bool has_soil_temperature;
	float soil_temperature;
	size_t one_wire_temperature_count;
	float one_wire_temperature[MESHTASTIC_NODEDB_ONE_WIRE_TEMPERATURE_MAX];
};

struct meshtastic_nodedb_node {
	uint32_t num;
	uint32_t last_heard_uptime_sec;
	float snr;
	uint8_t channel;
	uint8_t next_hop;
	bool via_mqtt;
	bool has_hops_away;
	uint8_t hops_away;

	bool has_user;
	char long_name[MESHTASTIC_NODEDB_LONG_NAME_LEN];
	char short_name[MESHTASTIC_NODEDB_SHORT_NAME_LEN];
	uint8_t hw_model;
	uint8_t role;
	bool is_licensed;
	bool has_is_unmessagable;
	bool is_unmessagable;
	size_t public_key_len;
	uint8_t public_key[MESHTASTIC_NODEDB_PUBLIC_KEY_MAX_LEN];

	bool has_position;
	struct meshtastic_nodedb_position position;

	bool has_device_metrics;
	struct meshtastic_nodedb_device_metrics device_metrics;

	bool has_environment_metrics;
	struct meshtastic_nodedb_environment_metrics environment_metrics;

	bool has_status;
	char status[MESHTASTIC_NODEDB_STATUS_LEN];
};

/**
 * @brief Return the number of entries currently stored in the in-RAM NodeDB.
 */
size_t meshtastic_nodedb_count(void);

/**
 * @brief Copy one NodeDB entry by node number.
 *
 * @retval 0 Entry copied.
 * @retval -EINVAL @p out is NULL.
 * @retval -ENOENT The node is not present.
 * @retval -ENOTSUP NodeDB support is not enabled.
 */
int meshtastic_nodedb_get(uint32_t node_num, struct meshtastic_nodedb_node *out);

/**
 * @brief Copy one NodeDB entry by table index.
 *
 * Entries are indexed from 0 to meshtastic_nodedb_count() - 1.
 *
 * @retval 0 Entry copied.
 * @retval -EINVAL @p out is NULL.
 * @retval -ENOENT @p index is out of range.
 * @retval -ENOTSUP NodeDB support is not enabled.
 */
int meshtastic_nodedb_get_by_index(size_t index, struct meshtastic_nodedb_node *out);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_MESHTASTIC_NODEDB_H_ */
