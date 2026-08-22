/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <pb_decode.h>
#include <pb_encode.h>

#include <zephyr/meshtastic/meshtastic.h>

#include "meshtastic_channels.h"
#include "meshtastic_core.h"
#include "meshtastic_phoneapi.h"

#include "phoneapi_fixture.h"

#define QUEUE_SIZE 4U

static struct meshtastic_phoneapi_frame queue[QUEUE_SIZE];
static struct meshtastic_phoneapi api;
static uint32_t data_ready_count;
static uint32_t disconnect_count;
static uint32_t invalidate_count;

static void on_data_ready(struct meshtastic_phoneapi *unused)
{
	ARG_UNUSED(unused);
	data_ready_count++;
}

static void on_disconnect(struct meshtastic_phoneapi *unused)
{
	ARG_UNUSED(unused);
	disconnect_count++;
}

static void on_invalidate(struct meshtastic_phoneapi *unused)
{
	ARG_UNUSED(unused);
	invalidate_count++;
}

static void enqueue_text(const char *text)
{
	meshtastic_FromRadio from = meshtastic_FromRadio_init_zero;

	from.which_payload_variant = meshtastic_FromRadio_packet_tag;
	from.packet.id = (uint32_t)text[0];
	from.packet.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
	from.packet.decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
	from.packet.decoded.payload.size = (pb_size_t)strlen(text);
	memcpy(from.packet.decoded.payload.bytes, text, strlen(text));

	zassert_ok(meshtastic_phoneapi_enqueue_fromradio(&api, &from));
}

static meshtastic_FromRadio decode_frame(const struct meshtastic_phoneapi_frame *frame)
{
	meshtastic_FromRadio from = meshtastic_FromRadio_init_zero;
	pb_istream_t stream = pb_istream_from_buffer(frame->data, frame->len);

	zassert_true(pb_decode(&stream, meshtastic_FromRadio_fields, &from),
		     "FromRadio decode failed");

	return from;
}

static meshtastic_FromRadio pop_decoded(void)
{
	struct meshtastic_phoneapi_frame frame;

	zassert_true(meshtastic_phoneapi_pop_frame(&api, &frame), "expected a queued frame");

	return decode_frame(&frame);
}

static const char *pop_text(void)
{
	static char text[64];
	meshtastic_FromRadio from = pop_decoded();

	zassert_equal(from.which_payload_variant, meshtastic_FromRadio_packet_tag);
	memcpy(text, from.packet.decoded.payload.bytes, from.packet.decoded.payload.size);
	text[from.packet.decoded.payload.size] = '\0';

	return text;
}

static void send_toradio(const meshtastic_ToRadio *to)
{
	uint8_t buf[MESHTASTIC_API_FRAME_MAX];
	pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));

	zassert_true(pb_encode(&stream, meshtastic_ToRadio_fields, to), "ToRadio encode failed");
	meshtastic_phoneapi_handle_toradio(&api, buf, stream.bytes_written);
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);

	meshtastic_phoneapi_init(&api, "test", queue, QUEUE_SIZE, on_data_ready, on_disconnect,
				 on_invalidate, NULL);
	data_ready_count = 0U;
	disconnect_count = 0U;
	invalidate_count = 0U;
	mock_lora_reset();
}

ZTEST_SUITE(phoneapi, NULL, phoneapi_suite_setup, before, NULL, NULL);

ZTEST(phoneapi, test_frames_are_delivered_in_order)
{
	enqueue_text("one");
	enqueue_text("two");

	zassert_equal(meshtastic_phoneapi_pending_count(&api), 2U);
	zassert_equal(data_ready_count, 2U);
	zassert_str_equal(pop_text(), "one");
	zassert_str_equal(pop_text(), "two");
	zassert_equal(meshtastic_phoneapi_pending_count(&api), 0U);
}

ZTEST(phoneapi, test_a_full_queue_drops_the_oldest_frame)
{
	const char *names[] = {"a", "b", "c", "d", "e"};

	ARRAY_FOR_EACH(names, i) {
		enqueue_text(names[i]);
	}

	zassert_equal(meshtastic_phoneapi_pending_count(&api), QUEUE_SIZE);
	zassert_str_equal(pop_text(), "b", "the oldest frame should have been dropped");
	zassert_str_equal(pop_text(), "c");
	zassert_str_equal(pop_text(), "d");
	zassert_str_equal(pop_text(), "e");
}

ZTEST(phoneapi, test_an_undelivered_frame_can_be_pushed_back_to_the_front)
{
	struct meshtastic_phoneapi_frame frame;

	enqueue_text("first");
	enqueue_text("second");

	zassert_true(meshtastic_phoneapi_pop_frame(&api, &frame));
	meshtastic_phoneapi_push_frame_front(&api, &frame);

	zassert_equal(meshtastic_phoneapi_pending_count(&api), 2U);
	zassert_str_equal(pop_text(), "first");
	zassert_str_equal(pop_text(), "second");
}

ZTEST(phoneapi, test_pushing_back_into_a_full_queue_drops_the_newest_frame)
{
	struct meshtastic_phoneapi_frame frame;

	enqueue_text("a");
	enqueue_text("b");
	enqueue_text("c");
	enqueue_text("d");

	zassert_true(meshtastic_phoneapi_pop_frame(&api, &frame));
	enqueue_text("e");
	zassert_equal(meshtastic_phoneapi_pending_count(&api), QUEUE_SIZE);

	meshtastic_phoneapi_push_frame_front(&api, &frame);
	zassert_str_equal(pop_text(), "a");
	zassert_str_equal(pop_text(), "b");
	zassert_str_equal(pop_text(), "c");
	zassert_str_equal(pop_text(), "d");
	zassert_equal(meshtastic_phoneapi_pending_count(&api), 0U,
		      "\"e\" should have been dropped");
}

ZTEST(phoneapi, test_the_current_frame_is_held_until_the_transport_releases_it)
{
	struct meshtastic_phoneapi_frame frame;

	zassert_false(meshtastic_phoneapi_current_frame(&api, &frame), "queue starts empty");

	enqueue_text("held");
	zassert_true(meshtastic_phoneapi_current_frame(&api, &frame));
	zassert_equal(meshtastic_phoneapi_pending_count(&api), 1U, "the held frame still counts");

	/* A retried read returns the same frame rather than consuming the next one. */
	zassert_true(meshtastic_phoneapi_current_frame(&api, &frame));
	zassert_equal(decode_frame(&frame).packet.decoded.payload.bytes[0], 'h');

	meshtastic_phoneapi_current_frame_complete(&api);
	zassert_equal(meshtastic_phoneapi_pending_count(&api), 0U);
	zassert_false(meshtastic_phoneapi_current_frame(&api, &frame));
}

ZTEST(phoneapi, test_reset_clears_the_queue_and_the_handshake)
{
	enqueue_text("stale");
	meshtastic_phoneapi_enqueue_phone_config(&api, 7U);

	meshtastic_phoneapi_reset(&api);

	zassert_equal(meshtastic_phoneapi_pending_count(&api), 0U);
	zassert_equal(meshtastic_phoneapi_from_num(&api), 0U);
}

ZTEST(phoneapi, test_my_info_describes_the_local_node)
{
	meshtastic_FromRadio from;

	meshtastic_phoneapi_enqueue_my_info(&api, 0U);

	from = pop_decoded();
	zassert_equal(from.which_payload_variant, meshtastic_FromRadio_my_info_tag);
	zassert_equal(from.my_info.my_node_num, TEST_NODE_ID);
	zassert_equal(from.my_info.device_id.size, 4U);
	zassert_str_equal(from.my_info.pio_env, "zephyr");
}

ZTEST(phoneapi, test_rebooted_notifies_the_phone_to_resynchronise)
{
	meshtastic_FromRadio from;

	meshtastic_phoneapi_enqueue_rebooted(&api);

	from = pop_decoded();
	zassert_equal(from.which_payload_variant, meshtastic_FromRadio_rebooted_tag);
	zassert_true(from.rebooted);
}

ZTEST(phoneapi, test_want_config_replays_the_upstream_handshake_order)
{
	static const pb_size_t head[] = {
		meshtastic_FromRadio_my_info_tag,
		meshtastic_FromRadio_deviceuiConfig_tag,
		meshtastic_FromRadio_node_info_tag,
		meshtastic_FromRadio_metadata_tag,
	};
	meshtastic_FromRadio from;

	meshtastic_phoneapi_enqueue_phone_config(&api, 0xABCDEF01U);
	zassert_equal(invalidate_count, 1U, "any half-sent frame must be dropped");

	ARRAY_FOR_EACH(head, i) {
		from = pop_decoded();
		zassert_equal(from.which_payload_variant, head[i],
			      "handshake frame %zu out of order", i);
	}

	for (uint8_t i = 0U; i < MESHTASTIC_MAX_CHANNELS; i++) {
		from = pop_decoded();
		zassert_equal(from.which_payload_variant, meshtastic_FromRadio_channel_tag);
		zassert_equal(from.channel.index, i);
	}

	for (int i = 0; i < 10; i++) {
		from = pop_decoded();
		zassert_equal(from.which_payload_variant, meshtastic_FromRadio_config_tag,
			      "expected 10 Config frames");
	}

	for (int i = 0; i < 16; i++) {
		from = pop_decoded();
		zassert_equal(from.which_payload_variant, meshtastic_FromRadio_moduleConfig_tag,
			      "expected 16 ModuleConfig frames");
	}

	from = pop_decoded();
	zassert_equal(from.which_payload_variant, meshtastic_FromRadio_queueStatus_tag);
	zassert_equal(from.queueStatus.maxlen, QUEUE_SIZE);

	from = pop_decoded();
	zassert_equal(from.which_payload_variant, meshtastic_FromRadio_config_complete_id_tag);
	zassert_equal(from.config_complete_id, 0xABCDEF01U, "the phone's nonce must come back");

	zassert_equal(meshtastic_phoneapi_pending_count(&api), 0U, "handshake should be complete");
}

ZTEST(phoneapi, test_the_handshake_reports_the_local_channel_table)
{
	meshtastic_FromRadio from;

	meshtastic_phoneapi_enqueue_phone_config(&api, 1U);

	for (int i = 0; i < 4; i++) {
		(void)pop_decoded();
	}

	from = pop_decoded();
	zassert_equal(from.which_payload_variant, meshtastic_FromRadio_channel_tag);
	zassert_equal(from.channel.role, meshtastic_Channel_Role_PRIMARY);
	zassert_str_equal(from.channel.settings.name, MESHTASTIC_CHANNEL_LONGFAST);

	from = pop_decoded();
	zassert_equal(from.channel.index, 1);
	zassert_equal(from.channel.role, meshtastic_Channel_Role_DISABLED);
}

ZTEST(phoneapi, test_want_config_discards_frames_the_phone_has_not_read)
{
	enqueue_text("queued");
	meshtastic_phoneapi_enqueue_phone_config(&api, 1U);

	zassert_equal(pop_decoded().which_payload_variant, meshtastic_FromRadio_my_info_tag,
		      "want_config drops anything the phone has not read yet");
}

ZTEST(phoneapi, test_next_config_frame_is_idle_outside_a_handshake)
{
	struct meshtastic_phoneapi_frame frame;

	zassert_equal(meshtastic_phoneapi_next_config_frame(&api, &frame), -ENOENT);
}

ZTEST(phoneapi, test_a_toradio_packet_is_transmitted_and_acknowledged_locally)
{
	meshtastic_ToRadio to = meshtastic_ToRadio_init_zero;
	meshtastic_FromRadio from;

	to.which_payload_variant = meshtastic_ToRadio_packet_tag;
	to.packet.id = 0x1234U;
	to.packet.to = MESHTASTIC_NODE_BROADCAST;
	to.packet.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
	to.packet.decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
	to.packet.decoded.payload.size = 5U;
	memcpy(to.packet.decoded.payload.bytes, "hello", 5U);

	send_toradio(&to);

	zassert_equal(mock_lora_send_count(), 1U, "the packet should reach the radio");

	from = pop_decoded();
	zassert_equal(from.which_payload_variant, meshtastic_FromRadio_queueStatus_tag);
	zassert_equal(from.queueStatus.mesh_packet_id, 0x1234U);
	zassert_equal(from.queueStatus.res, 0);
}

ZTEST(phoneapi, test_a_heartbeat_answers_with_the_queue_status)
{
	meshtastic_ToRadio to = meshtastic_ToRadio_init_zero;
	meshtastic_FromRadio from;

	to.which_payload_variant = meshtastic_ToRadio_heartbeat_tag;
	send_toradio(&to);

	from = pop_decoded();
	zassert_equal(from.which_payload_variant, meshtastic_FromRadio_queueStatus_tag);
	zassert_equal(from.queueStatus.mesh_packet_id, 0U);
	zassert_equal(from.queueStatus.maxlen, QUEUE_SIZE);
}

ZTEST(phoneapi, test_a_disconnect_notifies_the_transport)
{
	meshtastic_ToRadio to = meshtastic_ToRadio_init_zero;

	to.which_payload_variant = meshtastic_ToRadio_disconnect_tag;
	send_toradio(&to);

	zassert_equal(disconnect_count, 1U);
	zassert_equal(meshtastic_phoneapi_pending_count(&api), 0U);
}

ZTEST(phoneapi, test_undecodable_toradio_frames_are_dropped)
{
	const uint8_t garbage[] = {0xFFU, 0xFFU, 0xFFU, 0xFFU};

	meshtastic_phoneapi_handle_toradio(&api, garbage, sizeof(garbage));

	zassert_equal(meshtastic_phoneapi_pending_count(&api), 0U);
	zassert_equal(mock_lora_send_count(), 0U);
}

ZTEST(phoneapi, test_a_trailing_stream_start_byte_is_recovered)
{
	meshtastic_ToRadio to = meshtastic_ToRadio_init_zero;
	uint8_t buf[MESHTASTIC_API_FRAME_MAX];
	pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));

	to.which_payload_variant = meshtastic_ToRadio_heartbeat_tag;
	zassert_true(pb_encode(&stream, meshtastic_ToRadio_fields, &to));

	/* A bogus StreamAPI length can pull the next frame's START1 into the payload. */
	buf[stream.bytes_written] = 0x94U;
	meshtastic_phoneapi_handle_toradio(&api, buf, stream.bytes_written + 1U);

	zassert_equal(pop_decoded().which_payload_variant, meshtastic_FromRadio_queueStatus_tag);
}

ZTEST(phoneapi, test_unsupported_toradio_variants_are_ignored)
{
	meshtastic_ToRadio to = meshtastic_ToRadio_init_zero;

	to.which_payload_variant = meshtastic_ToRadio_mqttClientProxyMessage_tag;
	send_toradio(&to);

	zassert_equal(meshtastic_phoneapi_pending_count(&api), 0U);
}

ZTEST(phoneapi, test_received_packets_fan_out_to_registered_transports)
{
	struct meshtastic_packet packet = {
		.from = PEER_NODE_ID,
		.to = TEST_NODE_ID,
		.id = 0x2222U,
		.portnum = MESHTASTIC_PORT_TEXT_MESSAGE,
		.payload = (const uint8_t *)"radio",
		.payload_len = 5U,
		.channel_index = MESHTASTIC_CHANNEL_INDEX_INVALID,
	};
	meshtastic_FromRadio from;

	meshtastic_phoneapi_register(&api);
	meshtastic_phoneapi_register(&api);

	meshtastic_phoneapi_on_packet(&packet);

	from = pop_decoded();
	zassert_equal(from.which_payload_variant, meshtastic_FromRadio_packet_tag);
	zassert_equal(from.packet.from, PEER_NODE_ID);
	zassert_equal(from.packet.id, 0x2222U);
	zassert_equal(from.packet.decoded.portnum, meshtastic_PortNum_TEXT_MESSAGE_APP);
}
