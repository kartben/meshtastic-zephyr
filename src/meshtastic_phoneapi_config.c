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

#include "meshtastic_channels.h"
#include "meshtastic_config_store.h"
#include "meshtastic_phoneapi.h"

#include "meshtastic/device_ui.pb.h"
#include "meshtastic/module_config.pb.h"

LOG_MODULE_DECLARE(meshtastic, CONFIG_MESHTASTIC_LOG_LEVEL);

static int enqueue_id(struct meshtastic_phoneapi *api, uint32_t id, meshtastic_FromRadio *from)
{
	from->id = id;
	return meshtastic_phoneapi_enqueue_fromradio(api, from);
}

static void enqueue_node_info(struct meshtastic_phoneapi *api, uint32_t id)
{
	meshtastic_FromRadio from = meshtastic_FromRadio_init_zero;

	from.which_payload_variant = meshtastic_FromRadio_node_info_tag;
	from.node_info.num = meshtastic_get_node_id();
	from.node_info.has_user = true;
	meshtastic_fill_user(&from.node_info.user);

	(void)enqueue_id(api, id, &from);
}

static void enqueue_metadata_frame(struct meshtastic_phoneapi *api, uint32_t id)
{
	meshtastic_FromRadio from = meshtastic_FromRadio_init_zero;

	from.which_payload_variant = meshtastic_FromRadio_metadata_tag;
	strncpy(from.metadata.firmware_version, "zephyr.123",
		sizeof(from.metadata.firmware_version) - 1U);
	from.metadata.hasBluetooth = IS_ENABLED(CONFIG_MESHTASTIC_BLE);
	from.metadata.role = meshtastic_device_role();
	from.metadata.hw_model = meshtastic_hw_model();

	(void)enqueue_id(api, id, &from);
}

static void enqueue_channel(struct meshtastic_phoneapi *api, uint32_t id, int index)
{
	meshtastic_FromRadio from = meshtastic_FromRadio_init_zero;
	meshtastic_Channel slot = meshtastic_Channel_init_zero;

	from.which_payload_variant = meshtastic_FromRadio_channel_tag;
	from.channel.index = index;

	if (meshtastic_config_store_get_channel((uint8_t)index, &slot) == 0 &&
	    slot.role != meshtastic_Channel_Role_DISABLED) {
		from.channel = slot;
		from.channel.index = index;
	} else {
		from.channel.role = meshtastic_Channel_Role_DISABLED;
		from.channel.has_settings = true;
	}

	(void)enqueue_id(api, id, &from);
}

static void enqueue_config_variant(struct meshtastic_phoneapi *api, uint32_t id,
				   pb_size_t which_tag)
{
	meshtastic_FromRadio from = meshtastic_FromRadio_init_zero;
	int ret;

	from.which_payload_variant = meshtastic_FromRadio_config_tag;

	ret = meshtastic_config_store_get_config(which_tag, &from.config);
	if (ret < 0) {
		return;
	}

	(void)enqueue_id(api, id, &from);
}

static void enqueue_module_variant(struct meshtastic_phoneapi *api, uint32_t id,
				   pb_size_t which_tag)
{
	meshtastic_FromRadio from = meshtastic_FromRadio_init_zero;
	int ret;

	from.which_payload_variant = meshtastic_FromRadio_moduleConfig_tag;

	ret = meshtastic_config_store_get_module(which_tag, &from.moduleConfig);
	if (ret < 0) {
		return;
	}

	(void)enqueue_id(api, id, &from);
}

void meshtastic_phoneapi_enqueue_phone_config(struct meshtastic_phoneapi *api, uint32_t request_id)
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
	uint32_t id;

	LOG_INF("%s want_config nonce=%u", api->name, request_id);

	meshtastic_phoneapi_enqueue_my_info(api, request_id);

	id = meshtastic_next_fromradio_id();
	from = (meshtastic_FromRadio)meshtastic_FromRadio_init_zero;
	from.which_payload_variant = meshtastic_FromRadio_deviceuiConfig_tag;
	(void)meshtastic_config_store_get_device_ui(&from.deviceuiConfig);
	(void)enqueue_id(api, id, &from);

	enqueue_node_info(api, meshtastic_next_fromradio_id());
	enqueue_metadata_frame(api, meshtastic_next_fromradio_id());

	for (uint8_t i = 0; i < MESHTASTIC_MAX_CHANNELS; i++) {
		enqueue_channel(api, meshtastic_next_fromradio_id(), i);
	}

	for (size_t i = 0; i < ARRAY_SIZE(config_tags); i++) {
		enqueue_config_variant(api, meshtastic_next_fromradio_id(), config_tags[i]);
	}

	for (size_t i = 0; i < ARRAY_SIZE(module_tags); i++) {
		enqueue_module_variant(api, meshtastic_next_fromradio_id(), module_tags[i]);
	}

	meshtastic_phoneapi_enqueue_queue_status(api, 0, 0U);

	id = meshtastic_next_fromradio_id();
	from = (meshtastic_FromRadio)meshtastic_FromRadio_init_zero;
	from.which_payload_variant = meshtastic_FromRadio_config_complete_id_tag;
	from.config_complete_id = request_id;
	(void)enqueue_id(api, id, &from);

	LOG_INF("%s config response enqueued for nonce=%u", api->name, request_id);
}

int meshtastic_phoneapi_set_channel(uint8_t index, const meshtastic_Channel *channel)
{
	return meshtastic_config_store_set_channel(index, channel);
}
