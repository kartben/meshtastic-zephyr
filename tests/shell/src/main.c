/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_dummy.h>
#include <zephyr/ztest.h>

#include <zephyr/meshtastic/meshtastic.h>

#include "meshtastic_channels.h"
#include "meshtastic_config_store.h"
#include "meshtastic_core.h"
#include "meshtastic_packet.h"

#include "mock_lora.h"

#define TEST_NODE_ID 0x12345678U
#define PEER_NODE_ID 0x87654321U

#define TEST_PSK_HEX "000102030405060708090a0b0c0d0e0f"

static struct meshtastic_config cfg = {
	.node_id = TEST_NODE_ID,
	.psk = meshtastic_default_psk,
	.psk_len = sizeof(meshtastic_default_psk),
	.channel_name = MESHTASTIC_CHANNEL_LONGFAST,
	.frequency = MESHTASTIC_FREQ_EU,
	.long_name = "Zephyr Test Node",
	.short_name = "ZTN",
};

static const struct shell *sh;
static char captured[4096];
static size_t captured_len;

/*
 * Reading the dummy backend drains it, so keep our own copy: a command's
 * output can arrive in several chunks once the shell thread flushes.
 */
static bool output_contains(const char *needle)
{
	size_t len;
	const char *chunk = shell_backend_dummy_get_output(sh, &len);

	if (len > 0U && (captured_len + len) < sizeof(captured)) {
		memcpy(&captured[captured_len], chunk, len);
		captured_len += len;
		captured[captured_len] = '\0';
	}

	return strstr(captured, needle) != NULL;
}

static void assert_output_contains(const char *needle)
{
	WAIT_FOR(output_contains(needle), 500000, k_msleep(1));
	zassert_true(output_contains(needle), "expected \"%s\" in:\n%s", needle, captured);
}

/* Run a command and return the shell's exit code. */
static int run(const char *cmd)
{
	/* Let whatever the previous command printed land before dropping it. */
	k_msleep(20);
	shell_backend_dummy_clear_output(sh);
	captured_len = 0U;
	captured[0] = '\0';

	return shell_execute_cmd(sh, cmd);
}

/* Deferred commands are handed to a worker thread; wait for the frame it sends. */
static void decode_deferred_tx(struct meshtastic_packet *packet, uint8_t *payload,
			       size_t payload_len)
{
	uint8_t wire[MESHTASTIC_PKT_MAX];
	uint32_t wire_len;

	mock_lora_wait_for_send_count(1U, K_MSEC(1000));
	wire_len = mock_lora_last_tx(wire, sizeof(wire));
	zassert_ok(meshtastic_decode_wire_packet(wire, (int)wire_len, 0, 0, packet, payload,
						 payload_len),
		   "could not decode the transmitted frame");
}

static void *setup(void)
{
	cfg.lora_dev = mock_lora_device();
	zassert_ok(meshtastic_init(&cfg), "meshtastic_init failed");

	sh = shell_backend_dummy_get_ptr();
	zassert_not_null(sh, "dummy shell backend missing");
	/* The dummy backend needs one processing pass before it accepts commands. */
	shell_process(sh);

	return NULL;
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);

	zassert_ok(meshtastic_channels_init_from_config(&cfg));
	zassert_ok(meshtastic_config_store_seed(&cfg));
	meshtastic_set_device_role(meshtastic_Config_DeviceConfig_Role_CLIENT);
	meshtastic_set_rebroadcast_mode(meshtastic_Config_DeviceConfig_RebroadcastMode_ALL);
	mock_lora_reset();
	shell_backend_dummy_clear_output(sh);
	captured_len = 0U;
	captured[0] = '\0';
}

ZTEST_SUITE(shell_commands, NULL, setup, before, NULL, NULL);

ZTEST(shell_commands, test_status_reports_the_node_and_the_primary_channel)
{
	zassert_ok(run("meshtastic status"));

	assert_output_contains("node: 0x12345678");
	assert_output_contains("initialized: yes");
	assert_output_contains("primary channel: 0 \"LongFast\" hash=0x08");
	assert_output_contains("device role: client");
	assert_output_contains("rebroadcast mode: all");
	assert_output_contains("rebroadcasting: yes");
}

ZTEST(shell_commands, test_channel_list_shows_every_slot)
{
	zassert_ok(run("meshtastic channel list"));

	assert_output_contains("[0] role=primary name=\"LongFast\" hash=0x08");
	assert_output_contains("[7] role=disabled");
}

ZTEST(shell_commands, test_channel_show_prints_the_key_material)
{
	zassert_ok(run("meshtastic channel show 0"));

	assert_output_contains("role: primary");
	assert_output_contains("name: \"LongFast\"");
	assert_output_contains("hash: 0x08");
	assert_output_contains("psk hex: d4f1bb3a20290759f0bcffabcf4e6901");
}

ZTEST(shell_commands, test_channel_show_rejects_a_bad_index)
{
	zassert_equal(run("meshtastic channel show 8"), -EINVAL);
	assert_output_contains("invalid channel index: 8");

	zassert_equal(run("meshtastic channel show"), -EINVAL);
	assert_output_contains("usage:");
}

ZTEST(shell_commands, test_channel_set_updates_a_slot)
{
	zassert_ok(run("meshtastic channel set 1 name Private role secondary psk default "
		       "uplink on downlink off"));

	assert_output_contains("channel 1 updated");
	zassert_str_equal(meshtastic_channels_get_name(1U), "Private");
	zassert_equal(meshtastic_channels_get(1U)->role, meshtastic_Channel_Role_SECONDARY);
	zassert_true(meshtastic_channels_uplink_enabled(1U));
	zassert_false(meshtastic_channels_downlink_enabled(1U));
}

ZTEST(shell_commands, test_channel_set_accepts_a_hex_key)
{
	struct meshtastic_channel_key key;

	zassert_ok(run("meshtastic channel set 1 role secondary psk hex " TEST_PSK_HEX));

	zassert_ok(meshtastic_channels_get_key(1U, &key));
	zassert_equal(key.len, 16U);
	zassert_equal(key.bytes[0], 0x00U);
	zassert_equal(key.bytes[15], 0x0FU);
}

ZTEST(shell_commands, test_channel_set_rejects_bad_arguments)
{
	zassert_equal(run("meshtastic channel set 1"), -EINVAL);
	assert_output_contains("usage:");

	zassert_equal(run("meshtastic channel set 1 name"), -EINVAL);
	assert_output_contains("name requires a value");

	zassert_equal(run("meshtastic channel set 1 role bogus"), -EINVAL);
	assert_output_contains("invalid channel role: bogus");

	zassert_equal(run("meshtastic channel set 1 psk 11"), -EINVAL);
	assert_output_contains("invalid psk: 11");

	zassert_equal(run("meshtastic channel set 1 psk hex abcd"), -EINVAL);
	assert_output_contains("psk hex must be 32 or 64 characters");

	zassert_equal(run("meshtastic channel set 1 psk hex "
			  "zz0102030405060708090a0b0c0d0e0f"),
		      -EINVAL);
	assert_output_contains("invalid hex in psk");

	zassert_equal(run("meshtastic channel set 1 bogus on"), -EINVAL);
	assert_output_contains("unknown option: bogus");
}

ZTEST(shell_commands, test_channel_disable_clears_a_slot)
{
	zassert_ok(run("meshtastic channel set 1 role secondary name Private"));
	zassert_ok(run("meshtastic channel disable 1"));

	assert_output_contains("channel 1 disabled");
	zassert_equal(meshtastic_channels_get(1U)->role, meshtastic_Channel_Role_DISABLED);

	zassert_equal(run("meshtastic channel disable"), -EINVAL);
}

ZTEST(shell_commands, test_device_role_can_be_read_and_written)
{
	zassert_ok(run("meshtastic device role"));
	assert_output_contains("role: client");

	zassert_ok(run("meshtastic device role router"));
	assert_output_contains("role set to router");
	zassert_equal(meshtastic_device_role(), meshtastic_Config_DeviceConfig_Role_ROUTER);

	zassert_equal(run("meshtastic device role bogus"), -EINVAL);
	assert_output_contains("invalid device role: bogus");
}

ZTEST(shell_commands, test_rebroadcast_mode_can_be_read_and_written)
{
	zassert_ok(run("meshtastic device rebroadcast"));
	assert_output_contains("rebroadcast: all");

	zassert_ok(run("meshtastic device rebroadcast known_only"));
	assert_output_contains("rebroadcast set to known_only");
	zassert_equal(meshtastic_rebroadcast_mode(),
		      meshtastic_Config_DeviceConfig_RebroadcastMode_KNOWN_ONLY);

	zassert_equal(run("meshtastic device rebroadcast bogus"), -EINVAL);
	assert_output_contains("invalid rebroadcast mode: bogus");
}

ZTEST(shell_commands, test_nodedb_lists_and_shows_the_local_node)
{
	zassert_ok(run("meshtastic nodedb list"));
	assert_output_contains("0x12345678");

	zassert_ok(run("meshtastic nodedb show 0x12345678"));
	assert_output_contains("node: 0x12345678");
	assert_output_contains("long name: Zephyr Test Node");
	assert_output_contains("short name: ZTN");

	zassert_true(run("meshtastic nodedb show 0xdeadbeef") < 0);
	assert_output_contains("not found");

	zassert_equal(run("meshtastic nodedb show"), -EINVAL);
	zassert_equal(run("meshtastic nodedb show notanumber"), -EINVAL);
	assert_output_contains("invalid integer: notanumber");
}

ZTEST(shell_commands, test_text_send_broadcasts_the_message)
{
	struct meshtastic_packet sent;
	uint8_t payload[MESHTASTIC_MAX_PAYLOAD_LEN];

	zassert_ok(run("meshtastic text send broadcast hello mesh"));
	assert_output_contains("queued");

	decode_deferred_tx(&sent, payload, sizeof(payload));
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

	zassert_ok(run("meshtastic text send hello"));

	decode_deferred_tx(&sent, payload, sizeof(payload));
	zassert_equal(sent.to, MESHTASTIC_NODE_BROADCAST);
	zassert_equal(sent.payload_len, strlen("hello"));
}

ZTEST(shell_commands, test_text_send_honours_the_destination_and_channel)
{
	struct meshtastic_packet sent;
	uint8_t payload[MESHTASTIC_MAX_PAYLOAD_LEN];

	zassert_ok(run("meshtastic channel set 1 role secondary name Private"));
	mock_lora_reset();

	zassert_ok(run("meshtastic text send -c 1 0x87654321 hi"));

	decode_deferred_tx(&sent, payload, sizeof(payload));
	zassert_equal(sent.to, PEER_NODE_ID);
	zassert_equal(sent.channel_index, 1U);
}

ZTEST(shell_commands, test_text_send_can_request_an_acknowledgement)
{
	struct meshtastic_packet sent;
	uint8_t payload[MESHTASTIC_MAX_PAYLOAD_LEN];

	zassert_ok(run("meshtastic text send -a 0x87654321 ack me"));

	decode_deferred_tx(&sent, payload, sizeof(payload));
	zassert_true(sent.want_ack);
}

ZTEST(shell_commands, test_text_send_drops_the_ack_request_on_a_broadcast)
{
	struct meshtastic_packet sent;
	uint8_t payload[MESHTASTIC_MAX_PAYLOAD_LEN];

	zassert_ok(run("meshtastic text send -a broadcast hello"));
	assert_output_contains("broadcasts are not acknowledged");

	decode_deferred_tx(&sent, payload, sizeof(payload));
	zassert_false(sent.want_ack);
}

ZTEST(shell_commands, test_text_send_rejects_bad_arguments)
{
	char long_message[64 + MESHTASTIC_MAX_TEXT_LEN];

	zassert_equal(run("meshtastic text send"), -EINVAL);
	zassert_equal(run("meshtastic text send -c"), -EINVAL);
	zassert_equal(run("meshtastic text send -c 9 hello"), -EINVAL);
	assert_output_contains("invalid channel index: 9");

	zassert_equal(run("meshtastic text send notanumber hello"), -EINVAL);
	assert_output_contains("invalid integer: notanumber");

	strcpy(long_message, "meshtastic text send broadcast ");
	memset(long_message + strlen(long_message), 'x', MESHTASTIC_MAX_TEXT_LEN + 1U);
	long_message[strlen("meshtastic text send broadcast ") + MESHTASTIC_MAX_TEXT_LEN + 1U] =
		'\0';
	zassert_true(run(long_message) < 0);
	assert_output_contains("message too long");
}

ZTEST(shell_commands, test_metrics_send_queues_a_telemetry_packet)
{
	struct meshtastic_packet sent;
	uint8_t payload[MESHTASTIC_MAX_PAYLOAD_LEN];

	zassert_ok(run("meshtastic metrics send"));
	assert_output_contains("queued");

	decode_deferred_tx(&sent, payload, sizeof(payload));
	zassert_equal(sent.portnum, MESHTASTIC_PORT_TELEMETRY);
	zassert_equal(sent.to, MESHTASTIC_NODE_BROADCAST);
}

ZTEST(shell_commands, test_nodeinfo_send_queues_an_announcement)
{
	struct meshtastic_packet sent;
	uint8_t payload[MESHTASTIC_MAX_PAYLOAD_LEN];

	zassert_ok(run("meshtastic nodeinfo send 0x87654321"));

	decode_deferred_tx(&sent, payload, sizeof(payload));
	zassert_equal(sent.portnum, MESHTASTIC_PORT_NODEINFO);
	zassert_equal(sent.to, PEER_NODE_ID);
}

ZTEST(shell_commands, test_a_deferred_send_reports_a_failure)
{
	zassert_ok(run("meshtastic environment send"));

	k_msleep(200);
	zassert_equal(mock_lora_send_count(), 0U);
	assert_output_contains("no sensor readings available");
}

ZTEST(shell_commands, test_deferred_commands_reject_extra_arguments)
{
	zassert_equal(run("meshtastic metrics send broadcast extra"), -EINVAL);
	assert_output_contains("too many arguments");
}
