#include <string.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#include <zephyr/meshtastic/meshtastic.h>

#include <pb_encode.h>

#include "meshtastic_channels.h"
#include "meshtastic_core.h"
#include "meshtastic_packet.h"
#include "meshtastic_reliable.h"
#include "meshtastic_router.h"

#include "mock_lora.h"

#define TEST_NODE_ID  0x12345678U
#define PEER_NODE_ID  0x87654321U
#define OTHER_NODE_ID 0x13572468U

struct test_state {
	struct k_sem rx_sem;
	struct k_sem tx_sem;
	struct meshtastic_packet last_rx;
	struct meshtastic_packet last_event_packet;
	uint8_t last_rx_payload[MESHTASTIC_MAX_PAYLOAD_LEN];
	uint8_t last_event_payload[MESHTASTIC_MAX_PAYLOAD_LEN];
	size_t last_rx_payload_len;
	size_t last_event_payload_len;
	struct meshtastic_event last_event;
	uint32_t recv_count;
	uint32_t event_count;
	/*
	 * Delivery outcomes are tracked separately: the ROUTING packet that
	 * carries an acknowledgement also raises a PACKET_RECEIVED event, which
	 * would otherwise overwrite last_event before the test can read it.
	 */
	struct k_sem ack_sem;
	struct meshtastic_packet last_ack_packet;
	enum meshtastic_event_type last_ack_type;
	int last_ack_err;
	uint32_t ack_event_count;
};

static struct test_state state;

static void reset_callbacks_state(void)
{
	memset(&state.last_rx, 0, sizeof(state.last_rx));
	memset(&state.last_event_packet, 0, sizeof(state.last_event_packet));
	memset(state.last_rx_payload, 0, sizeof(state.last_rx_payload));
	memset(state.last_event_payload, 0, sizeof(state.last_event_payload));
	memset(&state.last_event, 0, sizeof(state.last_event));
	state.last_rx_payload_len = 0U;
	state.last_event_payload_len = 0U;
	state.recv_count = 0U;
	state.event_count = 0U;
	memset(&state.last_ack_packet, 0, sizeof(state.last_ack_packet));
	state.last_ack_type = MESHTASTIC_EVENT_PACKET_RECEIVED;
	state.last_ack_err = 0;
	state.ack_event_count = 0U;
	k_sem_reset(&state.rx_sem);
	k_sem_reset(&state.tx_sem);
	k_sem_reset(&state.ack_sem);
}

static void on_recv(uint32_t from, uint32_t to, uint32_t portnum, const uint8_t *payload,
		    size_t payload_len, int16_t rssi, int8_t snr)
{
	state.last_rx = (struct meshtastic_packet){
		.from = from,
		.to = to,
		.portnum = portnum,
		.payload = state.last_rx_payload,
		.payload_len = payload_len,
		.rssi = rssi,
		.snr = snr,
	};
	state.last_rx_payload_len = payload_len;
	if (payload_len > 0U) {
		memcpy(state.last_rx_payload, payload, payload_len);
	}
	state.recv_count++;
	k_sem_give(&state.rx_sem);
}

static void on_event(const struct meshtastic_event *event, void *user_data)
{
	ARG_UNUSED(user_data);

	state.last_event = *event;
	if (event->packet != NULL) {
		state.last_event_packet = *event->packet;
		state.last_event_packet.payload = state.last_event_payload;
		state.last_event_payload_len = event->packet->payload_len;
		if (event->packet->payload_len > 0U) {
			memcpy(state.last_event_payload, event->packet->payload,
			       event->packet->payload_len);
		}
	} else {
		memset(&state.last_event_packet, 0, sizeof(state.last_event_packet));
		memset(state.last_event_payload, 0, sizeof(state.last_event_payload));
		state.last_event_payload_len = 0U;
	}
	state.event_count++;
	if (event->type == MESHTASTIC_EVENT_TX_DONE || event->type == MESHTASTIC_EVENT_TX_FAILED) {
		k_sem_give(&state.tx_sem);
	}
	if (event->type == MESHTASTIC_EVENT_TX_ACKED || event->type == MESHTASTIC_EVENT_TX_NO_ACK) {
		state.last_ack_type = event->type;
		state.last_ack_err = event->err;
		if (event->packet != NULL) {
			state.last_ack_packet = *event->packet;
			state.last_ack_packet.payload = NULL;
		}
		state.ack_event_count++;
		k_sem_give(&state.ack_sem);
	}
}

static struct meshtastic_config cfg = {
	.node_id = TEST_NODE_ID,
	.psk = meshtastic_default_psk,
	.psk_len = sizeof(meshtastic_default_psk),
	.channel_name = MESHTASTIC_CHANNEL_LONGFAST,
	.frequency = MESHTASTIC_FREQ_EU,
};

static void *protocol_suite_setup(void)
{
	int ret;

	cfg.lora_dev = mock_lora_device();

	k_sem_init(&state.rx_sem, 0, 1);
	k_sem_init(&state.tx_sem, 0, 1);
	k_sem_init(&state.ack_sem, 0, 1);

	zassert_true(device_is_ready(mock_lora_device()), "mock lora device not ready");

	ret = meshtastic_init(&cfg);
	zassert_ok(ret, "meshtastic_init failed: %d", ret);

	meshtastic_set_recv_cb(on_recv);
	meshtastic_set_event_cb(on_event, NULL);

	mock_lora_reset();
	reset_callbacks_state();

	return NULL;
}

static void protocol_before(void *fixture)
{
	ARG_UNUSED(fixture);
	zassert_ok(meshtastic_channels_init_from_config(&cfg), "channel reset failed");
	meshtastic_set_device_role(meshtastic_Config_DeviceConfig_Role_CLIENT);
	meshtastic_set_rebroadcast_mode(meshtastic_Config_DeviceConfig_RebroadcastMode_ALL);
	memset(mt.dup_cache, 0, sizeof(mt.dup_cache));
	mt.dup_head = 0U;
	/* Leftover entries would retransmit into the next test's send counts. */
	meshtastic_reliable_reset();
	mock_lora_reset();
	reset_callbacks_state();
}

static void assert_payload(const uint8_t *actual, size_t actual_len, const void *expected,
			   size_t expected_len)
{
	zassert_equal(actual_len, expected_len, "unexpected payload len");
	if (expected_len > 0U) {
		zassert_not_null(actual, "expected payload pointer");
		zassert_mem_equal(actual, expected, expected_len, "unexpected payload");
	} else {
		zassert_is_null(actual, "empty payload should use NULL pointer");
	}
}

static void assert_rx_payload(const void *expected, size_t expected_len)
{
	zassert_equal(state.last_rx_payload_len, expected_len, "unexpected RX payload len");
	if (expected_len > 0U) {
		zassert_mem_equal(state.last_rx_payload, expected, expected_len,
				  "unexpected RX payload");
	}
}

static void assert_event_payload(const void *expected, size_t expected_len)
{
	zassert_equal(state.last_event_payload_len, expected_len, "unexpected event payload len");
	if (expected_len > 0U) {
		zassert_mem_equal(state.last_event_payload, expected, expected_len,
				  "unexpected event payload");
	}
}

static void assert_mock_send_count(uint32_t expected)
{
	zassert_equal(mock_lora_send_count(), expected, "unexpected lora_send count");
}

static void build_wire_packet(uint32_t from, uint32_t to, uint32_t id, uint8_t hop_limit,
			      uint32_t portnum, const uint8_t *payload, size_t payload_len,
			      uint8_t *wire, uint32_t *wire_len)
{
	struct meshtastic_packet packet = {
		.from = from,
		.to = to,
		.id = id,
		.portnum = portnum,
		.payload = payload,
		.payload_len = payload_len,
		.hop_limit = hop_limit,
		.hop_start = hop_limit,
		.channel_index = meshtastic_channels_primary_index(),
	};
	int ret;

	ret = meshtastic_build_wire_packet(&packet, wire, wire_len);
	zassert_ok(ret, "meshtastic_build_wire_packet failed: %d", ret);
}

static void build_peer_wire_packet(uint32_t to, uint32_t id, uint8_t hop_limit, const char *text,
				   uint8_t *wire, uint32_t *wire_len)
{
	build_wire_packet(PEER_NODE_ID, to, id, hop_limit, MESHTASTIC_PORT_TEXT_MESSAGE,
			  (const uint8_t *)text, strlen(text), wire, wire_len);
}

static void decode_last_tx(struct meshtastic_packet *decoded, uint8_t *payload, size_t payload_len)
{
	uint8_t wire[MESHTASTIC_PKT_MAX];
	uint32_t wire_len;
	int ret;

	assert_mock_send_count(1U);
	wire_len = mock_lora_last_tx(wire, sizeof(wire));
	zassert_true(wire_len > MESHTASTIC_HDR_LEN, "expected wire payload");

	ret = meshtastic_decode_wire_packet(wire, wire_len, 0, 0, decoded, payload, payload_len);
	zassert_ok(ret, "meshtastic_decode_wire_packet failed: %d", ret);
}

static void copy_last_tx_header(struct meshtastic_wire_header *hdr)
{
	uint8_t wire[MESHTASTIC_PKT_MAX];

	zassert_true(mock_lora_last_tx(wire, sizeof(wire)) >= MESHTASTIC_HDR_LEN,
		     "expected tx header");
	memcpy(hdr, wire, sizeof(*hdr));
}

static void assert_wire_header(const struct meshtastic_wire_header *hdr, uint32_t from, uint32_t to,
			       uint32_t id, uint8_t hop_limit, uint8_t hop_start, bool want_ack,
			       bool via_mqtt, uint8_t next_hop, uint8_t relay_node)
{
	uint8_t flags = hdr->flags;

	zassert_equal(sys_le32_to_cpu(hdr->src), from, "unexpected wire source");
	zassert_equal(sys_le32_to_cpu(hdr->dest), to, "unexpected wire destination");
	zassert_equal(sys_le32_to_cpu(hdr->id), id, "unexpected wire id");
	zassert_equal(flags & MESHTASTIC_FLAGS_HOP_LIMIT_MASK, hop_limit, "unexpected hop limit");
	zassert_equal((flags & MESHTASTIC_FLAGS_HOP_START_MASK) >> MESHTASTIC_FLAGS_HOP_START_SHIFT,
		      hop_start, "unexpected hop start");
	zassert_equal((flags & MESHTASTIC_FLAGS_WANT_ACK) != 0U, want_ack,
		      "unexpected want_ack flag");
	zassert_equal((flags & MESHTASTIC_FLAGS_VIA_MQTT) != 0U, via_mqtt,
		      "unexpected via_mqtt flag");
	zassert_equal(hdr->channel,
		      meshtastic_channels_get_hash(meshtastic_channels_primary_index()),
		      "unexpected channel hash");
	zassert_equal(hdr->next_hop, next_hop, "unexpected next hop");
	zassert_equal(hdr->relay_node, relay_node, "unexpected relay node");
}

/* Builds a peer packet that asks this node to acknowledge it. */
static void build_peer_want_ack_wire(uint32_t id, const char *text, uint8_t *wire,
				     uint32_t *wire_len)
{
	struct meshtastic_packet packet = {
		.from = PEER_NODE_ID,
		.to = TEST_NODE_ID,
		.id = id,
		.portnum = MESHTASTIC_PORT_TEXT_MESSAGE,
		.payload = (const uint8_t *)text,
		.payload_len = strlen(text),
		.hop_limit = 3U,
		.hop_start = 3U,
		.channel_index = meshtastic_channels_primary_index(),
		.want_ack = true,
	};
	int ret;

	ret = meshtastic_build_wire_packet(&packet, wire, wire_len);
	zassert_ok(ret, "meshtastic_build_wire_packet failed: %d", ret);
}

/* Builds the ROUTING reply a peer sends back for @p request_id. */
static void build_routing_reply_wire(uint32_t id, uint32_t request_id,
				     meshtastic_Routing_Error error, uint8_t *wire,
				     uint32_t *wire_len)
{
	meshtastic_Routing routing = meshtastic_Routing_init_zero;
	uint8_t payload[MESHTASTIC_MAX_PAYLOAD_LEN];
	pb_ostream_t stream;
	struct meshtastic_packet packet = {
		.from = PEER_NODE_ID,
		.to = TEST_NODE_ID,
		.id = id,
		.portnum = MESHTASTIC_PORT_ROUTING,
		.request_id = request_id,
		.hop_limit = 3U,
		.hop_start = 3U,
		.channel_index = meshtastic_channels_primary_index(),
	};
	int ret;

	routing.which_variant = meshtastic_Routing_error_reason_tag;
	routing.error_reason = error;

	stream = pb_ostream_from_buffer(payload, sizeof(payload));
	zassert_true(pb_encode(&stream, meshtastic_Routing_fields, &routing),
		     "routing encode failed");

	packet.payload = payload;
	packet.payload_len = stream.bytes_written;

	ret = meshtastic_build_wire_packet(&packet, wire, wire_len);
	zassert_ok(ret, "meshtastic_build_wire_packet failed: %d", ret);
}

/* Sends a unicast packet that asks for an acknowledgement; returns its packet ID. */
static uint32_t send_want_ack_unicast(const char *text)
{
	struct meshtastic_packet packet = {
		.to = PEER_NODE_ID,
		.portnum = MESHTASTIC_PORT_TEXT_MESSAGE,
		.payload = (const uint8_t *)text,
		.payload_len = strlen(text),
		.want_ack = true,
	};
	struct meshtastic_wire_header hdr;
	int ret;

	ret = meshtastic_send_packet(&packet, K_FOREVER);
	zassert_ok(ret, "meshtastic_send_packet failed: %d", ret);
	zassert_ok(k_sem_take(&state.tx_sem, K_SECONDS(1)), "timed out waiting for tx event");

	copy_last_tx_header(&hdr);
	zassert_true((hdr.flags & MESHTASTIC_FLAGS_WANT_ACK) != 0U,
		     "want_ack flag missing from wire header");

	return sys_le32_to_cpu(hdr.id);
}

ZTEST_SUITE(protocol_stack, NULL, protocol_suite_setup, protocol_before, NULL, NULL);

/* Verifies local text sends produce one LoRa frame, a TX_DONE event, and a decodable payload. */
ZTEST(protocol_stack, test_send_text_uses_mock_lora_and_round_trips)
{
	struct meshtastic_packet decoded;
	uint8_t payload[MESHTASTIC_MAX_PAYLOAD_LEN];
	struct meshtastic_status before;
	struct meshtastic_status after;
	int ret;

	ret = meshtastic_get_status(&before);
	zassert_ok(ret, "status read failed: %d", ret);

	ret = meshtastic_send_text(MESHTASTIC_NODE_BROADCAST, "ping");
	zassert_ok(ret, "meshtastic_send_text failed: %d", ret);
	zassert_ok(k_sem_take(&state.tx_sem, K_SECONDS(1)), "timed out waiting for tx event");

	decode_last_tx(&decoded, payload, sizeof(payload));
	zassert_equal(decoded.from, TEST_NODE_ID, "unexpected source");
	zassert_equal(decoded.to, MESHTASTIC_NODE_BROADCAST, "unexpected destination");
	zassert_equal(decoded.portnum, MESHTASTIC_PORT_TEXT_MESSAGE, "unexpected port");
	assert_payload(decoded.payload, decoded.payload_len, "ping", 4U);
	zassert_equal(state.last_event.type, MESHTASTIC_EVENT_TX_DONE, "unexpected event");

	ret = meshtastic_get_status(&after);
	zassert_ok(ret, "status read failed: %d", ret);
	zassert_equal(after.tx_packets, before.tx_packets + 1U, "tx counter not incremented");
}

/* Verifies invalid send API arguments fail before reaching the LoRa driver. */
ZTEST(protocol_stack, test_invalid_send_inputs_do_not_transmit)
{
	static uint8_t too_large_payload[MESHTASTIC_MAX_PAYLOAD_LEN + 1U];
	char too_long_text[MESHTASTIC_MAX_TEXT_LEN + 2U];
	int ret;

	memset(too_long_text, 'x', sizeof(too_long_text) - 1U);
	too_long_text[sizeof(too_long_text) - 1U] = '\0';

	ret = meshtastic_send_text(MESHTASTIC_NODE_BROADCAST, NULL);
	zassert_equal(ret, -EINVAL, "NULL text should be rejected");
	ret = meshtastic_send_text(MESHTASTIC_NODE_BROADCAST, "");
	zassert_equal(ret, -EINVAL, "empty text should be rejected");
	ret = meshtastic_send_text(MESHTASTIC_NODE_BROADCAST, too_long_text);
	zassert_equal(ret, -EINVAL, "oversized text should be rejected");
	ret = meshtastic_send_data(0U, MESHTASTIC_PORT_PRIVATE, NULL, 0U, K_FOREVER);
	zassert_equal(ret, -EINVAL, "zero destination should be rejected");
	ret = meshtastic_send_data(MESHTASTIC_NODE_BROADCAST, MESHTASTIC_PORT_PRIVATE, NULL, 1U,
				   K_FOREVER);
	zassert_equal(ret, -EINVAL, "missing payload should be rejected");
	ret = meshtastic_send_data(MESHTASTIC_NODE_BROADCAST, MESHTASTIC_PORT_PRIVATE,
				   too_large_payload, sizeof(too_large_payload), K_FOREVER);
	zassert_equal(ret, -EINVAL, "oversized payload should be rejected");

	assert_mock_send_count(0U);
	zassert_equal(state.event_count, 0U, "invalid sends should not emit events");
}

/* Verifies non-text data may carry an empty payload and decodes as a zero-length Data payload. */
ZTEST(protocol_stack, test_zero_length_data_payload_round_trips)
{
	struct meshtastic_packet decoded;
	uint8_t payload[MESHTASTIC_MAX_PAYLOAD_LEN];
	int ret;

	ret = meshtastic_send_data(MESHTASTIC_NODE_BROADCAST, MESHTASTIC_PORT_PRIVATE, NULL, 0U,
				   K_FOREVER);
	zassert_ok(ret, "empty data send failed: %d", ret);
	zassert_ok(k_sem_take(&state.tx_sem, K_SECONDS(1)), "timed out waiting for tx event");

	decode_last_tx(&decoded, payload, sizeof(payload));
	zassert_equal(decoded.portnum, MESHTASTIC_PORT_PRIVATE, "unexpected port");
	assert_payload(decoded.payload, decoded.payload_len, NULL, 0U);
}

/* Verifies explicit packet metadata is preserved in both the wire header and decoded payload. */
ZTEST(protocol_stack, test_send_packet_preserves_explicit_metadata)
{
	const uint8_t body[] = {0x10, 0x20, 0x30};
	struct meshtastic_packet packet = {
		.from = TEST_NODE_ID,
		.to = PEER_NODE_ID,
		.id = 0x44445555U,
		.portnum = MESHTASTIC_PORT_PRIVATE,
		.payload = body,
		.payload_len = sizeof(body),
		.data_dest = OTHER_NODE_ID,
		.data_source = TEST_NODE_ID,
		.request_id = 0x1010U,
		.reply_id = 0x2020U,
		.hop_limit = 5U,
		.hop_start = 6U,
		.channel_index = meshtastic_channels_primary_index(),
		.next_hop = 0x21U,
		.relay_node = 0x43U,
		.want_ack = true,
		.via_mqtt = true,
		.want_response = true,
	};
	struct meshtastic_wire_header hdr;
	struct meshtastic_packet decoded;
	uint8_t payload[MESHTASTIC_MAX_PAYLOAD_LEN];
	int ret;

	ret = meshtastic_send_packet(&packet, K_FOREVER);
	zassert_ok(ret, "metadata packet send failed: %d", ret);
	zassert_ok(k_sem_take(&state.tx_sem, K_SECONDS(1)), "timed out waiting for tx event");

	copy_last_tx_header(&hdr);
	assert_wire_header(&hdr, TEST_NODE_ID, PEER_NODE_ID, packet.id, packet.hop_limit,
			   packet.hop_start, true, true, packet.next_hop, packet.relay_node);

	decode_last_tx(&decoded, payload, sizeof(payload));
	zassert_equal(decoded.data_dest, packet.data_dest, "unexpected Data.dest");
	zassert_equal(decoded.data_source, packet.data_source, "unexpected Data.source");
	zassert_equal(decoded.request_id, packet.request_id, "unexpected request id");
	zassert_equal(decoded.reply_id, packet.reply_id, "unexpected reply id");
	zassert_true(decoded.want_response, "want_response not preserved");
	assert_payload(decoded.payload, decoded.payload_len, body, sizeof(body));
}

/* Verifies radio send errors emit TX_FAILED and update failure status instead of TX_DONE. */
ZTEST(protocol_stack, test_radio_send_failure_emits_failed_event)
{
	struct meshtastic_status before;
	struct meshtastic_status after;
	int ret;

	ret = meshtastic_get_status(&before);
	zassert_ok(ret, "status read failed: %d", ret);

	mock_lora_set_send_result(-EIO);
	ret = meshtastic_send_text(MESHTASTIC_NODE_BROADCAST, "fail");
	zassert_equal(ret, -EIO, "send should return mock radio failure");
	zassert_ok(k_sem_take(&state.tx_sem, K_SECONDS(1)), "timed out waiting for failure event");

	assert_mock_send_count(1U);
	zassert_equal(state.last_event.type, MESHTASTIC_EVENT_TX_FAILED,
		      "unexpected failure event");
	zassert_equal(state.last_event.err, -EIO, "unexpected failure errno");

	ret = meshtastic_get_status(&after);
	zassert_ok(ret, "status read failed: %d", ret);
	zassert_equal(after.tx_failures, before.tx_failures + 1U,
		      "tx failure counter not incremented");
	zassert_equal(after.tx_packets, before.tx_packets, "failed TX should not count as sent");
}

/* Verifies decoded MeshPacket conversion preserves application payload and packet metadata. */
ZTEST(protocol_stack, test_mesh_packet_conversion_preserves_decoded_metadata)
{
	const uint8_t body[] = "meshpb";
	struct meshtastic_packet packet = {
		.from = PEER_NODE_ID,
		.to = TEST_NODE_ID,
		.id = 0x5101U,
		.portnum = MESHTASTIC_PORT_PRIVATE,
		.payload = body,
		.payload_len = sizeof(body) - 1U,
		.data_dest = TEST_NODE_ID,
		.data_source = PEER_NODE_ID,
		.request_id = 0x55U,
		.reply_id = 0x66U,
		.hop_limit = 4U,
		.hop_start = 5U,
		.channel_index = meshtastic_channels_primary_index(),
		.next_hop = 0x78U,
		.relay_node = 0x87U,
		.want_ack = true,
		.via_mqtt = true,
		.want_response = true,
	};
	meshtastic_MeshPacket mesh;
	struct meshtastic_packet out;
	uint8_t payload[MESHTASTIC_MAX_PAYLOAD_LEN];
	int ret;

	ret = meshtastic_packet_to_mesh_pb(&packet, &mesh);
	zassert_ok(ret, "packet_to_mesh_pb failed: %d", ret);
	ret = meshtastic_mesh_pb_to_packet(&mesh, &out, payload, sizeof(payload));
	zassert_ok(ret, "mesh_pb_to_packet failed: %d", ret);

	zassert_equal(out.from, packet.from, "unexpected source");
	zassert_equal(out.to, packet.to, "unexpected destination");
	zassert_equal(out.id, packet.id, "unexpected packet id");
	zassert_equal(out.portnum, packet.portnum, "unexpected port");
	zassert_equal(out.channel_index, packet.channel_index, "unexpected channel index");
	zassert_equal(out.channel, meshtastic_channels_primary_hash(), "unexpected channel hash");
	zassert_equal(out.next_hop, packet.next_hop, "unexpected next hop");
	zassert_equal(out.relay_node, packet.relay_node, "unexpected relay node");
	zassert_true(out.want_ack, "want_ack not preserved");
	zassert_true(out.via_mqtt, "via_mqtt not preserved");
	zassert_true(out.want_response, "want_response not preserved");
	assert_payload(out.payload, out.payload_len, body, sizeof(body) - 1U);
}

/* Verifies MeshPacket copies preserve encrypted union data and the active payload variant. */
ZTEST(protocol_stack, test_mesh_packet_copy_preserves_encrypted_payload)
{
	const uint8_t encrypted[] = {0xaa, 0xbb, 0xcc, 0xdd};
	meshtastic_MeshPacket src = meshtastic_MeshPacket_init_zero;
	meshtastic_MeshPacket dst = meshtastic_MeshPacket_init_zero;

	src.from = PEER_NODE_ID;
	src.to = TEST_NODE_ID;
	src.id = 0x5202U;
	src.which_payload_variant = meshtastic_MeshPacket_encrypted_tag;
	src.encrypted.size = sizeof(encrypted);
	memcpy(src.encrypted.bytes, encrypted, sizeof(encrypted));

	meshtastic_mesh_packet_copy(&dst, &src);

	zassert_equal(dst.which_payload_variant, meshtastic_MeshPacket_encrypted_tag,
		      "encrypted variant not preserved");
	zassert_equal(dst.encrypted.size, sizeof(encrypted), "encrypted length not preserved");
	zassert_mem_equal(dst.encrypted.bytes, encrypted, sizeof(encrypted),
			  "encrypted bytes not preserved");
}

/* Verifies encrypted MeshPacket payloads decode with the active channel and bad payloads fail. */
ZTEST(protocol_stack, test_mesh_pb_try_decode_accepts_valid_encrypted_payload_only)
{
	const uint8_t body[] = "secret";
	uint8_t wire[MESHTASTIC_PKT_MAX];
	uint32_t wire_len;
	meshtastic_MeshPacket mesh = meshtastic_MeshPacket_init_zero;
	meshtastic_MeshPacket empty = meshtastic_MeshPacket_init_zero;
	meshtastic_MeshPacket bad = meshtastic_MeshPacket_init_zero;
	const struct meshtastic_wire_header *hdr;
	int ret;

	build_wire_packet(PEER_NODE_ID, TEST_NODE_ID, 0x5303U, 3U, MESHTASTIC_PORT_PRIVATE, body,
			  sizeof(body) - 1U, wire, &wire_len);

	hdr = (const struct meshtastic_wire_header *)wire;
	mesh.from = PEER_NODE_ID;
	mesh.to = TEST_NODE_ID;
	mesh.id = 0x5303U;
	mesh.channel = hdr->channel;
	mesh.which_payload_variant = meshtastic_MeshPacket_encrypted_tag;
	mesh.encrypted.size = wire_len - MESHTASTIC_HDR_LEN;
	memcpy(mesh.encrypted.bytes, wire + MESHTASTIC_HDR_LEN, mesh.encrypted.size);

	ret = meshtastic_mesh_pb_try_decode(&mesh);
	zassert_ok(ret, "valid encrypted MeshPacket did not decode: %d", ret);
	zassert_equal(mesh.which_payload_variant, meshtastic_MeshPacket_decoded_tag,
		      "payload variant not switched to decoded");
	zassert_equal((uint32_t)mesh.decoded.portnum, MESHTASTIC_PORT_PRIVATE,
		      "unexpected decoded port");
	zassert_equal(mesh.decoded.payload.size, sizeof(body) - 1U, "unexpected decoded length");
	zassert_mem_equal(mesh.decoded.payload.bytes, body, sizeof(body) - 1U,
			  "unexpected decoded payload");

	empty.which_payload_variant = meshtastic_MeshPacket_encrypted_tag;
	ret = meshtastic_mesh_pb_try_decode(&empty);
	zassert_true(ret < 0, "empty encrypted payload should be rejected");

	bad.from = PEER_NODE_ID;
	bad.id = 0x5304U;
	bad.channel = hdr->channel;
	bad.which_payload_variant = meshtastic_MeshPacket_encrypted_tag;
	bad.encrypted.size = 3U;
	memset(bad.encrypted.bytes, 0xa5, bad.encrypted.size);
	ret = meshtastic_mesh_pb_try_decode(&bad);
	zassert_true(ret < 0, "invalid encrypted payload should be rejected");
}

/* Verifies a valid LoRa frame addressed to this node is delivered with RSSI/SNR metadata. */
ZTEST(protocol_stack, test_mock_radio_receive_delivers_packet)
{
	uint8_t wire[MESHTASTIC_PKT_MAX];
	uint32_t wire_len;
	struct meshtastic_status before;
	struct meshtastic_status after;
	int ret;

	ret = meshtastic_get_status(&before);
	zassert_ok(ret, "status read failed: %d", ret);

	build_peer_wire_packet(TEST_NODE_ID, 0x1001U, 3U, "pong", wire, &wire_len);
	mock_lora_inject_rx(wire, wire_len, -42, 7);

	zassert_ok(k_sem_take(&state.rx_sem, K_SECONDS(1)), "timed out waiting for rx callback");
	zassert_equal(state.recv_count, 1U, "expected one delivery");
	zassert_equal(state.last_rx.from, PEER_NODE_ID, "unexpected source");
	zassert_equal(state.last_rx.to, TEST_NODE_ID, "unexpected destination");
	zassert_equal(state.last_rx.portnum, MESHTASTIC_PORT_TEXT_MESSAGE, "unexpected port");
	assert_rx_payload("pong", 4U);
	zassert_equal(state.last_rx.rssi, -42, "unexpected rssi");
	zassert_equal(state.last_rx.snr, 7, "unexpected snr");
	zassert_equal(state.last_event.type, MESHTASTIC_EVENT_PACKET_RECEIVED, "unexpected event");

	ret = meshtastic_get_status(&after);
	zassert_ok(ret, "status read failed: %d", ret);
	zassert_equal(after.rx_packets, before.rx_packets + 1U, "rx counter not incremented");
}

/* Verifies too-short LoRa frames are ignored before duplicate, decode, or delivery handling. */
ZTEST(protocol_stack, test_too_short_rx_frame_is_ignored)
{
	uint8_t wire[MESHTASTIC_HDR_LEN - 1U] = {0};
	struct meshtastic_status before;
	struct meshtastic_status after;
	int ret;

	ret = meshtastic_get_status(&before);
	zassert_ok(ret, "status read failed: %d", ret);

	mock_lora_inject_rx(wire, sizeof(wire), -10, 1);
	k_sleep(K_MSEC(100));

	zassert_equal(state.recv_count, 0U, "short frame should not be delivered");
	ret = meshtastic_get_status(&after);
	zassert_ok(ret, "status read failed: %d", ret);
	zassert_equal(after.rx_packets, before.rx_packets, "short frame should not count as RX");
	zassert_equal(after.decode_failures, before.decode_failures,
		      "short frame should not count as decode failure");
}

/* Verifies undecodable channel hashes count as decode failures without local delivery. */
ZTEST(protocol_stack, test_unknown_channel_hash_counts_decode_failure_without_delivery)
{
	uint8_t wire[MESHTASTIC_PKT_MAX];
	uint32_t wire_len;
	struct meshtastic_wire_header *hdr;
	struct meshtastic_status before;
	struct meshtastic_status after;
	int ret;

	ret = meshtastic_get_status(&before);
	zassert_ok(ret, "status read failed: %d", ret);

	build_peer_wire_packet(TEST_NODE_ID, 0x5404U, 3U, "badch", wire, &wire_len);
	hdr = (struct meshtastic_wire_header *)wire;
	hdr->channel ^= 0xffU;
	mock_lora_inject_rx(wire, wire_len, -20, 4);
	k_sleep(K_MSEC(100));

	zassert_equal(state.recv_count, 0U, "undecodable packet should not be delivered");
	ret = meshtastic_get_status(&after);
	zassert_ok(ret, "status read failed: %d", ret);
	zassert_equal(after.rx_packets, before.rx_packets + 1U, "rx counter not incremented");
	zassert_equal(after.decode_failures, before.decode_failures + 1U,
		      "decode failure counter not incremented");
}

/* Verifies broadcast LoRa packets are delivered locally just like direct unicasts. */
ZTEST(protocol_stack, test_broadcast_rx_packet_is_delivered_locally)
{
	uint8_t wire[MESHTASTIC_PKT_MAX];
	uint32_t wire_len;

	build_peer_wire_packet(MESHTASTIC_NODE_BROADCAST, 0x5505U, 0U, "all", wire, &wire_len);
	mock_lora_inject_rx(wire, wire_len, -33, 6);

	zassert_ok(k_sem_take(&state.rx_sem, K_SECONDS(1)), "timed out waiting for rx callback");
	zassert_equal(state.recv_count, 1U, "expected one delivery");
	zassert_equal(state.last_rx.to, MESHTASTIC_NODE_BROADCAST, "unexpected destination");
	assert_rx_payload("all", 3U);
}

/* Verifies duplicate local packets are delivered once and counted as duplicates thereafter. */
ZTEST(protocol_stack, test_duplicate_packets_are_suppressed)
{
	uint8_t wire[MESHTASTIC_PKT_MAX];
	uint32_t wire_len;
	struct meshtastic_status before;
	struct meshtastic_status after;
	int ret;

	ret = meshtastic_get_status(&before);
	zassert_ok(ret, "status read failed: %d", ret);

	build_peer_wire_packet(TEST_NODE_ID, 0x2002U, 3U, "dupe", wire, &wire_len);
	mock_lora_inject_rx(wire, wire_len, -30, 5);
	zassert_ok(k_sem_take(&state.rx_sem, K_SECONDS(1)), "timed out waiting for first rx");

	mock_lora_inject_rx(wire, wire_len, -30, 5);
	zassert_equal(-EAGAIN, k_sem_take(&state.rx_sem, K_MSEC(100)),
		      "duplicate unexpectedly delivered");

	ret = meshtastic_get_status(&after);
	zassert_ok(ret, "status read failed: %d", ret);
	zassert_equal(after.duplicate_packets, before.duplicate_packets + 1U,
		      "duplicate counter not incremented");
	zassert_equal(state.recv_count, 1U, "expected single delivery");
}

/* Verifies duplicate foreign packets do not trigger additional relay transmissions. */
ZTEST(protocol_stack, test_duplicate_foreign_packets_do_not_relay_again)
{
	uint8_t wire[MESHTASTIC_PKT_MAX];
	uint32_t wire_len;
	struct meshtastic_status before;
	struct meshtastic_status after;
	int ret;

	ret = meshtastic_get_status(&before);
	zassert_ok(ret, "status read failed: %d", ret);

	build_peer_wire_packet(OTHER_NODE_ID, 0x5606U, 3U, "relay-once", wire, &wire_len);
	mock_lora_inject_rx(wire, wire_len, -35, 4);
	k_sleep(K_MSEC(100));
	assert_mock_send_count(1U);

	mock_lora_reset();
	mock_lora_inject_rx(wire, wire_len, -35, 4);
	k_sleep(K_MSEC(100));

	assert_mock_send_count(0U);
	zassert_equal(state.recv_count, 0U, "foreign packets should not be locally delivered");
	ret = meshtastic_get_status(&after);
	zassert_ok(ret, "status read failed: %d", ret);
	zassert_equal(after.duplicate_packets, before.duplicate_packets + 1U,
		      "duplicate counter not incremented");
}

/* Verifies foreign unicasts with hop limit remaining are relayed with the hop limit decremented. */
ZTEST(protocol_stack, test_foreign_unicast_is_relayed_with_decremented_hop_limit)
{
	struct meshtastic_wire_header hdr;
	uint8_t wire[MESHTASTIC_PKT_MAX];
	uint32_t wire_len;
	struct meshtastic_status before;
	struct meshtastic_status after;
	int ret;

	ret = meshtastic_get_status(&before);
	zassert_ok(ret, "status read failed: %d", ret);

	build_peer_wire_packet(OTHER_NODE_ID, 0x3003U, 3U, "relay", wire, &wire_len);
	mock_lora_inject_rx(wire, wire_len, -55, 2);
	k_sleep(K_MSEC(100));

	assert_mock_send_count(1U);
	copy_last_tx_header(&hdr);
	zassert_equal(hdr.flags & MESHTASTIC_FLAGS_HOP_LIMIT_MASK, 2U,
		      "relay hop limit not decremented");
	zassert_equal(state.recv_count, 0U, "foreign unicast should not be delivered locally");

	ret = meshtastic_get_status(&after);
	zassert_ok(ret, "status read failed: %d", ret);
	zassert_equal(after.relayed_packets, before.relayed_packets + 1U,
		      "relay counter not incremented");
}

/* Verifies foreign unicasts with no hop limit remaining are not relayed. */
ZTEST(protocol_stack, test_foreign_unicast_with_zero_hop_limit_is_not_relayed)
{
	uint8_t wire[MESHTASTIC_PKT_MAX];
	uint32_t wire_len;

	build_peer_wire_packet(OTHER_NODE_ID, 0x5707U, 0U, "terminal", wire, &wire_len);
	mock_lora_inject_rx(wire, wire_len, -48, 2);
	k_sleep(K_MSEC(100));

	assert_mock_send_count(0U);
	zassert_equal(state.recv_count, 0U, "foreign unicast should not be delivered locally");
}

/* Verifies rebroadcast policy NONE and CLIENT_MUTE role both suppress foreign relays. */
ZTEST(protocol_stack, test_rebroadcast_policy_can_suppress_foreign_relay)
{
	uint8_t wire[MESHTASTIC_PKT_MAX];
	uint32_t wire_len;
	struct meshtastic_status before;
	struct meshtastic_status after;
	int ret;

	ret = meshtastic_get_status(&before);
	zassert_ok(ret, "status read failed: %d", ret);

	meshtastic_set_rebroadcast_mode(meshtastic_Config_DeviceConfig_RebroadcastMode_NONE);
	build_peer_wire_packet(OTHER_NODE_ID, 0x5808U, 3U, "none", wire, &wire_len);
	mock_lora_inject_rx(wire, wire_len, -45, 2);
	k_sleep(K_MSEC(100));
	assert_mock_send_count(0U);

	mock_lora_reset();
	meshtastic_set_rebroadcast_mode(meshtastic_Config_DeviceConfig_RebroadcastMode_ALL);
	meshtastic_set_device_role(meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE);
	build_peer_wire_packet(OTHER_NODE_ID, 0x5809U, 3U, "mute", wire, &wire_len);
	mock_lora_inject_rx(wire, wire_len, -45, 2);
	k_sleep(K_MSEC(100));
	assert_mock_send_count(0U);

	ret = meshtastic_get_status(&after);
	zassert_ok(ret, "status read failed: %d", ret);
	zassert_equal(after.relayed_packets, before.relayed_packets,
		      "relay counter should not change when policy suppresses relay");
}

/* Verifies decoded MQTT downlink broadcasts are delivered locally and marked via MQTT. */
ZTEST(protocol_stack, test_downlink_decoded_broadcast_delivers_locally_via_mqtt)
{
	const uint8_t body[] = "mqtt";
	meshtastic_MeshPacket mesh = meshtastic_MeshPacket_init_zero;
	int ret;

	mesh.from = PEER_NODE_ID;
	mesh.to = MESHTASTIC_NODE_BROADCAST;
	mesh.id = 0x5901U;
	mesh.hop_limit = 2U;
	mesh.hop_start = 2U;
	mesh.channel = meshtastic_channels_primary_index();
	mesh.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
	mesh.decoded.portnum = MESHTASTIC_PORT_TEXT_MESSAGE;
	mesh.decoded.payload.size = sizeof(body) - 1U;
	memcpy(mesh.decoded.payload.bytes, body, sizeof(body) - 1U);

	ret = meshtastic_inject_downlink_mesh_packet(&mesh);
	zassert_ok(ret, "downlink inject failed: %d", ret);

	zassert_equal(state.recv_count, 1U, "broadcast downlink should be delivered");
	zassert_equal(state.last_event.type, MESHTASTIC_EVENT_PACKET_RECEIVED,
		      "unexpected downlink event");
	zassert_true(state.last_event_packet.via_mqtt, "downlink should be marked via MQTT");
	assert_event_payload(body, sizeof(body) - 1U);
	assert_mock_send_count(1U);
}

/* Verifies foreign decoded MQTT downlinks relay onto LoRa without local packet delivery. */
ZTEST(protocol_stack, test_downlink_foreign_packet_relays_without_local_delivery)
{
	const uint8_t body[] = "fwd";
	meshtastic_MeshPacket mesh = meshtastic_MeshPacket_init_zero;
	int ret;

	mesh.from = PEER_NODE_ID;
	mesh.to = OTHER_NODE_ID;
	mesh.id = 0x5902U;
	mesh.hop_limit = 2U;
	mesh.hop_start = 2U;
	mesh.channel = meshtastic_channels_primary_index();
	mesh.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
	mesh.decoded.portnum = MESHTASTIC_PORT_TEXT_MESSAGE;
	mesh.decoded.payload.size = sizeof(body) - 1U;
	memcpy(mesh.decoded.payload.bytes, body, sizeof(body) - 1U);

	ret = meshtastic_inject_downlink_mesh_packet(&mesh);
	zassert_ok(ret, "foreign downlink inject failed: %d", ret);

	assert_mock_send_count(1U);
	zassert_equal(state.recv_count, 0U, "foreign downlink should not be delivered locally");
	zassert_equal(state.last_event.type, MESHTASTIC_EVENT_TX_DONE,
		      "relay should emit TX_DONE only");
}

/* Verifies duplicate MQTT downlinks are rejected before relay or local delivery. */
ZTEST(protocol_stack, test_duplicate_downlink_returns_ealready)
{
	const uint8_t body[] = "dupe";
	meshtastic_MeshPacket mesh = meshtastic_MeshPacket_init_zero;
	int ret;

	mesh.from = PEER_NODE_ID;
	mesh.to = MESHTASTIC_NODE_BROADCAST;
	mesh.id = 0x5903U;
	mesh.hop_limit = 2U;
	mesh.hop_start = 2U;
	mesh.channel = meshtastic_channels_primary_index();
	mesh.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
	mesh.decoded.portnum = MESHTASTIC_PORT_TEXT_MESSAGE;
	mesh.decoded.payload.size = sizeof(body) - 1U;
	memcpy(mesh.decoded.payload.bytes, body, sizeof(body) - 1U);

	ret = meshtastic_inject_downlink_mesh_packet(&mesh);
	zassert_ok(ret, "first downlink inject failed: %d", ret);

	mock_lora_reset();
	reset_callbacks_state();
	ret = meshtastic_inject_downlink_mesh_packet(&mesh);
	zassert_equal(ret, -EALREADY, "duplicate downlink should return -EALREADY");
	assert_mock_send_count(0U);
	zassert_equal(state.recv_count, 0U, "duplicate downlink should not be delivered");
	zassert_equal(state.event_count, 0U, "duplicate downlink should not emit events");
}

/* Verifies a unicast want_ack send is tracked and settled by a matching ROUTING acknowledgement. */
ZTEST(protocol_stack, test_want_ack_unicast_is_settled_by_routing_ack)
{
	uint8_t wire[MESHTASTIC_PKT_MAX];
	uint32_t wire_len;
	uint32_t id;

	id = send_want_ack_unicast("dm");
	zassert_equal(meshtastic_reliable_pending(), 1U, "packet was not tracked");

	build_routing_reply_wire(0x0AC00001U, id, meshtastic_Routing_Error_NONE, wire, &wire_len);
	mock_lora_inject_rx(wire, wire_len, -20, 5);

	zassert_ok(k_sem_take(&state.ack_sem, K_SECONDS(1)), "timed out waiting for ack event");
	zassert_equal(state.last_ack_type, MESHTASTIC_EVENT_TX_ACKED, "expected TX_ACKED");
	zassert_ok(state.last_ack_err, "unexpected error on acknowledged packet");
	zassert_equal(state.last_ack_packet.id, id, "ack reported the wrong packet id");
	zassert_equal(state.last_ack_packet.to, PEER_NODE_ID, "ack reported the wrong destination");
	zassert_equal(meshtastic_reliable_pending(), 0U, "tracking entry was not released");
}

/* Verifies broadcasts are never tracked, since no node acknowledges them. */
ZTEST(protocol_stack, test_want_ack_broadcast_is_not_tracked)
{
	struct meshtastic_packet packet = {
		.to = MESHTASTIC_NODE_BROADCAST,
		.portnum = MESHTASTIC_PORT_TEXT_MESSAGE,
		.payload = (const uint8_t *)"all",
		.payload_len = 3U,
		.want_ack = true,
	};
	int ret;

	ret = meshtastic_send_packet(&packet, K_FOREVER);
	zassert_ok(ret, "meshtastic_send_packet failed: %d", ret);
	zassert_ok(k_sem_take(&state.tx_sem, K_SECONDS(1)), "timed out waiting for tx event");

	zassert_equal(meshtastic_reliable_pending(), 0U, "broadcast should not be tracked");
}

/* Verifies a routing error settles the packet as undelivered rather than acknowledged. */
ZTEST(protocol_stack, test_routing_error_reports_delivery_failure)
{
	uint8_t wire[MESHTASTIC_PKT_MAX];
	uint32_t wire_len;
	uint32_t id;

	id = send_want_ack_unicast("dm");

	build_routing_reply_wire(0x0AC00002U, id, meshtastic_Routing_Error_NO_ROUTE, wire,
				 &wire_len);
	mock_lora_inject_rx(wire, wire_len, -20, 5);

	zassert_ok(k_sem_take(&state.ack_sem, K_SECONDS(1)), "timed out waiting for ack event");
	zassert_equal(state.last_ack_type, MESHTASTIC_EVENT_TX_NO_ACK, "expected TX_NO_ACK");
	zassert_equal(state.last_ack_err, -EHOSTUNREACH,
		      "expected -EHOSTUNREACH for routing error");
	zassert_equal(state.last_ack_packet.id, id, "failure reported the wrong packet id");
	zassert_equal(meshtastic_reliable_pending(), 0U, "tracking entry was not released");
}

/* Verifies an unanswered packet is retransmitted byte-for-byte, then reported as undelivered. */
ZTEST(protocol_stack, test_unacknowledged_packet_is_retransmitted_then_reported)
{
	uint8_t first[MESHTASTIC_PKT_MAX];
	uint8_t retry[MESHTASTIC_PKT_MAX];
	uint32_t first_len;
	uint32_t retry_len;
	uint32_t id;

	id = send_want_ack_unicast("retry");
	assert_mock_send_count(1U);
	first_len = mock_lora_last_tx(first, sizeof(first));

	/* The test build configures one retransmission, roughly one second out. */
	zassert_true(mock_lora_wait_for_send_count(2U, K_MSEC(3000)),
		     "the radio did not transmit in time");
	retry_len = mock_lora_last_tx(retry, sizeof(retry));

	/*
	 * The retry must reuse the original packet ID and frame: that is what
	 * lets the destination recognise it as a duplicate of a message it may
	 * already have delivered.
	 */
	zassert_equal(retry_len, first_len, "retransmission changed the frame length");
	zassert_mem_equal(retry, first, first_len, "retransmission altered the frame");

	zassert_ok(k_sem_take(&state.ack_sem, K_SECONDS(5)), "timed out waiting for no-ack event");
	zassert_equal(state.last_ack_type, MESHTASTIC_EVENT_TX_NO_ACK, "expected TX_NO_ACK");
	zassert_equal(state.last_ack_err, -ETIMEDOUT, "expected -ETIMEDOUT after retries");
	zassert_equal(state.last_ack_packet.id, id, "failure reported the wrong packet id");
	zassert_equal(meshtastic_reliable_pending(), 0U, "tracking entry was not released");
}

/*
 * Verifies a duplicate of a packet that wants an acknowledgement is acknowledged again.
 * Without this the sender's retry is wasted whenever it was the acknowledgement that was lost.
 */
ZTEST(protocol_stack, test_duplicate_want_ack_packet_is_acknowledged_again)
{
	uint8_t wire[MESHTASTIC_PKT_MAX];
	uint32_t wire_len;

	build_peer_want_ack_wire(0x0DEF0001U, "hi", wire, &wire_len);

	mock_lora_inject_rx(wire, wire_len, -20, 5);
	zassert_true(mock_lora_wait_for_send_count(1U, K_MSEC(1000)),
		     "the radio did not transmit in time");
	zassert_ok(k_sem_take(&state.rx_sem, K_SECONDS(1)), "timed out waiting for delivery");

	/* The same frame again: suppressed as a duplicate, but still acknowledged. */
	mock_lora_inject_rx(wire, wire_len, -20, 5);
	zassert_true(mock_lora_wait_for_send_count(2U, K_MSEC(1000)),
		     "the radio did not transmit in time");
	zassert_equal(state.recv_count, 1U, "duplicate must not be delivered twice");
}

/* Verifies a channel with no PSK sends and receives in the clear, as upstream does. */
ZTEST(protocol_stack, test_cleartext_channel_round_trips)
{
	meshtastic_Channel cleartext = *meshtastic_channels_get(0U);
	struct meshtastic_packet decoded;
	uint8_t payload[MESHTASTIC_MAX_PAYLOAD_LEN];
	uint8_t wire[MESHTASTIC_PKT_MAX];
	uint32_t wire_len;

	cleartext.settings.psk.size = 0U;
	zassert_ok(meshtastic_channels_set_slot(0U, &cleartext));

	zassert_ok(meshtastic_send_text(MESHTASTIC_NODE_BROADCAST, "clear"));
	zassert_ok(k_sem_take(&state.tx_sem, K_SECONDS(1)), "timed out waiting for tx event");

	wire_len = mock_lora_last_tx(wire, sizeof(wire));
	zassert_ok(meshtastic_decode_wire_packet(wire, (int)wire_len, 0, 0, &decoded, payload,
						 sizeof(payload)),
		   "an unencrypted frame should decode");
	assert_payload(decoded.payload, decoded.payload_len, "clear", strlen("clear"));
}

/* Verifies KNOWN_ONLY refuses to decode a packet whose sender is not in the NodeDB. */
ZTEST(protocol_stack, test_known_only_mode_leaves_a_stranger_undecoded)
{
	uint8_t wire[MESHTASTIC_PKT_MAX];
	uint32_t wire_len;
	uint32_t before = mt.status.decode_failures;

	build_peer_wire_packet(TEST_NODE_ID, 0x0AB10001U, 3U, "stranger", wire, &wire_len);
	meshtastic_set_rebroadcast_mode(meshtastic_Config_DeviceConfig_RebroadcastMode_KNOWN_ONLY);

	mock_lora_inject_rx(wire, wire_len, -20, 5);
	k_msleep(100);

	zassert_equal(state.recv_count, 0U, "an unknown sender must not be delivered");
	zassert_equal(mt.status.decode_failures, before + 1U);
}

/* Verifies CORE_PORTNUMS_ONLY drops packets from ports outside the standard set. */
ZTEST(protocol_stack, test_core_portnums_only_drops_other_ports)
{
	uint8_t wire[MESHTASTIC_PKT_MAX];
	uint32_t wire_len;
	struct meshtastic_packet packet = {
		.from = PEER_NODE_ID,
		.to = TEST_NODE_ID,
		.id = 0x0AB20001U,
		.portnum = MESHTASTIC_PORT_PRIVATE,
		.payload = (const uint8_t *)"private",
		.payload_len = strlen("private"),
		.hop_limit = 3U,
		.hop_start = 3U,
		.channel_index = meshtastic_channels_primary_index(),
	};

	zassert_ok(meshtastic_build_wire_packet(&packet, wire, &wire_len));
	meshtastic_set_rebroadcast_mode(
		meshtastic_Config_DeviceConfig_RebroadcastMode_CORE_PORTNUMS_ONLY);

	mock_lora_inject_rx(wire, wire_len, -20, 5);
	k_msleep(100);

	zassert_equal(state.recv_count, 0U, "a non-core port must not be delivered");

	/* A text message on the same policy still gets through. */
	packet.id = 0x0AB20002U;
	packet.portnum = MESHTASTIC_PORT_TEXT_MESSAGE;
	packet.payload = (const uint8_t *)"core";
	packet.payload_len = strlen("core");
	zassert_ok(meshtastic_build_wire_packet(&packet, wire, &wire_len));

	mock_lora_inject_rx(wire, wire_len, -20, 5);
	zassert_ok(k_sem_take(&state.rx_sem, K_SECONDS(1)), "timed out waiting for delivery");
}

/* Verifies LOCAL_ONLY keeps foreign traffic off our radio. */
ZTEST(protocol_stack, test_local_only_mode_suppresses_foreign_relay)
{
	uint8_t wire[MESHTASTIC_PKT_MAX];
	uint32_t wire_len;

	build_wire_packet(PEER_NODE_ID, OTHER_NODE_ID, 0x0AB30001U, 3U,
			  MESHTASTIC_PORT_TEXT_MESSAGE, (const uint8_t *)"relay", strlen("relay"),
			  wire, &wire_len);
	meshtastic_set_rebroadcast_mode(meshtastic_Config_DeviceConfig_RebroadcastMode_LOCAL_ONLY);

	mock_lora_inject_rx(wire, wire_len, -20, 5);
	k_msleep(100);

	assert_mock_send_count(0U);
}

/* Verifies a broadcast without a packet ID is not relayed: it cannot be deduplicated. */
ZTEST(protocol_stack, test_broadcast_without_an_id_is_not_relayed)
{
	uint8_t wire[MESHTASTIC_PKT_MAX];
	uint32_t wire_len;

	build_wire_packet(PEER_NODE_ID, MESHTASTIC_NODE_BROADCAST, 0U, 3U,
			  MESHTASTIC_PORT_TEXT_MESSAGE, (const uint8_t *)"anon", strlen("anon"),
			  wire, &wire_len);

	mock_lora_inject_rx(wire, wire_len, -20, 5);
	k_msleep(100);

	assert_mock_send_count(0U);
}

/* Verifies we do not relay a frame that names us as its source. */
ZTEST(protocol_stack, test_our_own_frame_heard_back_is_not_relayed)
{
	uint8_t wire[MESHTASTIC_PKT_MAX];
	uint32_t wire_len;

	build_wire_packet(TEST_NODE_ID, OTHER_NODE_ID, 0x0AB40001U, 3U,
			  MESHTASTIC_PORT_TEXT_MESSAGE, (const uint8_t *)"echo", strlen("echo"),
			  wire, &wire_len);

	mock_lora_inject_rx(wire, wire_len, -20, 5);
	k_msleep(100);

	assert_mock_send_count(0U);
}

/* Verifies the Data encoder rejects payloads it cannot represent. */
ZTEST(protocol_stack, test_data_encoding_rejects_bad_payloads)
{
	uint8_t buf[MESHTASTIC_PAYLOAD_MAX];
	uint8_t payload[MESHTASTIC_MAX_PAYLOAD_LEN] = {0};
	size_t encoded_len;

	zassert_equal(meshtastic_encode_data(MESHTASTIC_PORT_TEXT_MESSAGE, NULL, 1U, buf,
					     sizeof(buf), &encoded_len),
		      -EINVAL);
	zassert_equal(meshtastic_encode_data(MESHTASTIC_PORT_TEXT_MESSAGE, payload,
					     MESHTASTIC_MAX_PAYLOAD_LEN + 1U, buf, sizeof(buf),
					     &encoded_len),
		      -EINVAL);
	zassert_equal(meshtastic_encode_data(MESHTASTIC_PORT_TEXT_MESSAGE, payload, sizeof(payload),
					     buf, sizeof(buf), NULL),
		      -EINVAL);
	/* A full payload no longer fits once the Data framing is added. */
	zassert_equal(meshtastic_encode_data(MESHTASTIC_PORT_TEXT_MESSAGE, payload, sizeof(payload),
					     buf, 4U, &encoded_len),
		      -ENOMEM);
}

/* Verifies the wire decoder rejects frames it cannot make sense of. */
ZTEST(protocol_stack, test_wire_decoding_rejects_bad_frames)
{
	uint8_t wire[MESHTASTIC_PKT_MAX] = {0};
	uint8_t payload[MESHTASTIC_MAX_PAYLOAD_LEN];
	struct meshtastic_packet packet;
	bool decoded = true;

	zassert_equal(meshtastic_decode_wire_packet(wire, MESHTASTIC_HDR_LEN - 1, 0, 0, &packet,
						    payload, sizeof(payload)),
		      -EINVAL);
	zassert_equal(meshtastic_decode_wire_packet(NULL, MESHTASTIC_PKT_MAX, 0, 0, &packet,
						    payload, sizeof(payload)),
		      -EINVAL);

	/* A header whose channel hash matches no slot leaves the payload encrypted. */
	wire[12] = 0xA5U;
	zassert_equal(meshtastic_decode_wire_packet(wire, MESHTASTIC_HDR_LEN + 4, 0, 0, &packet,
						    payload, sizeof(payload)),
		      -EBADMSG);
	zassert_ok(meshtastic_try_decode_wire_packet(wire, MESHTASTIC_HDR_LEN + 4, 0, 0, &packet,
						     payload, sizeof(payload), &decoded));
	zassert_false(decoded);
}

/* Verifies the MeshPacket helpers guard their arguments. */
ZTEST(protocol_stack, test_mesh_packet_helpers_guard_their_arguments)
{
	meshtastic_MeshPacket mesh = meshtastic_MeshPacket_init_zero;
	struct meshtastic_packet packet = {0};

	zassert_equal(meshtastic_packet_to_mesh_pb(NULL, &mesh), -EINVAL);
	zassert_equal(meshtastic_packet_to_mesh_pb(&packet, NULL), -EINVAL);
	zassert_equal(meshtastic_mesh_pb_try_decode(NULL), -EINVAL);

	/* Copying from or to NULL must simply do nothing. */
	meshtastic_mesh_packet_copy(NULL, &mesh);
	meshtastic_mesh_packet_copy(&mesh, NULL);

	zassert_equal(meshtastic_packet_wire_hash_for_index(0U), meshtastic_channels_get_hash(0U));
}
