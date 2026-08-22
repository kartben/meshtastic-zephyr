/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

/*
 * Build-only coverage for feature combinations the running suites do not
 * compile: the settings backend and the MQTT gateway, and the Kconfig switches
 * that turn optional behaviour off. Nothing here runs; the value is that every
 * combination keeps compiling.
 */

#include <zephyr/kernel.h>

#include <zephyr/meshtastic/meshtastic.h>

int main(void)
{
	static const struct meshtastic_config cfg = {
		.psk = meshtastic_default_psk,
		.psk_len = sizeof(meshtastic_default_psk),
		.channel_name = MESHTASTIC_CHANNEL_LONGFAST,
		.frequency = MESHTASTIC_FREQ_EU,
	};

	(void)meshtastic_init(&cfg);

	return 0;
}
