/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

/**
 * @file
 * @brief Device side of the MQTT gateway test.
 *
 * Boots the stack against the mock radio and exposes the handful of knobs the
 * pytest suite needs to drive the mesh. The broker is a real mosquitto running
 * on the host, reached over TCP through the native simulator's offloaded
 * sockets.
 */

#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>

#include <zephyr/meshtastic/meshtastic.h>

#include "meshtastic_channels.h"
#include "meshtastic_core.h"
#include "meshtastic_packet.h"

#include "mock_lora.h"

#define TEST_NODE_ID 0x12345678U

static struct meshtastic_config cfg = {
	.node_id = TEST_NODE_ID,
	.psk = meshtastic_default_psk,
	.psk_len = sizeof(meshtastic_default_psk),
	.channel_name = MESHTASTIC_CHANNEL_LONGFAST,
	.frequency = MESHTASTIC_FREQ_EU,
};

static int parse_onoff(const char *arg, bool *out)
{
	if (strcmp(arg, "on") == 0) {
		*out = true;
	} else if (strcmp(arg, "off") == 0) {
		*out = false;
	} else {
		return -EINVAL;
	}

	return 0;
}

static int set_primary_flags(bool uplink, bool downlink)
{
	meshtastic_Channel ch = *meshtastic_channels_get(0U);

	ch.settings.uplink_enabled = uplink;
	ch.settings.downlink_enabled = downlink;

	return meshtastic_channels_set_slot(0U, &ch);
}

/*
 * Hands the radio a frame built as if a peer had sent it. hop_limit is zero so
 * the router has nothing to rebroadcast and the only radio traffic a test sees
 * is what it asked for.
 */
static int cmd_hear(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t wire[MESHTASTIC_PKT_MAX];
	char hex[2 * MESHTASTIC_PKT_MAX + 1];
	uint32_t wire_len;
	struct meshtastic_packet packet = {
		.from = (uint32_t)strtoul(argv[1], NULL, 16),
		.to = MESHTASTIC_NODE_BROADCAST,
		.id = (uint32_t)strtoul(argv[2], NULL, 16),
		.portnum = MESHTASTIC_PORT_TEXT_MESSAGE,
		.payload = (const uint8_t *)argv[3],
		.payload_len = strlen(argv[3]),
		.hop_limit = 0U,
		.hop_start = 3U,
		.channel_index = meshtastic_channels_primary_index(),
	};
	int ret = meshtastic_build_wire_packet(&packet, wire, &wire_len);

	if (ret != 0) {
		shell_error(sh, "build failed (%d)", ret);
		return ret;
	}

	mock_lora_inject_rx(wire, wire_len, -50, 10);

	/* Echo the frame so the test can check it against what gets published. */
	bin2hex(wire, wire_len, hex, sizeof(hex));
	shell_print(sh, "heard %s", hex);

	return 0;
}

static int cmd_send(const struct shell *sh, size_t argc, char **argv)
{
	int ret = meshtastic_send_text(MESHTASTIC_NODE_BROADCAST, argv[1]);

	if (ret != 0) {
		shell_error(sh, "send failed (%d)", ret);
		return ret;
	}

	shell_print(sh, "sent");

	return 0;
}

static int cmd_tx(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "tx %u", mock_lora_send_count());

	return 0;
}

static int cmd_uplink(const struct shell *sh, size_t argc, char **argv)
{
	bool enabled;

	if (parse_onoff(argv[1], &enabled) != 0) {
		shell_error(sh, "expected on or off");
		return -EINVAL;
	}

	return set_primary_flags(enabled, meshtastic_channels_downlink_enabled(0U));
}

static int cmd_downlink(const struct shell *sh, size_t argc, char **argv)
{
	bool enabled;

	if (parse_onoff(argv[1], &enabled) != 0) {
		shell_error(sh, "expected on or off");
		return -EINVAL;
	}

	return set_primary_flags(meshtastic_channels_uplink_enabled(0U), enabled);
}

static int cmd_reset(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	mock_lora_reset();
	meshtastic_channels_init_from_config(&cfg);
	shell_print(sh, "reset");

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	gw_cmds, SHELL_CMD_ARG(hear, NULL, "<from> <id> <text>", cmd_hear, 4, 0),
	SHELL_CMD_ARG(send, NULL, "<text>", cmd_send, 2, 0),
	SHELL_CMD_ARG(tx, NULL, "radio transmit count", cmd_tx, 1, 0),
	SHELL_CMD_ARG(uplink, NULL, "<on|off>", cmd_uplink, 2, 0),
	SHELL_CMD_ARG(downlink, NULL, "<on|off>", cmd_downlink, 2, 0),
	SHELL_CMD_ARG(reset, NULL, "clear radio and channel state", cmd_reset, 1, 0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(gw, &gw_cmds, "MQTT gateway test control", NULL);

int main(void)
{
	cfg.lora_dev = mock_lora_device();

	return meshtastic_init(&cfg);
}
