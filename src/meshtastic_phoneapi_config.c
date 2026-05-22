/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

/*
 * Meshtastic PhoneAPI configuration handshake for phone clients.
 *
 * The official Meshtastic mobile apps expect the same FromRadio sequence as
 * meshtastic_firmware/src/mesh/PhoneAPI.cpp after a ToRadio want_config_id.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include "meshtastic_channels.h"
#include "meshtastic_config_store.h"
#include "meshtastic_phoneapi.h"

#include "meshtastic/device_ui.pb.h"
#include "meshtastic/module_config.pb.h"

LOG_MODULE_DECLARE(meshtastic, CONFIG_MESHTASTIC_LOG_LEVEL);

static void fill_my_info(meshtastic_FromRadio *from)
{
	uint8_t node_id[4];

	from->id = meshtastic_next_fromradio_id();
	from->which_payload_variant = meshtastic_FromRadio_my_info_tag;
	from->my_info.my_node_num = meshtastic_get_node_id();
	from->my_info.min_app_version = 0U;
	from->my_info.nodedb_count = 1U;
	from->my_info.device_id.size = sizeof(node_id);
	sys_put_le32(meshtastic_get_node_id(), node_id);
	memcpy(from->my_info.device_id.bytes, node_id, sizeof(node_id));
	strncpy(from->my_info.pio_env, "zephyr", sizeof(from->my_info.pio_env) - 1U);
}

static void fill_node_info(meshtastic_FromRadio *from)
{
	from->id = meshtastic_next_fromradio_id();
	from->which_payload_variant = meshtastic_FromRadio_node_info_tag;
	from->node_info.num = meshtastic_get_node_id();
	from->node_info.has_user = true;
	meshtastic_fill_user(&from->node_info.user);
}

static void fill_metadata_frame(meshtastic_FromRadio *from)
{
	from->id = meshtastic_next_fromradio_id();
	from->which_payload_variant = meshtastic_FromRadio_metadata_tag;
	strncpy(from->metadata.firmware_version, "zephyr.123",
		sizeof(from->metadata.firmware_version) - 1U);
	from->metadata.hasBluetooth = IS_ENABLED(CONFIG_MESHTASTIC_BLE);
	from->metadata.role = meshtastic_device_role();
	from->metadata.hw_model = meshtastic_hw_model();
}

static void fill_channel(meshtastic_FromRadio *from, int index)
{
	meshtastic_Channel slot = meshtastic_Channel_init_zero;

	from->id = meshtastic_next_fromradio_id();
	from->which_payload_variant = meshtastic_FromRadio_channel_tag;
	from->channel.index = index;

	if (meshtastic_config_store_get_channel((uint8_t)index, &slot) == 0 &&
	    slot.role != meshtastic_Channel_Role_DISABLED) {
		from->channel = slot;
		from->channel.index = index;
	} else {
		from->channel.role = meshtastic_Channel_Role_DISABLED;
		from->channel.has_settings = true;
	}
}

static int fill_config_variant(meshtastic_FromRadio *from, pb_size_t which_tag)
{
	int ret;

	from->id = meshtastic_next_fromradio_id();
	from->which_payload_variant = meshtastic_FromRadio_config_tag;

	ret = meshtastic_config_store_get_config(which_tag, &from->config);
	if (ret < 0) {
		return ret;
	}

	return 0;
}

static int fill_module_variant(meshtastic_FromRadio *from, pb_size_t which_tag)
{
	int ret;

	from->id = meshtastic_next_fromradio_id();
	from->which_payload_variant = meshtastic_FromRadio_moduleConfig_tag;

	ret = meshtastic_config_store_get_module(which_tag, &from->moduleConfig);
	if (ret < 0) {
		return ret;
	}

	return 0;
}

static int emit_frame(struct meshtastic_phoneapi *api, const meshtastic_FromRadio *from,
		      enum meshtastic_phoneapi_config_state next_state, uint8_t next_index,
		      struct meshtastic_phoneapi_frame *frame)
{
	int ret = meshtastic_phoneapi_encode_fromradio_frame(from, frame);

	if (ret < 0) {
		return ret;
	}

	api->config_state = next_state;
	api->config_index = next_index;

	return 0;
}

int meshtastic_phoneapi_next_config_frame(struct meshtastic_phoneapi *api,
					  struct meshtastic_phoneapi_frame *frame)
{
	static const pb_size_t config_tags[] = {
		meshtastic_Config_device_tag,     meshtastic_Config_position_tag,
		meshtastic_Config_power_tag,      meshtastic_Config_network_tag,
		meshtastic_Config_display_tag,    meshtastic_Config_lora_tag,
		meshtastic_Config_bluetooth_tag,  meshtastic_Config_security_tag,
		meshtastic_Config_sessionkey_tag, meshtastic_Config_device_ui_tag,
	};
	static const pb_size_t module_tags[] = {
		meshtastic_ModuleConfig_mqtt_tag,
		meshtastic_ModuleConfig_serial_tag,
		meshtastic_ModuleConfig_external_notification_tag,
		meshtastic_ModuleConfig_store_forward_tag,
		meshtastic_ModuleConfig_range_test_tag,
		meshtastic_ModuleConfig_telemetry_tag,
		meshtastic_ModuleConfig_canned_message_tag,
		meshtastic_ModuleConfig_audio_tag,
		meshtastic_ModuleConfig_remote_hardware_tag,
		meshtastic_ModuleConfig_neighbor_info_tag,
		meshtastic_ModuleConfig_ambient_lighting_tag,
		meshtastic_ModuleConfig_detection_sensor_tag,
		meshtastic_ModuleConfig_paxcounter_tag,
		meshtastic_ModuleConfig_statusmessage_tag,
		meshtastic_ModuleConfig_traffic_management_tag,
		meshtastic_ModuleConfig_tak_tag,
	};
	meshtastic_FromRadio from = meshtastic_FromRadio_init_zero;
	int ret;

	while (true) {
		switch (api->config_state) {
		case MESHTASTIC_PHONEAPI_CONFIG_IDLE:
			return -ENOENT;
		case MESHTASTIC_PHONEAPI_CONFIG_MY_INFO:
			fill_my_info(&from);
			return emit_frame(api, &from, MESHTASTIC_PHONEAPI_CONFIG_DEVICE_UI, 0U,
					  frame);
		case MESHTASTIC_PHONEAPI_CONFIG_DEVICE_UI:
			from.id = meshtastic_next_fromradio_id();
			from.which_payload_variant = meshtastic_FromRadio_deviceuiConfig_tag;
			(void)meshtastic_config_store_get_device_ui(&from.deviceuiConfig);
			return emit_frame(api, &from, MESHTASTIC_PHONEAPI_CONFIG_NODE_INFO, 0U,
					  frame);
		case MESHTASTIC_PHONEAPI_CONFIG_NODE_INFO:
			fill_node_info(&from);
			return emit_frame(api, &from, MESHTASTIC_PHONEAPI_CONFIG_METADATA, 0U,
					  frame);
		case MESHTASTIC_PHONEAPI_CONFIG_METADATA:
			fill_metadata_frame(&from);
			return emit_frame(api, &from, MESHTASTIC_PHONEAPI_CONFIG_CHANNELS, 0U,
					  frame);
		case MESHTASTIC_PHONEAPI_CONFIG_CHANNELS:
			fill_channel(&from, api->config_index);
			return emit_frame(api, &from,
					  (api->config_index + 1U >= MESHTASTIC_MAX_CHANNELS)
						  ? MESHTASTIC_PHONEAPI_CONFIG_CONFIGS
						  : MESHTASTIC_PHONEAPI_CONFIG_CHANNELS,
					  (api->config_index + 1U >= MESHTASTIC_MAX_CHANNELS)
						  ? 0U
						  : (uint8_t)(api->config_index + 1U),
					  frame);
		case MESHTASTIC_PHONEAPI_CONFIG_CONFIGS:
			while (api->config_index < ARRAY_SIZE(config_tags)) {
				from = (meshtastic_FromRadio)meshtastic_FromRadio_init_zero;
				ret = fill_config_variant(&from, config_tags[api->config_index]);
				if (ret == 0) {
					return emit_frame(
						api, &from,
						(api->config_index + 1U >= ARRAY_SIZE(config_tags))
							? MESHTASTIC_PHONEAPI_CONFIG_MODULES
							: MESHTASTIC_PHONEAPI_CONFIG_CONFIGS,
						(api->config_index + 1U >= ARRAY_SIZE(config_tags))
							? 0U
							: (uint8_t)(api->config_index + 1U),
						frame);
				}
				api->config_index++;
			}
			api->config_state = MESHTASTIC_PHONEAPI_CONFIG_MODULES;
			api->config_index = 0U;
			break;
		case MESHTASTIC_PHONEAPI_CONFIG_MODULES:
			while (api->config_index < ARRAY_SIZE(module_tags)) {
				from = (meshtastic_FromRadio)meshtastic_FromRadio_init_zero;
				ret = fill_module_variant(&from, module_tags[api->config_index]);
				if (ret == 0) {
					return emit_frame(
						api, &from,
						(api->config_index + 1U >= ARRAY_SIZE(module_tags))
							? MESHTASTIC_PHONEAPI_CONFIG_QUEUE_STATUS
							: MESHTASTIC_PHONEAPI_CONFIG_MODULES,
						(api->config_index + 1U >= ARRAY_SIZE(module_tags))
							? 0U
							: (uint8_t)(api->config_index + 1U),
						frame);
				}
				api->config_index++;
			}
			api->config_state = MESHTASTIC_PHONEAPI_CONFIG_QUEUE_STATUS;
			api->config_index = 0U;
			break;
		case MESHTASTIC_PHONEAPI_CONFIG_QUEUE_STATUS:
			from.id = meshtastic_next_fromradio_id();
			from.which_payload_variant = meshtastic_FromRadio_queueStatus_tag;
			from.queueStatus.res = 0;
			from.queueStatus.free = (api->count >= api->queue_size)
							? 0U
							: (api->queue_size - api->count);
			from.queueStatus.maxlen = api->queue_size;
			from.queueStatus.mesh_packet_id = 0U;
			return emit_frame(api, &from, MESHTASTIC_PHONEAPI_CONFIG_COMPLETE, 0U,
					  frame);
		case MESHTASTIC_PHONEAPI_CONFIG_COMPLETE:
			from.id = meshtastic_next_fromradio_id();
			from.which_payload_variant = meshtastic_FromRadio_config_complete_id_tag;
			from.config_complete_id = api->config_request_id;
			return emit_frame(api, &from, MESHTASTIC_PHONEAPI_CONFIG_IDLE, 0U, frame);
		default:
			api->config_state = MESHTASTIC_PHONEAPI_CONFIG_IDLE;
			api->config_index = 0U;
			return -EINVAL;
		}
	}
}

void meshtastic_phoneapi_enqueue_phone_config(struct meshtastic_phoneapi *api, uint32_t request_id)
{
	LOG_INF("%s want_config nonce=%u", api->name, request_id);

	k_mutex_lock(&api->lock, K_FOREVER);
	api->head = 0U;
	api->tail = 0U;
	api->count = 0U;
	api->current_valid = false;
	api->config_state = MESHTASTIC_PHONEAPI_CONFIG_MY_INFO;
	api->config_index = 0U;
	api->config_request_id = request_id;
	k_mutex_unlock(&api->lock);

	if (api->invalidate_delivery != NULL) {
		api->invalidate_delivery(api);
	}

	meshtastic_phoneapi_notify_data_ready(api);
}

int meshtastic_phoneapi_set_channel(uint8_t index, const meshtastic_Channel *channel)
{
	return meshtastic_config_store_set_channel(index, channel);
}
