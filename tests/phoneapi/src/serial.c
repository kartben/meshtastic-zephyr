/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/serial/uart_emul.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <pb_decode.h>
#include <pb_encode.h>

#include <zephyr/meshtastic/meshtastic.h>

#include "meshtastic_phoneapi.h"
#include "meshtastic_serial.h"

#include "phoneapi_fixture.h"

/* StreamAPI framing: 0x94 0xc3 then a big-endian 16-bit payload length. */
#define START1     0x94U
#define START2     0xC3U
#define HEADER_LEN 4U

static const struct device *const uart = DEVICE_DT_GET(DT_CHOSEN(zephyr_meshtastic_uart));

static void feed(const uint8_t *data, size_t len)
{
	zassert_equal(uart_emul_put_rx_data(uart, data, len), len, "UART RX FIFO full");
	k_msleep(50);
}

static void feed_toradio(const meshtastic_ToRadio *to)
{
	uint8_t frame[MESHTASTIC_API_FRAME_MAX + HEADER_LEN];
	pb_ostream_t stream =
		pb_ostream_from_buffer(&frame[HEADER_LEN], sizeof(frame) - HEADER_LEN);

	zassert_true(pb_encode(&stream, meshtastic_ToRadio_fields, to), "ToRadio encode failed");

	frame[0] = START1;
	frame[1] = START2;
	frame[2] = (uint8_t)(stream.bytes_written >> 8);
	frame[3] = (uint8_t)stream.bytes_written;

	feed(frame, HEADER_LEN + stream.bytes_written);
}

static size_t drain_tx(uint8_t *out, size_t out_len)
{
	size_t total = 0U;
	uint32_t read;

	/* The transport hands frames to the UART from its own work queue. */
	for (int i = 0; i < 20 && total < out_len; i++) {
		read = uart_emul_get_tx_data(uart, &out[total], out_len - total);
		if (read == 0U && total > 0U) {
			break;
		}
		total += read;
		k_msleep(10);
	}

	return total;
}

static meshtastic_FromRadio decode_first_frame(const uint8_t *buf, size_t len)
{
	meshtastic_FromRadio from = meshtastic_FromRadio_init_zero;
	pb_istream_t stream;
	uint16_t payload_len;

	zassert_true(len >= HEADER_LEN, "no StreamAPI frame in %zu byte(s)", len);
	zassert_equal(buf[0], START1);
	zassert_equal(buf[1], START2);

	payload_len = ((uint16_t)buf[2] << 8) | buf[3];
	zassert_true(len >= HEADER_LEN + payload_len, "truncated StreamAPI frame");

	stream = pb_istream_from_buffer(&buf[HEADER_LEN], payload_len);
	zassert_true(pb_decode(&stream, meshtastic_FromRadio_fields, &from),
		     "FromRadio decode failed");

	return from;
}

static void before(void *fixture)
{
	uint8_t discard[512];
	int idle = 0;

	ARG_UNUSED(fixture);

	(void)uart_emul_flush_rx_data(uart);

	/* Let the transport's work queue finish flushing anything still queued. */
	while (idle < 3) {
		k_msleep(10);
		idle = (uart_emul_get_tx_data(uart, discard, sizeof(discard)) == 0U) ? idle + 1 : 0;
	}

	mock_lora_reset();
}

ZTEST_SUITE(serial_transport, NULL, phoneapi_suite_setup, before, NULL, NULL);

ZTEST(serial_transport, test_encode_frame_matches_the_streamapi_header)
{
	const uint8_t payload[] = {0x01U, 0x02U, 0x03U};
	uint8_t out[16];

	zassert_equal(meshtastic_serial_encode_frame(payload, sizeof(payload), out, sizeof(out)),
		      HEADER_LEN + sizeof(payload));
	zassert_equal(out[0], START1);
	zassert_equal(out[1], START2);
	zassert_equal(out[2], 0U);
	zassert_equal(out[3], sizeof(payload));
	zassert_mem_equal(&out[HEADER_LEN], payload, sizeof(payload));
}

ZTEST(serial_transport, test_encode_frame_refuses_to_overflow)
{
	const uint8_t payload[] = {0x01U, 0x02U, 0x03U};
	uint8_t out[16];

	zassert_equal(meshtastic_serial_encode_frame(payload, sizeof(payload), out, HEADER_LEN),
		      0U);
	zassert_equal(meshtastic_serial_encode_frame(payload, MESHTASTIC_API_FRAME_MAX + 1U, out,
						     sizeof(out)),
		      0U);
}

ZTEST(serial_transport, test_a_framed_heartbeat_is_answered_over_the_uart)
{
	meshtastic_ToRadio to = meshtastic_ToRadio_init_zero;
	uint8_t tx[512];
	size_t len;

	to.which_payload_variant = meshtastic_ToRadio_heartbeat_tag;
	feed_toradio(&to);

	len = drain_tx(tx, sizeof(tx));
	zassert_equal(decode_first_frame(tx, len).which_payload_variant,
		      meshtastic_FromRadio_queueStatus_tag);
}

ZTEST(serial_transport, test_a_framed_packet_reaches_the_radio)
{
	meshtastic_ToRadio to = meshtastic_ToRadio_init_zero;

	to.which_payload_variant = meshtastic_ToRadio_packet_tag;
	to.packet.id = 0x4321U;
	to.packet.to = MESHTASTIC_NODE_BROADCAST;
	to.packet.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
	to.packet.decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
	to.packet.decoded.payload.size = 4U;
	memcpy(to.packet.decoded.payload.bytes, "ping", 4U);

	feed_toradio(&to);

	zassert_equal(mock_lora_send_count(), 1U);
}

ZTEST(serial_transport, test_leading_noise_is_skipped_before_the_start_bytes)
{
	const uint8_t noise[] = {'j', 'u', 'n', 'k', START1, 0x00U};
	meshtastic_ToRadio to = meshtastic_ToRadio_init_zero;
	uint8_t tx[512];
	size_t len;

	feed(noise, sizeof(noise));

	to.which_payload_variant = meshtastic_ToRadio_heartbeat_tag;
	feed_toradio(&to);

	len = drain_tx(tx, sizeof(tx));
	zassert_equal(decode_first_frame(tx, len).which_payload_variant,
		      meshtastic_FromRadio_queueStatus_tag);
}

ZTEST(serial_transport, test_an_oversized_length_resynchronises_the_parser)
{
	const uint8_t bogus[] = {START1, START2, 0xFFU, 0xFFU};
	meshtastic_ToRadio to = meshtastic_ToRadio_init_zero;
	uint8_t tx[512];
	size_t len;

	feed(bogus, sizeof(bogus));

	to.which_payload_variant = meshtastic_ToRadio_heartbeat_tag;
	feed_toradio(&to);

	len = drain_tx(tx, sizeof(tx));
	zassert_equal(decode_first_frame(tx, len).which_payload_variant,
		      meshtastic_FromRadio_queueStatus_tag);
}

ZTEST(serial_transport, test_an_empty_frame_is_ignored)
{
	const uint8_t empty[] = {START1, START2, 0x00U, 0x00U};
	uint8_t tx[512];

	feed(empty, sizeof(empty));

	zassert_equal(drain_tx(tx, sizeof(tx)), 0U, "an empty frame must not be answered");
}

ZTEST(serial_transport, test_a_received_packet_is_streamed_to_the_phone)
{
	struct meshtastic_packet packet = {
		.from = PEER_NODE_ID,
		.to = TEST_NODE_ID,
		.id = 0x9999U,
		.portnum = MESHTASTIC_PORT_TEXT_MESSAGE,
		.payload = (const uint8_t *)"mesh",
		.payload_len = 4U,
		.channel_index = MESHTASTIC_CHANNEL_INDEX_INVALID,
	};
	meshtastic_FromRadio from;
	uint8_t tx[512];
	size_t len;

	meshtastic_phoneapi_on_packet(&packet);

	len = drain_tx(tx, sizeof(tx));
	from = decode_first_frame(tx, len);
	zassert_equal(from.which_payload_variant, meshtastic_FromRadio_packet_tag);
	zassert_equal(from.packet.id, 0x9999U);
	zassert_equal(from.packet.from, PEER_NODE_ID);
}
