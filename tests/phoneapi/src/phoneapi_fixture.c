/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

#include <zephyr/ztest.h>

#include <zephyr/meshtastic/meshtastic.h>

#include "phoneapi_fixture.h"

void *phoneapi_suite_setup(void)
{
	static struct meshtastic_config cfg = {
		.node_id = TEST_NODE_ID,
		.psk = meshtastic_default_psk,
		.psk_len = sizeof(meshtastic_default_psk),
		.channel_name = MESHTASTIC_CHANNEL_LONGFAST,
		.frequency = MESHTASTIC_FREQ_EU,
		.long_name = "Zephyr Test Node",
		.short_name = "ZTN",
	};
	static bool initialized;

	if (!initialized) {
		cfg.lora_dev = mock_lora_device();
		zassert_ok(meshtastic_init(&cfg), "meshtastic_init failed");
		initialized = true;
	}

	return NULL;
}
