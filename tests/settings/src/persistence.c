/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

/**
 * @file
 * @brief What survives a reboot, and what the store refuses to load.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/ztest.h>

#include "meshtastic_channels.h"
#include "meshtastic_config_store.h"
#include "meshtastic_settings.h"

#include "settings_fixture.h"

#define SAVE_DELAY K_MSEC(CONFIG_MESHTASTIC_SETTINGS_SAVE_DELAY_MS)

/* Keys the exporter is expected to produce, all under the meshtastic subtree. */
struct exported_keys {
	bool owner;
	bool primary_channel;
	bool lora_config;
};

static void set_channel(uint8_t index, meshtastic_Channel_Role role, const char *name)
{
	meshtastic_Channel channel = *meshtastic_channels_get(index);

	channel.role = role;
	channel.has_settings = true;
	strncpy(channel.settings.name, name, sizeof(channel.settings.name) - 1U);

	zassert_ok(meshtastic_config_store_set_channel(index, &channel));
}

static const char *stored_channel_name(uint8_t index)
{
	static meshtastic_Channel channel;

	zassert_ok(meshtastic_config_store_get_channel(index, &channel));

	return channel.settings.name;
}

static meshtastic_Config_DeviceConfig_Role stored_device_role(void)
{
	meshtastic_Config config;

	zassert_ok(meshtastic_config_store_get_config(meshtastic_Config_device_tag, &config));

	return config.payload_variant.device.role;
}

static int note_exported_key(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg,
			     void *param)
{
	struct exported_keys *seen = param;

	ARG_UNUSED(len);
	ARG_UNUSED(read_cb);
	ARG_UNUSED(cb_arg);

	seen->owner |= strcmp(key, "owner") == 0;
	seen->primary_channel |= strcmp(key, "channel/0") == 0;
	seen->lora_config |= strcmp(key, "config/lora") == 0;

	return 0;
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);

	set_channel(0U, meshtastic_Channel_Role_PRIMARY, MESHTASTIC_CHANNEL_LONGFAST);
	zassert_ok(meshtastic_settings_flush());
}

ZTEST_SUITE(settings_persistence, NULL, settings_test_setup, before, NULL, NULL);

ZTEST(settings_persistence, test_a_saved_channel_comes_back_after_a_reload)
{
	set_channel(0U, meshtastic_Channel_Role_PRIMARY, "Persisted");
	zassert_ok(meshtastic_settings_flush());

	settings_test_forget_ram();
	zassert_str_equal(stored_channel_name(0U), MESHTASTIC_CHANNEL_LONGFAST,
			  "the store should be back to its defaults before the reload");

	settings_test_reload();

	zassert_str_equal(stored_channel_name(0U), "Persisted");
}

ZTEST(settings_persistence, test_a_saved_config_comes_back_after_a_reload)
{
	zassert_ok(meshtastic_config_store_set_device_role(
		meshtastic_Config_DeviceConfig_Role_ROUTER));
	zassert_ok(meshtastic_settings_flush());

	settings_test_forget_ram();
	zassert_equal(stored_device_role(), meshtastic_Config_DeviceConfig_Role_CLIENT);

	settings_test_reload();

	zassert_equal(stored_device_role(), meshtastic_Config_DeviceConfig_Role_ROUTER);
}

ZTEST(settings_persistence, test_a_change_is_saved_once_the_delay_has_passed)
{
	/* Changing configuration schedules the save; nothing else asks for it. */
	set_channel(0U, meshtastic_Channel_Role_PRIMARY, "Coalesced");

	k_sleep(SAVE_DELAY);
	k_sleep(K_MSEC(50));

	settings_test_forget_ram();
	settings_test_reload();

	zassert_str_equal(stored_channel_name(0U), "Coalesced");
}

ZTEST(settings_persistence, test_settings_are_stored_under_the_meshtastic_subtree)
{
	struct exported_keys seen = {0};

	zassert_ok(settings_load_subtree_direct("meshtastic", note_exported_key, &seen));

	zassert_true(seen.owner, "owner was not exported");
	zassert_true(seen.primary_channel, "channel/0 was not exported");
	zassert_true(seen.lora_config, "config/lora was not exported");
}

ZTEST(settings_persistence, test_an_oversized_record_is_ignored)
{
	uint8_t oversized[MESHTASTIC_STORE_VALUE_MAX + 1] = {0};

	zassert_ok(settings_save_one("meshtastic/channel/0", oversized, sizeof(oversized)));

	settings_test_reload();

	zassert_str_equal(stored_channel_name(0U), MESHTASTIC_CHANNEL_LONGFAST,
			  "an unreadable record must not overwrite what is in memory");

	zassert_ok(settings_delete("meshtastic/channel/0"));
}

ZTEST(settings_persistence, test_a_record_the_store_cannot_parse_is_ignored)
{
	const uint8_t nonsense[] = {0xFFU, 0xFFU, 0xFFU, 0xFFU};

	set_channel(1U, meshtastic_Channel_Role_SECONDARY, "Backup");
	zassert_ok(settings_save_one("meshtastic/channel/1", nonsense, sizeof(nonsense)));

	settings_test_reload();

	zassert_str_equal(stored_channel_name(1U), "Backup",
			  "an unparseable record must not overwrite what is in memory");

	zassert_ok(settings_delete("meshtastic/channel/1"));
}

ZTEST(settings_persistence, test_an_unknown_key_is_ignored)
{
	const uint8_t value[] = {0x01U};

	zassert_ok(settings_save_one("meshtastic/not/a/setting", value, sizeof(value)));

	settings_test_reload();

	zassert_ok(settings_delete("meshtastic/not/a/setting"));
}

ZTEST(settings_persistence, test_a_stored_value_can_be_read_back_at_runtime)
{
	uint8_t expected[MESHTASTIC_STORE_VALUE_MAX];
	uint8_t actual[MESHTASTIC_STORE_VALUE_MAX];
	int len;

	len = meshtastic_config_store_setting_get("owner", expected, sizeof(expected));
	zassert_true(len > 0, "the store has no owner record");

	zassert_equal(settings_runtime_get("meshtastic/owner", actual, sizeof(actual)), len);
	zassert_mem_equal(actual, expected, (size_t)len);
}
