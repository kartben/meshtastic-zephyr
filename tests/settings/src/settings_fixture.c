/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

#include <zephyr/settings/settings.h>
#include <zephyr/ztest.h>

#include "meshtastic_channels.h"
#include "meshtastic_config_store.h"

#include "mock_lora.h"
#include "settings_fixture.h"

struct meshtastic_config settings_test_cfg = {
	.node_id = SETTINGS_TEST_NODE_ID,
	.psk = meshtastic_default_psk,
	.psk_len = sizeof(meshtastic_default_psk),
	.channel_name = MESHTASTIC_CHANNEL_LONGFAST,
	.frequency = MESHTASTIC_FREQ_EU,
};

void *settings_test_setup(void)
{
	static bool started;

	if (!started) {
		settings_test_cfg.lora_dev = mock_lora_device();
		zassert_ok(meshtastic_init(&settings_test_cfg), "meshtastic_init failed");
		started = true;
	}

	return NULL;
}

void settings_test_forget_ram(void)
{
	/* Seeding snapshots live state rather than inventing it, so reset that first. */
	meshtastic_set_device_role(meshtastic_Config_DeviceConfig_Role_CLIENT);
	zassert_ok(meshtastic_channels_init_from_config(&settings_test_cfg));

	zassert_ok(meshtastic_config_store_seed(&settings_test_cfg));
}

void settings_test_reload(void)
{
	zassert_ok(settings_load_subtree("meshtastic"), "settings load failed");
}
