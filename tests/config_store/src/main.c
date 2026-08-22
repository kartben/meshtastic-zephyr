/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#include <zephyr/meshtastic/meshtastic.h>

#include "meshtastic_channels.h"
#include "meshtastic_config_store.h"
#include "meshtastic_core.h"

#include "mock_lora.h"

#define TEST_NODE_ID    0x12345678U
#define TEST_LONG_NAME  "Zephyr Test Node"
#define TEST_SHORT_NAME "ZTN"

/* meshtastic_config_store_setting_get() prefixes records with version + length. */
#define RECORD_HEADER_LEN 4U

static struct meshtastic_config cfg = {
	.node_id = TEST_NODE_ID,
	.psk = meshtastic_default_psk,
	.psk_len = sizeof(meshtastic_default_psk),
	.channel_name = MESHTASTIC_CHANNEL_LONGFAST,
	.frequency = MESHTASTIC_FREQ_EU,
	.long_name = TEST_LONG_NAME,
	.short_name = TEST_SHORT_NAME,
};

static meshtastic_Config load_config(pb_size_t tag)
{
	meshtastic_Config config;

	zassert_ok(meshtastic_config_store_get_config(tag, &config), "get_config(%u) failed", tag);

	return config;
}

static meshtastic_ModuleConfig load_module(pb_size_t tag)
{
	meshtastic_ModuleConfig module;

	zassert_ok(meshtastic_config_store_get_module(tag, &module), "get_module(%u) failed", tag);

	return module;
}

static void *setup(void)
{
	cfg.lora_dev = mock_lora_device();
	zassert_ok(meshtastic_init(&cfg), "meshtastic_init failed");

	return NULL;
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);

	/* Seeding snapshots the live transmit context, so restore that first. */
	mt.hop_limit = CONFIG_MESHTASTIC_DEFAULT_HOP_LIMIT;
	mt.tx_power = CONFIG_MESHTASTIC_TX_POWER;
	mt.frequency = cfg.frequency;

	zassert_ok(meshtastic_channels_init_from_config(&cfg));
	zassert_ok(meshtastic_config_store_seed(&cfg));
	zassert_ok(meshtastic_config_store_apply_core());
}

ZTEST_SUITE(config_store, NULL, setup, before, NULL, NULL);

ZTEST(config_store, test_seed_records_the_owner_names)
{
	zassert_str_equal(meshtastic_config_store_long_name(), TEST_LONG_NAME);
	zassert_str_equal(meshtastic_config_store_short_name(), TEST_SHORT_NAME);
	zassert_str_equal(mt.long_name, TEST_LONG_NAME);
	zassert_str_equal(mt.short_name, TEST_SHORT_NAME);
}

ZTEST(config_store, test_seed_derives_the_lora_region_from_the_frequency)
{
	meshtastic_Config_LoRaConfig lora;

	lora = load_config(meshtastic_Config_lora_tag).payload_variant.lora;

	zassert_equal(lora.region, meshtastic_Config_LoRaConfig_RegionCode_EU_868);
	zassert_true(lora.use_preset);
	zassert_equal(lora.modem_preset, meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST);
	zassert_true(lora.tx_enabled);
	zassert_equal(lora.hop_limit, CONFIG_MESHTASTIC_DEFAULT_HOP_LIMIT);
	zassert_equal(lora.tx_power, CONFIG_MESHTASTIC_TX_POWER);
}

ZTEST(config_store, test_seed_mirrors_the_channel_table)
{
	meshtastic_Channel channel;

	zassert_ok(meshtastic_config_store_get_channel(0U, &channel));
	zassert_equal(channel.role, meshtastic_Channel_Role_PRIMARY);
	zassert_equal(channel.index, 0);
	zassert_str_equal(channel.settings.name, MESHTASTIC_CHANNEL_LONGFAST);

	zassert_ok(meshtastic_config_store_get_channel(7U, &channel));
	zassert_equal(channel.role, meshtastic_Channel_Role_DISABLED);
	zassert_equal(channel.index, 7);
}

ZTEST(config_store, test_unknown_variants_are_rejected)
{
	meshtastic_Config config;
	meshtastic_ModuleConfig module;
	meshtastic_Channel channel;

	zassert_equal(meshtastic_config_store_get_config(0U, &config), -EINVAL);
	zassert_equal(meshtastic_config_store_get_config(meshtastic_Config_lora_tag, NULL),
		      -EINVAL);
	zassert_equal(meshtastic_config_store_get_module(0U, &module), -EINVAL);
	zassert_equal(meshtastic_config_store_get_module(meshtastic_ModuleConfig_mqtt_tag, NULL),
		      -EINVAL);
	zassert_equal(meshtastic_config_store_get_channel(MESHTASTIC_MAX_CHANNELS, &channel),
		      -EINVAL);
	zassert_equal(meshtastic_config_store_get_channel(0U, NULL), -EINVAL);
	zassert_equal(meshtastic_config_store_get_device_ui(NULL), -EINVAL);
	zassert_equal(meshtastic_config_store_seed(NULL), -EINVAL);
}

ZTEST(config_store, test_lora_config_updates_the_transmit_context)
{
	meshtastic_Config config = load_config(meshtastic_Config_lora_tag);

	config.payload_variant.lora.hop_limit = 5U;
	config.payload_variant.lora.tx_power = 11;
	config.payload_variant.lora.region = meshtastic_Config_LoRaConfig_RegionCode_US;

	zassert_ok(meshtastic_config_store_set_config(&config));
	zassert_equal(mt.hop_limit, 5U);
	zassert_equal(mt.tx_power, 11);
	zassert_equal(mt.frequency, MESHTASTIC_FREQ_US);
}

ZTEST(config_store, test_lora_override_frequency_wins_over_the_region)
{
	meshtastic_Config config = load_config(meshtastic_Config_lora_tag);

	config.payload_variant.lora.region = meshtastic_Config_LoRaConfig_RegionCode_US;
	config.payload_variant.lora.override_frequency = 433.5f;

	zassert_ok(meshtastic_config_store_set_config(&config));
	zassert_equal(mt.frequency, 433500000U);
}

ZTEST(config_store, test_device_config_updates_role_and_rebroadcast_mode)
{
	meshtastic_Config config = load_config(meshtastic_Config_device_tag);

	config.payload_variant.device.role = meshtastic_Config_DeviceConfig_Role_ROUTER;
	config.payload_variant.device.rebroadcast_mode =
		meshtastic_Config_DeviceConfig_RebroadcastMode_LOCAL_ONLY;

	zassert_ok(meshtastic_config_store_set_config(&config));
	zassert_equal(meshtastic_device_role(), meshtastic_Config_DeviceConfig_Role_ROUTER);
	zassert_equal(meshtastic_rebroadcast_mode(),
		      meshtastic_Config_DeviceConfig_RebroadcastMode_LOCAL_ONLY);
}

ZTEST(config_store, test_invalid_config_values_are_rejected)
{
	meshtastic_Config device = load_config(meshtastic_Config_device_tag);
	meshtastic_Config lora = load_config(meshtastic_Config_lora_tag);
	meshtastic_Config bluetooth = load_config(meshtastic_Config_bluetooth_tag);
	meshtastic_Config unknown = meshtastic_Config_init_zero;

	device.payload_variant.device.role =
		(meshtastic_Config_DeviceConfig_Role)(meshtastic_Config_DeviceConfig_Role_CLIENT_BASE +
						      1);
	zassert_equal(meshtastic_config_store_set_config(&device), -EINVAL);

	lora.payload_variant.lora.hop_limit = 8U;
	zassert_equal(meshtastic_config_store_set_config(&lora), -EINVAL);

	bluetooth.payload_variant.bluetooth.mode =
		(meshtastic_Config_BluetoothConfig_PairingMode)(meshtastic_Config_BluetoothConfig_PairingMode_NO_PIN +
								1);
	zassert_equal(meshtastic_config_store_set_config(&bluetooth), -EINVAL);

	zassert_equal(meshtastic_config_store_set_config(&unknown), -EINVAL);
	zassert_equal(meshtastic_config_store_set_config(NULL), -EINVAL);
}

ZTEST(config_store, test_module_config_round_trips)
{
	meshtastic_ModuleConfig module = load_module(meshtastic_ModuleConfig_telemetry_tag);
	meshtastic_ModuleConfig unknown = meshtastic_ModuleConfig_init_zero;

	module.payload_variant.telemetry.device_update_interval = 900U;
	zassert_ok(meshtastic_config_store_set_module(&module));

	module = load_module(meshtastic_ModuleConfig_telemetry_tag);
	zassert_equal(module.payload_variant.telemetry.device_update_interval, 900U);

	zassert_equal(meshtastic_config_store_set_module(&unknown), -EINVAL);
	zassert_equal(meshtastic_config_store_set_module(NULL), -EINVAL);
}

ZTEST(config_store, test_device_ui_config_is_served_from_the_config_slot)
{
	meshtastic_Config config = load_config(meshtastic_Config_device_ui_tag);
	meshtastic_DeviceUIConfig device_ui;

	config.payload_variant.device_ui.screen_brightness = 42U;
	zassert_ok(meshtastic_config_store_set_config(&config));

	zassert_ok(meshtastic_config_store_get_device_ui(&device_ui));
	zassert_equal(device_ui.screen_brightness, 42U);
}

ZTEST(config_store, test_setting_a_channel_updates_the_live_table)
{
	meshtastic_Channel channel = meshtastic_Channel_init_zero;

	channel.role = meshtastic_Channel_Role_SECONDARY;
	channel.has_settings = true;
	strcpy(channel.settings.name, "Secondary");
	memcpy(channel.settings.psk.bytes, meshtastic_default_psk, sizeof(meshtastic_default_psk));
	channel.settings.psk.size = sizeof(meshtastic_default_psk);

	zassert_ok(meshtastic_config_store_set_channel(2U, &channel));
	zassert_str_equal(meshtastic_channels_get_name(2U), "Secondary");

	zassert_ok(meshtastic_config_store_get_channel(2U, &channel));
	zassert_equal(channel.index, 2);
	zassert_equal(channel.role, meshtastic_Channel_Role_SECONDARY);
}

ZTEST(config_store, test_invalid_channels_are_rejected)
{
	meshtastic_Channel channel = meshtastic_Channel_init_zero;

	channel.role = meshtastic_Channel_Role_SECONDARY;
	channel.has_settings = true;

	zassert_equal(meshtastic_config_store_set_channel(MESHTASTIC_MAX_CHANNELS, &channel),
		      -EINVAL);
	zassert_equal(meshtastic_config_store_set_channel(0U, NULL), -EINVAL);

	channel.settings.psk.size = 8U;
	zassert_equal(meshtastic_config_store_set_channel(1U, &channel), -EINVAL);

	channel.settings.psk.size = 0U;
	channel.role = (meshtastic_Channel_Role)(meshtastic_Channel_Role_SECONDARY + 1);
	zassert_equal(meshtastic_config_store_set_channel(1U, &channel), -EINVAL);
}

ZTEST(config_store, test_role_and_rebroadcast_setters_validate_their_input)
{
	zassert_ok(meshtastic_config_store_set_device_role(
		meshtastic_Config_DeviceConfig_Role_ROUTER_LATE));
	zassert_equal(meshtastic_device_role(), meshtastic_Config_DeviceConfig_Role_ROUTER_LATE);
	zassert_equal(load_config(meshtastic_Config_device_tag).payload_variant.device.role,
		      meshtastic_Config_DeviceConfig_Role_ROUTER_LATE);

	zassert_ok(meshtastic_config_store_set_rebroadcast_mode(
		meshtastic_Config_DeviceConfig_RebroadcastMode_KNOWN_ONLY));
	zassert_equal(meshtastic_rebroadcast_mode(),
		      meshtastic_Config_DeviceConfig_RebroadcastMode_KNOWN_ONLY);

	zassert_equal(
		meshtastic_config_store_set_device_role((
			meshtastic_Config_DeviceConfig_Role)(meshtastic_Config_DeviceConfig_Role_CLIENT_BASE +
							     1)),
		-EINVAL);
	zassert_equal(
		meshtastic_config_store_set_rebroadcast_mode((
			meshtastic_Config_DeviceConfig_RebroadcastMode)(meshtastic_Config_DeviceConfig_RebroadcastMode_CORE_PORTNUMS_ONLY +
									1)),
		-EINVAL);
}

ZTEST(config_store, test_owner_setting_round_trips)
{
	uint8_t record[MESHTASTIC_STORE_VALUE_MAX];
	int len;

	len = meshtastic_config_store_setting_get("owner", record, sizeof(record));
	zassert_true(len > 0, "owner record encode failed: %d", len);

	zassert_ok(meshtastic_config_store_seed(&cfg));
	zassert_ok(meshtastic_config_store_setting_set("owner", record, (size_t)len));
	zassert_str_equal(meshtastic_config_store_long_name(), TEST_LONG_NAME);
	zassert_str_equal(meshtastic_config_store_short_name(), TEST_SHORT_NAME);

	zassert_equal(meshtastic_config_store_setting_get("owner", record, RECORD_HEADER_LEN),
		      -ENOMEM);
	zassert_equal(meshtastic_config_store_setting_set("owner", record, (size_t)len - 1U),
		      -EINVAL);
}

ZTEST(config_store, test_channel_setting_round_trips)
{
	uint8_t record[MESHTASTIC_STORE_VALUE_MAX];
	meshtastic_Channel channel;
	int len;

	zassert_ok(meshtastic_config_store_get_channel(0U, &channel));
	strcpy(channel.settings.name, "Persisted");
	zassert_ok(meshtastic_config_store_set_channel(0U, &channel));

	len = meshtastic_config_store_setting_get("channel/0", record, sizeof(record));
	zassert_true(len > (int)RECORD_HEADER_LEN, "channel record encode failed: %d", len);

	zassert_ok(meshtastic_config_store_seed(&cfg));
	zassert_ok(meshtastic_config_store_setting_set("channel/3", record, (size_t)len));

	zassert_ok(meshtastic_config_store_get_channel(3U, &channel));
	zassert_str_equal(channel.settings.name, "Persisted");
	zassert_equal(channel.index, 3, "the key index overrides the stored one");
}

ZTEST(config_store, test_config_and_module_settings_round_trip)
{
	uint8_t record[MESHTASTIC_STORE_VALUE_MAX];
	int len;

	len = meshtastic_config_store_setting_get("config/lora", record, sizeof(record));
	zassert_true(len > (int)RECORD_HEADER_LEN, "lora record encode failed: %d", len);
	zassert_ok(meshtastic_config_store_setting_set("config/lora", record, (size_t)len));
	zassert_equal(load_config(meshtastic_Config_lora_tag).payload_variant.lora.hop_limit,
		      CONFIG_MESHTASTIC_DEFAULT_HOP_LIMIT);

	/* A record only loads under the key that names its own variant. */
	zassert_equal(meshtastic_config_store_setting_set("config/device", record, (size_t)len),
		      -EINVAL);

	len = meshtastic_config_store_setting_get("module/mqtt", record, sizeof(record));
	zassert_true(len >= (int)RECORD_HEADER_LEN, "mqtt record encode failed: %d", len);
	zassert_ok(meshtastic_config_store_setting_set("module/mqtt", record, (size_t)len));
	zassert_equal(meshtastic_config_store_setting_set("module/serial", record, (size_t)len),
		      -EINVAL);
}

ZTEST(config_store, test_corrupt_records_are_rejected)
{
	uint8_t record[MESHTASTIC_STORE_VALUE_MAX];
	int len;

	len = meshtastic_config_store_setting_get("channel/0", record, sizeof(record));
	zassert_true(len > (int)RECORD_HEADER_LEN);

	record[0] = 0xFFU;
	zassert_equal(meshtastic_config_store_setting_set("channel/0", record, (size_t)len),
		      -EINVAL);

	record[0] = 1U;
	sys_put_le16((uint16_t)(len - RECORD_HEADER_LEN + 1U), &record[2]);
	zassert_equal(meshtastic_config_store_setting_set("channel/0", record, (size_t)len),
		      -EINVAL);

	zassert_equal(
		meshtastic_config_store_setting_set("channel/0", record, RECORD_HEADER_LEN - 1U),
		-EINVAL);
}

ZTEST(config_store, test_unknown_setting_keys_are_rejected)
{
	uint8_t record[MESHTASTIC_STORE_VALUE_MAX];

	zassert_equal(meshtastic_config_store_setting_get("nope", record, sizeof(record)), -ENOENT);
	zassert_equal(meshtastic_config_store_setting_get(NULL, record, sizeof(record)), -EINVAL);
	zassert_equal(meshtastic_config_store_setting_get("owner", NULL, sizeof(record)), -EINVAL);
	zassert_equal(meshtastic_config_store_setting_get("channel/9", record, sizeof(record)),
		      -EINVAL);
	zassert_equal(meshtastic_config_store_setting_get("channel/x", record, sizeof(record)),
		      -EINVAL);
	zassert_equal(meshtastic_config_store_setting_get("channel/", record, sizeof(record)),
		      -EINVAL);
	zassert_equal(meshtastic_config_store_setting_get("config/nope", record, sizeof(record)),
		      -ENOENT);
	zassert_equal(meshtastic_config_store_setting_get("module/nope", record, sizeof(record)),
		      -ENOENT);

	zassert_equal(meshtastic_config_store_setting_set("nope", record, 1U), -ENOENT);
	zassert_equal(meshtastic_config_store_setting_set(NULL, record, 1U), -EINVAL);
	zassert_equal(meshtastic_config_store_setting_set("owner", NULL, 1U), -EINVAL);
	zassert_equal(meshtastic_config_store_setting_set("channel/9", record, 1U), -EINVAL);
	zassert_equal(meshtastic_config_store_setting_set("config/nope", record, 1U), -ENOENT);
	zassert_equal(meshtastic_config_store_setting_set("module/nope", record, 1U), -ENOENT);
}

static size_t exported_count;
static int export_stop_after;

static int count_export(const char *name, const void *val, size_t val_len)
{
	zassert_not_null(name);
	zassert_not_null(val);
	zassert_true(val_len > 0U, "%s exported an empty record", name);

	exported_count++;
	if (export_stop_after > 0 && exported_count == (size_t)export_stop_after) {
		return -EIO;
	}

	return 0;
}

ZTEST(config_store, test_export_walks_every_stored_key)
{
	/* owner + 8 channels + 10 config variants + 16 module variants. */
	const size_t expected = 1U + MESHTASTIC_MAX_CHANNELS + 10U + 16U;

	exported_count = 0U;
	export_stop_after = 0;
	zassert_ok(meshtastic_config_store_export(count_export));
	zassert_equal(exported_count, expected, "exported %zu keys", exported_count);

	zassert_equal(meshtastic_config_store_export(NULL), -EINVAL);
}

ZTEST(config_store, test_export_stops_at_the_first_error)
{
	exported_count = 0U;
	export_stop_after = 3;
	zassert_equal(meshtastic_config_store_export(count_export), -EIO);
	zassert_equal(exported_count, 3U);
	export_stop_after = 0;
}
