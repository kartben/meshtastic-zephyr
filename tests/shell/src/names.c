/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

/**
 * @file
 * @brief The names the shell accepts and prints for roles and modes.
 *
 * The lists below are the Meshtastic enums lowercased, and every one of them
 * has to survive a trip out to the shell and back.
 */

#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include "meshtastic_channels.h"
#include "meshtastic_core.h"

#include "shell_fixture.h"

static const struct {
	const char *name;
	meshtastic_Config_DeviceConfig_Role role;
} device_roles[] = {
	{"client", meshtastic_Config_DeviceConfig_Role_CLIENT},
	{"client_mute", meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE},
	{"router", meshtastic_Config_DeviceConfig_Role_ROUTER},
	{"router_late", meshtastic_Config_DeviceConfig_Role_ROUTER_LATE},
	{"client_base", meshtastic_Config_DeviceConfig_Role_CLIENT_BASE},
	{"sensor", meshtastic_Config_DeviceConfig_Role_SENSOR},
	{"tracker", meshtastic_Config_DeviceConfig_Role_TRACKER},
	{"tak", meshtastic_Config_DeviceConfig_Role_TAK},
	{"client_hidden", meshtastic_Config_DeviceConfig_Role_CLIENT_HIDDEN},
	{"lost_and_found", meshtastic_Config_DeviceConfig_Role_LOST_AND_FOUND},
	{"tak_tracker", meshtastic_Config_DeviceConfig_Role_TAK_TRACKER},
};

static const struct {
	const char *name;
	meshtastic_Config_DeviceConfig_RebroadcastMode mode;
} rebroadcast_modes[] = {
	{"all", meshtastic_Config_DeviceConfig_RebroadcastMode_ALL},
	{"all_skip_decoding", meshtastic_Config_DeviceConfig_RebroadcastMode_ALL_SKIP_DECODING},
	{"local_only", meshtastic_Config_DeviceConfig_RebroadcastMode_LOCAL_ONLY},
	{"known_only", meshtastic_Config_DeviceConfig_RebroadcastMode_KNOWN_ONLY},
	{"none", meshtastic_Config_DeviceConfig_RebroadcastMode_NONE},
	{"core_portnums_only",
	 meshtastic_Config_DeviceConfig_RebroadcastMode_CORE_PORTNUMS_ONLY},
};

ZTEST_SUITE(shell_names, NULL, shell_test_setup, shell_test_before, NULL, NULL);

ZTEST(shell_names, test_every_device_role_can_be_set_by_name)
{
	ARRAY_FOR_EACH(device_roles, i) {
		char cmd[64];

		snprintk(cmd, sizeof(cmd), "meshtastic device role %s", device_roles[i].name);

		zassert_ok(shell_run(cmd), "the shell rejected role %s", device_roles[i].name);
		zassert_equal(meshtastic_device_role(), device_roles[i].role);
	}
}

ZTEST(shell_names, test_every_device_role_prints_its_own_name)
{
	ARRAY_FOR_EACH(device_roles, i) {
		char expected[64];

		meshtastic_set_device_role(device_roles[i].role);

		zassert_ok(shell_run("meshtastic device role"));
		snprintk(expected, sizeof(expected), "role: %s\r\n", device_roles[i].name);
		shell_expect(expected);
	}
}

ZTEST(shell_names, test_every_rebroadcast_mode_can_be_set_by_name)
{
	ARRAY_FOR_EACH(rebroadcast_modes, i) {
		char cmd[64];

		snprintk(cmd, sizeof(cmd), "meshtastic device rebroadcast %s",
			 rebroadcast_modes[i].name);

		zassert_ok(shell_run(cmd), "the shell rejected mode %s",
			   rebroadcast_modes[i].name);
		zassert_equal(meshtastic_rebroadcast_mode(), rebroadcast_modes[i].mode);
	}
}

ZTEST(shell_names, test_every_rebroadcast_mode_prints_its_own_name)
{
	ARRAY_FOR_EACH(rebroadcast_modes, i) {
		char expected[64];

		meshtastic_set_rebroadcast_mode(rebroadcast_modes[i].mode);

		zassert_ok(shell_run("meshtastic device rebroadcast"));
		snprintk(expected, sizeof(expected), "rebroadcast: %s\r\n",
			 rebroadcast_modes[i].name);
		shell_expect(expected);
	}
}
