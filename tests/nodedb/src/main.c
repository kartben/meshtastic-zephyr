/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <pb_encode.h>

#include <zephyr/meshtastic/meshtastic.h>
#include <zephyr/meshtastic/nodedb.h>

#include "meshtastic_core.h"
#include "meshtastic_modules.h"

#include "mock_lora.h"

/* Declared privately by the stack; the suite drives a clean table per test. */
int meshtastic_nodedb_init(void);

#define TEST_NODE_ID  0x12345678U
#define PEER_NODE_ID  0x87654321U
#define OTHER_NODE_ID 0x13572468U

static struct meshtastic_config cfg = {
	.node_id = TEST_NODE_ID,
	.psk = meshtastic_default_psk,
	.psk_len = sizeof(meshtastic_default_psk),
	.channel_name = MESHTASTIC_CHANNEL_LONGFAST,
	.frequency = MESHTASTIC_FREQ_EU,
	.long_name = "Zephyr Test Node",
	.short_name = "ZTN",
};

static uint8_t payload_buf[MESHTASTIC_MAX_PAYLOAD_LEN];

static size_t encode(const pb_msgdesc_t *fields, const void *msg)
{
	pb_ostream_t stream = pb_ostream_from_buffer(payload_buf, sizeof(payload_buf));

	zassert_true(pb_encode(&stream, fields, msg), "protobuf encode failed");

	return stream.bytes_written;
}

static void deliver_nodeinfo(uint32_t from, const char *long_name, const char *short_name)
{
	meshtastic_User user = meshtastic_User_init_zero;
	struct meshtastic_packet packet = {
		.from = from,
		.to = MESHTASTIC_NODE_BROADCAST,
		.portnum = MESHTASTIC_PORT_NODEINFO,
		.payload = payload_buf,
	};

	snprintk(user.id, sizeof(user.id), "!%08x", from);
	strcpy(user.long_name, long_name);
	strcpy(user.short_name, short_name);
	user.hw_model = meshtastic_HardwareModel_TBEAM;
	user.role = meshtastic_Config_DeviceConfig_Role_ROUTER;

	packet.payload_len = encode(meshtastic_User_fields, &user);
	meshtastic_dispatch_modules(&packet);
}

static struct meshtastic_nodedb_node lookup(uint32_t node)
{
	struct meshtastic_nodedb_node out;

	zassert_ok(meshtastic_nodedb_get(node, &out), "node 0x%08x missing", node);

	return out;
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

	zassert_ok(meshtastic_nodedb_init(), "nodedb reset failed");
}

ZTEST_SUITE(nodedb, NULL, setup, before, NULL, NULL);

ZTEST(nodedb, test_init_seeds_the_local_node)
{
	struct meshtastic_nodedb_node local = lookup(TEST_NODE_ID);

	zassert_equal(meshtastic_nodedb_count(), 1U);
	zassert_equal(local.num, TEST_NODE_ID);
	zassert_true(local.has_user);
	zassert_str_equal(local.long_name, cfg.long_name);
	zassert_str_equal(local.short_name, cfg.short_name);
}

ZTEST(nodedb, test_nodeinfo_packet_records_the_peer_user)
{
	struct meshtastic_nodedb_node peer;

	deliver_nodeinfo(PEER_NODE_ID, "Peer Node", "PEER");

	peer = lookup(PEER_NODE_ID);
	zassert_equal(meshtastic_nodedb_count(), 2U);
	zassert_true(peer.has_user);
	zassert_str_equal(peer.long_name, "Peer Node");
	zassert_str_equal(peer.short_name, "PEER");
	zassert_equal(peer.hw_model, meshtastic_HardwareModel_TBEAM);
	zassert_equal(peer.role, meshtastic_Config_DeviceConfig_Role_ROUTER);
	zassert_false(peer.is_licensed);
	zassert_false(peer.has_is_unmessagable);
}

ZTEST(nodedb, test_nodeinfo_carries_licensing_and_public_key_flags)
{
	meshtastic_User user = meshtastic_User_init_zero;
	struct meshtastic_packet packet = {
		.from = PEER_NODE_ID,
		.to = MESHTASTIC_NODE_BROADCAST,
		.portnum = MESHTASTIC_PORT_NODEINFO,
		.payload = payload_buf,
	};
	struct meshtastic_nodedb_node peer;

	user.is_licensed = true;
	user.has_is_unmessagable = true;
	user.is_unmessagable = true;
	user.public_key.size = 32U;
	memset(user.public_key.bytes, 0xABU, user.public_key.size);

	packet.payload_len = encode(meshtastic_User_fields, &user);
	meshtastic_dispatch_modules(&packet);

	peer = lookup(PEER_NODE_ID);
	zassert_true(peer.is_licensed);
	zassert_true(peer.has_is_unmessagable);
	zassert_true(peer.is_unmessagable);
	zassert_equal(peer.public_key_len, 32U);
	zassert_equal(peer.public_key[0], 0xABU);
}

ZTEST(nodedb, test_undecodable_nodeinfo_still_registers_the_sender)
{
	const uint8_t garbage[] = {0xFFU, 0xFFU, 0xFFU};
	struct meshtastic_packet packet = {
		.from = PEER_NODE_ID,
		.to = MESHTASTIC_NODE_BROADCAST,
		.portnum = MESHTASTIC_PORT_NODEINFO,
		.payload = garbage,
		.payload_len = sizeof(garbage),
	};

	meshtastic_dispatch_modules(&packet);

	zassert_false(lookup(PEER_NODE_ID).has_user);
}

ZTEST(nodedb, test_position_packet_records_the_last_known_fix)
{
	meshtastic_Position position = meshtastic_Position_init_zero;
	struct meshtastic_packet packet = {
		.from = PEER_NODE_ID,
		.to = MESHTASTIC_NODE_BROADCAST,
		.portnum = MESHTASTIC_PORT_POSITION,
		.payload = payload_buf,
	};
	struct meshtastic_nodedb_node peer;

	position.has_latitude_i = true;
	position.latitude_i = 486000000;
	position.has_longitude_i = true;
	position.longitude_i = 23000000;
	position.has_altitude = true;
	position.altitude = 340;
	position.time = 1700000000U;
	position.location_source = meshtastic_Position_LocSource_LOC_INTERNAL;
	position.precision_bits = 13U;

	packet.payload_len = encode(meshtastic_Position_fields, &position);
	meshtastic_dispatch_modules(&packet);

	peer = lookup(PEER_NODE_ID);
	zassert_true(peer.has_position);
	zassert_equal(peer.position.latitude_i, 486000000);
	zassert_equal(peer.position.longitude_i, 23000000);
	zassert_equal(peer.position.altitude, 340);
	zassert_equal(peer.position.time, 1700000000U);
	zassert_equal(peer.position.location_source, meshtastic_Position_LocSource_LOC_INTERNAL);
	zassert_equal(peer.position.precision_bits, 13U);
}

ZTEST(nodedb, test_position_request_is_not_stored_as_a_fix)
{
	meshtastic_Position position = meshtastic_Position_init_zero;
	struct meshtastic_packet packet = {
		.from = PEER_NODE_ID,
		.to = MESHTASTIC_NODE_BROADCAST,
		.portnum = MESHTASTIC_PORT_POSITION,
		.payload = payload_buf,
		.want_response = true,
	};

	packet.payload_len = encode(meshtastic_Position_fields, &position);
	meshtastic_dispatch_modules(&packet);

	zassert_false(lookup(PEER_NODE_ID).has_position);
}

ZTEST(nodedb, test_telemetry_packets_record_device_and_environment_metrics)
{
	meshtastic_Telemetry telemetry = meshtastic_Telemetry_init_zero;
	struct meshtastic_packet packet = {
		.from = PEER_NODE_ID,
		.to = MESHTASTIC_NODE_BROADCAST,
		.portnum = MESHTASTIC_PORT_TELEMETRY,
		.payload = payload_buf,
	};
	struct meshtastic_nodedb_node peer;

	telemetry.which_variant = meshtastic_Telemetry_device_metrics_tag;
	telemetry.variant.device_metrics.has_battery_level = true;
	telemetry.variant.device_metrics.battery_level = 77U;
	telemetry.variant.device_metrics.has_voltage = true;
	telemetry.variant.device_metrics.voltage = 3.9f;
	packet.payload_len = encode(meshtastic_Telemetry_fields, &telemetry);
	meshtastic_dispatch_modules(&packet);

	telemetry = (meshtastic_Telemetry)meshtastic_Telemetry_init_zero;
	telemetry.which_variant = meshtastic_Telemetry_environment_metrics_tag;
	telemetry.variant.environment_metrics.has_temperature = true;
	telemetry.variant.environment_metrics.temperature = 21.5f;
	telemetry.variant.environment_metrics.one_wire_temperature_count = 2U;
	telemetry.variant.environment_metrics.one_wire_temperature[0] = 10.0f;
	telemetry.variant.environment_metrics.one_wire_temperature[1] = 11.0f;
	packet.payload_len = encode(meshtastic_Telemetry_fields, &telemetry);
	meshtastic_dispatch_modules(&packet);

	peer = lookup(PEER_NODE_ID);
	zassert_true(peer.has_device_metrics);
	zassert_equal(peer.device_metrics.battery_level, 77U);
	zassert_within(peer.device_metrics.voltage, 3.9f, 0.001f);
	zassert_true(peer.has_environment_metrics);
	zassert_within(peer.environment_metrics.temperature, 21.5f, 0.001f);
	zassert_equal(peer.environment_metrics.one_wire_temperature_count, 2U);
	zassert_within(peer.environment_metrics.one_wire_temperature[1], 11.0f, 0.001f);
}

ZTEST(nodedb, test_node_status_packet_records_the_status_text)
{
	meshtastic_StatusMessage status = meshtastic_StatusMessage_init_zero;
	struct meshtastic_packet packet = {
		.from = PEER_NODE_ID,
		.to = MESHTASTIC_NODE_BROADCAST,
		.portnum = MESHTASTIC_PORT_NODE_STATUS,
		.payload = payload_buf,
	};
	struct meshtastic_nodedb_node peer;

	strcpy(status.status, "on the summit");
	packet.payload_len = encode(meshtastic_StatusMessage_fields, &status);
	meshtastic_dispatch_modules(&packet);

	peer = lookup(PEER_NODE_ID);
	zassert_true(peer.has_status);
	zassert_str_equal(peer.status, "on the summit");
}

ZTEST(nodedb, test_link_metadata_is_taken_from_the_packet_header)
{
	struct meshtastic_packet packet = {
		.from = PEER_NODE_ID,
		.to = MESHTASTIC_NODE_BROADCAST,
		.portnum = MESHTASTIC_PORT_TEXT_MESSAGE,
		.payload = (const uint8_t *)"hi",
		.payload_len = 2U,
		.channel_index = 2U,
		.next_hop = 0x21U,
		.hop_start = 3U,
		.hop_limit = 1U,
		.snr = 7,
		.via_mqtt = true,
	};
	struct meshtastic_nodedb_node peer;

	meshtastic_dispatch_modules(&packet);

	peer = lookup(PEER_NODE_ID);
	zassert_equal(peer.channel, 2U);
	zassert_equal(peer.next_hop, 0x21U);
	zassert_true(peer.via_mqtt);
	zassert_within(peer.snr, 7.0f, 0.001f);
	zassert_true(peer.has_hops_away);
	zassert_equal(peer.hops_away, 2U, "hops away is hop_start minus hop_limit");

	/* A packet whose channel could not be resolved falls back to channel 0. */
	packet.channel_index = MESHTASTIC_CHANNEL_INDEX_INVALID;
	meshtastic_dispatch_modules(&packet);
	zassert_equal(lookup(PEER_NODE_ID).channel, 0U);
}

ZTEST(nodedb, test_hops_away_is_unknown_without_a_usable_hop_start)
{
	struct meshtastic_packet packet = {
		.from = PEER_NODE_ID,
		.to = MESHTASTIC_NODE_BROADCAST,
		.portnum = MESHTASTIC_PORT_TEXT_MESSAGE,
		.payload = (const uint8_t *)"hi",
		.payload_len = 2U,
		.hop_limit = 3U,
	};

	/* Firmware older than 2.3.0 never set hop_start. */
	meshtastic_dispatch_modules(&packet);
	zassert_false(lookup(PEER_NODE_ID).has_hops_away);

	/* A hop_start below hop_limit cannot be trusted either. */
	packet.hop_start = 1U;
	meshtastic_dispatch_modules(&packet);
	zassert_false(lookup(PEER_NODE_ID).has_hops_away);
}

ZTEST(nodedb, test_our_own_and_anonymous_packets_are_ignored)
{
	struct meshtastic_packet packet = {
		.to = MESHTASTIC_NODE_BROADCAST,
		.portnum = MESHTASTIC_PORT_TEXT_MESSAGE,
		.payload = (const uint8_t *)"hi",
		.payload_len = 2U,
	};

	packet.from = TEST_NODE_ID;
	meshtastic_dispatch_modules(&packet);

	packet.from = 0U;
	meshtastic_dispatch_modules(&packet);

	meshtastic_dispatch_modules(NULL);

	zassert_equal(meshtastic_nodedb_count(), 1U, "only the local node should be stored");
}

ZTEST(nodedb, test_a_full_table_evicts_the_oldest_peer_and_keeps_the_local_node)
{
	struct meshtastic_nodedb_node node;

	/* One slot is taken by the local node, so the table holds MAX - 1 peers. */
	for (uint32_t i = 0U; i < CONFIG_MESHTASTIC_NODEDB_MAX_NODES - 1U; i++) {
		deliver_nodeinfo(PEER_NODE_ID + i, "Peer", "P");
	}
	zassert_equal(meshtastic_nodedb_count(), CONFIG_MESHTASTIC_NODEDB_MAX_NODES);

	deliver_nodeinfo(OTHER_NODE_ID, "Newcomer", "NEW");

	zassert_equal(meshtastic_nodedb_count(), CONFIG_MESHTASTIC_NODEDB_MAX_NODES);
	zassert_equal(meshtastic_nodedb_get(PEER_NODE_ID, &node), -ENOENT,
		      "the first peer heard should have been evicted");
	zassert_ok(meshtastic_nodedb_get(TEST_NODE_ID, &node));
	zassert_str_equal(lookup(OTHER_NODE_ID).long_name, "Newcomer");
}

ZTEST(nodedb, test_entries_can_be_walked_by_index)
{
	struct meshtastic_nodedb_node node;

	deliver_nodeinfo(PEER_NODE_ID, "Peer Node", "PEER");

	zassert_ok(meshtastic_nodedb_get_by_index(0U, &node));
	zassert_equal(node.num, TEST_NODE_ID);
	zassert_ok(meshtastic_nodedb_get_by_index(1U, &node));
	zassert_equal(node.num, PEER_NODE_ID);

	zassert_equal(meshtastic_nodedb_get_by_index(meshtastic_nodedb_count(), &node), -ENOENT);
	zassert_equal(meshtastic_nodedb_get_by_index(0U, NULL), -EINVAL);
	zassert_equal(meshtastic_nodedb_get(TEST_NODE_ID, NULL), -EINVAL);
	zassert_equal(meshtastic_nodedb_get(OTHER_NODE_ID, &node), -ENOENT);
}
