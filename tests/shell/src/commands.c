/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

/**
 * @file
 * @brief The meshtastic shell commands, from status through to text send.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include "meshtastic_channels.h"
#include "meshtastic_core.h"
#include "meshtastic_packet.h"

#include "mock_lora.h"
#include "shell_fixture.h"

#define TEST_PSK_HEX "000102030405060708090a0b0c0d0e0f"

ZTEST_SUITE(shell_commands, NULL, shell_test_setup, shell_test_before, NULL, NULL);


ZTEST(shell_commands, test_status_reports_the_node_and_the_primary_channel)
{
	zassert_ok(shell_run("meshtastic status"));

	shell_expect("node: 0x12345678");
	shell_expect("initialized: yes");
	shell_expect("primary channel: 0 \"LongFast\" hash=0x08");
	shell_expect("device role: client");
	shell_expect("rebroadcast mode: all");
	shell_expect("rebroadcasting: yes");
}

ZTEST(shell_commands, test_channel_list_shows_every_slot)
{
	zassert_ok(shell_run("meshtastic channel list"));

	shell_expect("[0] role=primary name=\"LongFast\" hash=0x08");
	shell_expect("[7] role=disabled");
}

ZTEST(shell_commands, test_channel_show_prints_the_key_material)
{
	zassert_ok(shell_run("meshtastic channel show 0"));

	shell_expect("role: primary");
	shell_expect("name: \"LongFast\"");
	shell_expect("hash: 0x08");
	shell_expect("psk hex: d4f1bb3a20290759f0bcffabcf4e6901");
}

ZTEST(shell_commands, test_channel_show_rejects_a_bad_index)
{
	zassert_equal(shell_run("meshtastic channel show 8"), -EINVAL);
	shell_expect("invalid channel index: 8");

	zassert_equal(shell_run("meshtastic channel show"), -EINVAL);
	shell_expect("usage:");
}

ZTEST(shell_commands, test_channel_set_updates_a_slot)
{
	zassert_ok(shell_run("meshtastic channel set 1 name Private role secondary psk default "
		       "uplink on downlink off"));

	shell_expect("channel 1 updated");
	zassert_str_equal(meshtastic_channels_get_name(1U), "Private");
	zassert_equal(meshtastic_channels_get(1U)->role, meshtastic_Channel_Role_SECONDARY);
	zassert_true(meshtastic_channels_uplink_enabled(1U));
	zassert_false(meshtastic_channels_downlink_enabled(1U));
}

ZTEST(shell_commands, test_channel_set_accepts_a_hex_key)
{
	struct meshtastic_channel_key key;

	zassert_ok(shell_run("meshtastic channel set 1 role secondary psk hex " TEST_PSK_HEX));

	zassert_ok(meshtastic_channels_get_key(1U, &key));
	zassert_equal(key.len, 16U);
	zassert_equal(key.bytes[0], 0x00U);
	zassert_equal(key.bytes[15], 0x0FU);
}

ZTEST(shell_commands, test_channel_set_rejects_bad_arguments)
{
	zassert_equal(shell_run("meshtastic channel set 1"), -EINVAL);
	shell_expect("usage:");

	zassert_equal(shell_run("meshtastic channel set 8 name Private"), -EINVAL);
	shell_expect("invalid channel index: 8");

	zassert_equal(shell_run("meshtastic channel set 1 name"), -EINVAL);
	shell_expect("name requires a value");

	zassert_equal(shell_run("meshtastic channel set 1 role"), -EINVAL);
	shell_expect("role requires a value");

	zassert_equal(shell_run("meshtastic channel set 1 psk"), -EINVAL);
	shell_expect("psk requires a value");

	zassert_equal(shell_run("meshtastic channel set 1 psk hex"), -EINVAL);
	shell_expect("psk hex requires hex digits");

	zassert_equal(shell_run("meshtastic channel set 1 uplink"), -EINVAL);
	shell_expect("uplink requires on|off");

	zassert_equal(shell_run("meshtastic channel set 1 downlink"), -EINVAL);
	shell_expect("downlink requires on|off");

	zassert_equal(shell_run("meshtastic channel set 1 role bogus"), -EINVAL);
	shell_expect("invalid channel role: bogus");

	zassert_equal(shell_run("meshtastic channel set 1 psk 11"), -EINVAL);
	shell_expect("invalid psk: 11");

	zassert_equal(shell_run("meshtastic channel set 1 psk hex abcd"), -EINVAL);
	shell_expect("psk hex must be 32 or 64 characters");

	zassert_equal(shell_run("meshtastic channel set 1 psk hex "
			  "zz0102030405060708090a0b0c0d0e0f"),
		      -EINVAL);
	shell_expect("invalid hex in psk");

	zassert_equal(shell_run("meshtastic channel set 1 bogus on"), -EINVAL);
	shell_expect("unknown option: bogus");
}

ZTEST(shell_commands, test_channel_disable_clears_a_slot)
{
	zassert_ok(shell_run("meshtastic channel set 1 role secondary name Private"));
	zassert_ok(shell_run("meshtastic channel disable 1"));

	shell_expect("channel 1 disabled");
	zassert_equal(meshtastic_channels_get(1U)->role, meshtastic_Channel_Role_DISABLED);

	zassert_equal(shell_run("meshtastic channel disable"), -EINVAL);
}

ZTEST(shell_commands, test_device_role_can_be_read_and_written)
{
	zassert_ok(shell_run("meshtastic device role"));
	shell_expect("role: client");

	zassert_ok(shell_run("meshtastic device role router"));
	shell_expect("role set to router");
	zassert_equal(meshtastic_device_role(), meshtastic_Config_DeviceConfig_Role_ROUTER);

	zassert_equal(shell_run("meshtastic device role bogus"), -EINVAL);
	shell_expect("invalid device role: bogus");
}

ZTEST(shell_commands, test_device_commands_take_at_most_one_argument)
{
	zassert_equal(shell_run("meshtastic device role client extra"), -EINVAL);
	shell_expect("usage: meshtastic device role [name]");

	zassert_equal(shell_run("meshtastic device rebroadcast all extra"), -EINVAL);
	shell_expect("usage: meshtastic device rebroadcast [mode]");
}

ZTEST(shell_commands, test_rebroadcast_mode_can_be_read_and_written)
{
	zassert_ok(shell_run("meshtastic device rebroadcast"));
	shell_expect("rebroadcast: all");

	zassert_ok(shell_run("meshtastic device rebroadcast known_only"));
	shell_expect("rebroadcast set to known_only");
	zassert_equal(meshtastic_rebroadcast_mode(),
		      meshtastic_Config_DeviceConfig_RebroadcastMode_KNOWN_ONLY);

	zassert_equal(shell_run("meshtastic device rebroadcast bogus"), -EINVAL);
	shell_expect("invalid rebroadcast mode: bogus");
}

ZTEST(shell_commands, test_nodedb_lists_and_shows_the_local_node)
{
	zassert_ok(shell_run("meshtastic nodedb list"));
	shell_expect("0x12345678");

	zassert_ok(shell_run("meshtastic nodedb show 0x12345678"));
	shell_expect("node: 0x12345678");
	shell_expect("long name: Zephyr Test Node");
	shell_expect("short name: ZTN");

	zassert_true(shell_run("meshtastic nodedb show 0xdeadbeef") < 0);
	shell_expect("not found");

	zassert_equal(shell_run("meshtastic nodedb show"), -EINVAL);
	zassert_equal(shell_run("meshtastic nodedb show notanumber"), -EINVAL);
	shell_expect("invalid integer: notanumber");
}

ZTEST(shell_commands, test_text_send_broadcasts_the_message)
{
	struct meshtastic_packet sent;
	uint8_t payload[MESHTASTIC_MAX_PAYLOAD_LEN];

	zassert_ok(shell_run("meshtastic text send broadcast hello mesh"));
	shell_expect("queued");

	shell_deferred_tx(&sent, payload, sizeof(payload));
	zassert_equal(sent.to, MESHTASTIC_NODE_BROADCAST);
	zassert_equal(sent.portnum, MESHTASTIC_PORT_TEXT_MESSAGE);
	zassert_equal(sent.payload_len, strlen("hello mesh"));
	zassert_mem_equal(payload, "hello mesh", strlen("hello mesh"),
			  "arguments should be rejoined with spaces");
}

ZTEST(shell_commands, test_a_lone_word_is_broadcast_without_naming_a_destination)
{
	struct meshtastic_packet sent;
	uint8_t payload[MESHTASTIC_MAX_PAYLOAD_LEN];

	zassert_ok(shell_run("meshtastic text send hello"));

	shell_deferred_tx(&sent, payload, sizeof(payload));
	zassert_equal(sent.to, MESHTASTIC_NODE_BROADCAST);
	zassert_equal(sent.payload_len, strlen("hello"));
}

ZTEST(shell_commands, test_text_send_honours_the_destination_and_channel)
{
	struct meshtastic_packet sent;
	uint8_t payload[MESHTASTIC_MAX_PAYLOAD_LEN];

	zassert_ok(shell_run("meshtastic channel set 1 role secondary name Private"));
	mock_lora_reset();

	zassert_ok(shell_run("meshtastic text send -c 1 0x87654321 hi"));

	shell_deferred_tx(&sent, payload, sizeof(payload));
	zassert_equal(sent.to, SHELL_TEST_PEER_ID);
	zassert_equal(sent.channel_index, 1U);
}

ZTEST(shell_commands, test_text_send_can_request_an_acknowledgement)
{
	struct meshtastic_packet sent;
	uint8_t payload[MESHTASTIC_MAX_PAYLOAD_LEN];

	zassert_ok(shell_run("meshtastic text send -a 0x87654321 ack me"));

	shell_deferred_tx(&sent, payload, sizeof(payload));
	zassert_true(sent.want_ack);
}

ZTEST(shell_commands, test_text_send_drops_the_ack_request_on_a_broadcast)
{
	struct meshtastic_packet sent;
	uint8_t payload[MESHTASTIC_MAX_PAYLOAD_LEN];

	zassert_ok(shell_run("meshtastic text send -a broadcast hello"));
	shell_expect("broadcasts are not acknowledged");

	shell_deferred_tx(&sent, payload, sizeof(payload));
	zassert_false(sent.want_ack);
}

ZTEST(shell_commands, test_text_send_accepts_a_maximum_length_message)
{
	char cmd[64 + MESHTASTIC_MAX_TEXT_LEN];
	struct meshtastic_packet sent;
	uint8_t payload[MESHTASTIC_MAX_PAYLOAD_LEN];
	size_t prefix;

	strcpy(cmd, "meshtastic text send broadcast ");
	prefix = strlen(cmd);
	memset(&cmd[prefix], 'x', MESHTASTIC_MAX_TEXT_LEN);
	cmd[prefix + MESHTASTIC_MAX_TEXT_LEN] = '\0';

	zassert_ok(shell_run(cmd));

	shell_deferred_tx(&sent, payload, sizeof(payload));
	zassert_equal(sent.payload_len, MESHTASTIC_MAX_TEXT_LEN);
	zassert_mem_equal(payload, &cmd[prefix], MESHTASTIC_MAX_TEXT_LEN);
}

ZTEST(shell_commands, test_text_send_rejects_bad_arguments)
{
	char long_message[64 + MESHTASTIC_MAX_TEXT_LEN];

	zassert_equal(shell_run("meshtastic text send"), -EINVAL);
	zassert_equal(shell_run("meshtastic text send -c"), -EINVAL);
	zassert_equal(shell_run("meshtastic text send -c 9 hello"), -EINVAL);
	shell_expect("invalid channel index: 9");

	zassert_equal(shell_run("meshtastic text send notanumber hello"), -EINVAL);
	shell_expect("invalid integer: notanumber");

	strcpy(long_message, "meshtastic text send broadcast ");
	memset(long_message + strlen(long_message), 'x', MESHTASTIC_MAX_TEXT_LEN + 1U);
	long_message[strlen("meshtastic text send broadcast ") + MESHTASTIC_MAX_TEXT_LEN + 1U] =
		'\0';
	zassert_true(shell_run(long_message) < 0);
	shell_expect("message too long");
}

ZTEST(shell_commands, test_metrics_send_queues_a_telemetry_packet)
{
	struct meshtastic_packet sent;
	uint8_t payload[MESHTASTIC_MAX_PAYLOAD_LEN];

	zassert_ok(shell_run("meshtastic metrics send"));
	shell_expect("queued");

	shell_deferred_tx(&sent, payload, sizeof(payload));
	zassert_equal(sent.portnum, MESHTASTIC_PORT_TELEMETRY);
	zassert_equal(sent.to, MESHTASTIC_NODE_BROADCAST);
}

ZTEST(shell_commands, test_nodeinfo_send_queues_an_announcement)
{
	struct meshtastic_packet sent;
	uint8_t payload[MESHTASTIC_MAX_PAYLOAD_LEN];

	zassert_ok(shell_run("meshtastic nodeinfo send 0x87654321"));

	shell_deferred_tx(&sent, payload, sizeof(payload));
	zassert_equal(sent.portnum, MESHTASTIC_PORT_NODEINFO);
	zassert_equal(sent.to, SHELL_TEST_PEER_ID);
}

ZTEST(shell_commands, test_a_deferred_send_reports_a_failure)
{
	zassert_ok(shell_run("meshtastic environment send"));

	k_msleep(200);
	zassert_equal(mock_lora_send_count(), 0U);
	shell_expect("no sensor readings available");
}

ZTEST(shell_commands, test_deferred_commands_reject_extra_arguments)
{
	zassert_equal(shell_run("meshtastic metrics send broadcast extra"), -EINVAL);
	shell_expect("too many arguments");
}
