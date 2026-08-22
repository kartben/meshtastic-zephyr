/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <pb_decode.h>
#include <pb_encode.h>

#include <zephyr/meshtastic/meshtastic.h>
#include <zephyr/meshtastic/telemetry.h>

#include "meshtastic_modules.h"
#include "meshtastic_telemetry_internal.h"

#include "fake_sensor.h"
#include "modules_fixture.h"

/* Collection helpers are declared privately by the stack. */
int meshtastic_collect_device_metrics(meshtastic_DeviceMetrics *metrics);
int meshtastic_collect_environment(meshtastic_EnvironmentMetrics *metrics);

/* Fresh peers per test: the reply-suppression tables are not resettable. */
#define METRICS_PEER     0xA0000001U
#define SUPPRESSED_PEER  0xA0000002U
#define ENVIRONMENT_PEER 0xA0000003U
#define BROADCAST_PEER   0xA0000004U
#define NO_SENSOR_PEER   0xA0000005U

static const struct fake_sensor_values readings = {
	.temperature = 21.5,      /* deg C */
	.humidity = 48.0,         /* %RH */
	.pressure = 101.3,        /* kPa */
	.gas_resistance = 250000, /* ohm */
	.light = 830.0,           /* lux */
};

static uint8_t request_payload[MESHTASTIC_MAX_PAYLOAD_LEN];
static uint8_t reply_payload[MESHTASTIC_MAX_PAYLOAD_LEN];

static struct meshtastic_packet telemetry_request(uint32_t from, uint32_t to,
						  const meshtastic_Telemetry *request)
{
	struct meshtastic_packet packet = {
		.from = from,
		.to = to,
		.id = from,
		.portnum = MESHTASTIC_PORT_TELEMETRY,
		.channel_index = MESHTASTIC_CHANNEL_INDEX_INVALID,
		.want_response = true,
	};
	pb_ostream_t stream;

	if (request != NULL) {
		stream = pb_ostream_from_buffer(request_payload, sizeof(request_payload));
		zassert_true(pb_encode(&stream, meshtastic_Telemetry_fields, request),
			     "Telemetry encode failed");
		packet.payload = request_payload;
		packet.payload_len = stream.bytes_written;
	}

	return packet;
}

static meshtastic_Telemetry decode_telemetry(const struct meshtastic_packet *packet)
{
	meshtastic_Telemetry telemetry = meshtastic_Telemetry_init_zero;
	pb_istream_t stream = pb_istream_from_buffer(packet->payload, packet->payload_len);

	zassert_true(pb_decode(&stream, meshtastic_Telemetry_fields, &telemetry),
		     "Telemetry decode failed");

	return telemetry;
}

/* Telemetry senders are known peers; an unknown one would also be probed. */
static void introduce(uint32_t node)
{
	modules_introduce_peer(node);
	k_msleep(50);
	modules_reset();
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);

	fake_sensor_set_values(&readings);
	fake_sensor_set_failing(false);
	modules_reset();
}

ZTEST_SUITE(telemetry, NULL, modules_suite_setup, before, NULL, NULL);

ZTEST(telemetry, test_device_metrics_always_report_uptime)
{
	meshtastic_DeviceMetrics metrics;

	zassert_ok(meshtastic_collect_device_metrics(&metrics));
	zassert_true(metrics.has_uptime_seconds);
	zassert_equal(meshtastic_collect_device_metrics(NULL), -EINVAL);
}

ZTEST(telemetry, test_sending_device_metrics_transmits_a_telemetry_packet)
{
	struct meshtastic_packet sent;
	meshtastic_Telemetry telemetry;

	zassert_ok(meshtastic_send_device_metrics(MESHTASTIC_NODE_BROADCAST, K_FOREVER));

	modules_decode_tx(1U, &sent, reply_payload, sizeof(reply_payload));
	zassert_equal(sent.portnum, MESHTASTIC_PORT_TELEMETRY);
	zassert_equal(sent.to, MESHTASTIC_NODE_BROADCAST);

	telemetry = decode_telemetry(&sent);
	zassert_equal(telemetry.which_variant, meshtastic_Telemetry_device_metrics_tag);
	zassert_true(telemetry.variant.device_metrics.has_uptime_seconds);
}

ZTEST(telemetry, test_a_request_is_answered_with_device_metrics)
{
	struct meshtastic_packet request = telemetry_request(METRICS_PEER, TEST_NODE_ID, NULL);
	struct meshtastic_packet reply;
	meshtastic_Telemetry telemetry;

	introduce(METRICS_PEER);
	meshtastic_dispatch_modules(&request);

	modules_decode_tx(1U, &reply, reply_payload, sizeof(reply_payload));
	zassert_equal(reply.to, METRICS_PEER);
	zassert_equal(reply.portnum, MESHTASTIC_PORT_TELEMETRY);
	zassert_equal(reply.request_id, request.id, "the reply must correlate with the request");
	zassert_false(reply.want_response, "a reply must not ask for another reply");

	telemetry = decode_telemetry(&reply);
	zassert_equal(telemetry.which_variant, meshtastic_Telemetry_device_metrics_tag);
}

ZTEST(telemetry, test_a_repeated_request_is_suppressed)
{
	struct meshtastic_packet request = telemetry_request(SUPPRESSED_PEER, TEST_NODE_ID, NULL);

	introduce(SUPPRESSED_PEER);
	meshtastic_dispatch_modules(&request);
	mock_lora_wait_for_send_count(1U, K_MSEC(500));

	request.id++;
	meshtastic_dispatch_modules(&request);
	k_msleep(100);

	zassert_equal(mock_lora_send_count(), 1U,
		      "a second request inside the suppression window must be ignored");
}

ZTEST(telemetry, test_an_environment_request_is_answered_with_sensor_readings)
{
	meshtastic_Telemetry request = meshtastic_Telemetry_init_zero;
	struct meshtastic_packet packet;
	struct meshtastic_packet reply;
	meshtastic_Telemetry telemetry;

	request.which_variant = meshtastic_Telemetry_environment_metrics_tag;
	packet = telemetry_request(ENVIRONMENT_PEER, TEST_NODE_ID, &request);

	introduce(ENVIRONMENT_PEER);
	meshtastic_dispatch_modules(&packet);

	modules_decode_tx(1U, &reply, reply_payload, sizeof(reply_payload));
	telemetry = decode_telemetry(&reply);
	zassert_equal(telemetry.which_variant, meshtastic_Telemetry_environment_metrics_tag);
	zassert_within(telemetry.variant.environment_metrics.temperature, 21.5f, 0.01f);
}

ZTEST(telemetry, test_an_environment_request_is_ignored_when_the_sensor_fails)
{
	meshtastic_Telemetry request = meshtastic_Telemetry_init_zero;
	struct meshtastic_packet packet;

	fake_sensor_set_failing(true);
	request.which_variant = meshtastic_Telemetry_environment_metrics_tag;
	packet = telemetry_request(NO_SENSOR_PEER, TEST_NODE_ID, &request);

	introduce(NO_SENSOR_PEER);
	meshtastic_dispatch_modules(&packet);
	k_msleep(100);

	zassert_equal(mock_lora_send_count(), 0U);
}

ZTEST(telemetry, test_broadcast_and_local_requests_are_not_answered)
{
	struct meshtastic_packet broadcast =
		telemetry_request(BROADCAST_PEER, MESHTASTIC_NODE_BROADCAST, NULL);
	struct meshtastic_packet loopback = telemetry_request(TEST_NODE_ID, TEST_NODE_ID, NULL);

	introduce(BROADCAST_PEER);
	meshtastic_dispatch_modules(&broadcast);
	meshtastic_dispatch_modules(&loopback);
	k_msleep(100);

	zassert_equal(mock_lora_send_count(), 0U);
}

ZTEST(telemetry, test_environment_metrics_are_converted_to_meshtastic_units)
{
	meshtastic_EnvironmentMetrics metrics;

	zassert_equal(meshtastic_collect_environment(&metrics), 5, "expected five readings");
	zassert_true(metrics.has_temperature);
	zassert_within(metrics.temperature, 21.5f, 0.01f);
	zassert_true(metrics.has_relative_humidity);
	zassert_within(metrics.relative_humidity, 48.0f, 0.01f);
	/* Zephyr reports kPa, the Meshtastic field is hPa. */
	zassert_true(metrics.has_barometric_pressure);
	zassert_within(metrics.barometric_pressure, 1013.0f, 0.1f);
	/* Zephyr reports ohms, the Meshtastic field is MOhm. */
	zassert_true(metrics.has_gas_resistance);
	zassert_within(metrics.gas_resistance, 0.25f, 0.001f);
	zassert_true(metrics.has_lux);
	zassert_within(metrics.lux, 830.0f, 0.1f);

	zassert_equal(meshtastic_collect_environment(NULL), -EINVAL);
}

ZTEST(telemetry, test_sending_environment_metrics_transmits_a_telemetry_packet)
{
	struct meshtastic_packet sent;

	zassert_ok(meshtastic_send_environment(MESHTASTIC_NODE_BROADCAST, K_FOREVER));

	modules_decode_tx(1U, &sent, reply_payload, sizeof(reply_payload));
	zassert_equal(decode_telemetry(&sent).which_variant,
		      meshtastic_Telemetry_environment_metrics_tag);
}

ZTEST(telemetry, test_a_failing_sensor_reports_a_metrics_error)
{
	meshtastic_EnvironmentMetrics metrics;
	struct meshtastic_event event;

	fake_sensor_set_failing(true);

	zassert_equal(meshtastic_collect_environment(&metrics), -ENODEV);
	zassert_equal(meshtastic_send_environment(MESHTASTIC_NODE_BROADCAST, K_NO_WAIT), -ENODEV);
	zassert_true(modules_last_event(&event, NULL, NULL, 0U));
	zassert_equal(event.type, MESHTASTIC_EVENT_METRICS_ERROR);
	zassert_equal(event.err, -ENODEV);
}

ZTEST(telemetry, test_an_empty_request_payload_decodes_as_an_unset_variant)
{
	meshtastic_Telemetry request;
	struct meshtastic_packet packet = {0};
	const uint8_t garbage[] = {0xFFU, 0xFFU, 0xFFU};

	zassert_true(meshtastic_telemetry_decode_request(NULL, &request));
	zassert_equal(request.which_variant, 0U);

	zassert_true(meshtastic_telemetry_decode_request(&packet, &request));
	zassert_equal(request.which_variant, 0U);

	zassert_false(meshtastic_telemetry_decode_request(&packet, NULL));

	packet.payload = garbage;
	packet.payload_len = sizeof(garbage);
	zassert_false(meshtastic_telemetry_decode_request(&packet, &request));
}

ZTEST(telemetry, test_encoding_a_reply_validates_its_arguments)
{
	meshtastic_Telemetry telemetry = meshtastic_Telemetry_init_zero;
	struct meshtastic_packet packet;

	zassert_equal(meshtastic_telemetry_encode_packet(0U, 0U, 0U, NULL, reply_payload, &packet),
		      -EINVAL);
	zassert_equal(meshtastic_telemetry_encode_packet(0U, 0U, 0U, &telemetry, NULL, &packet),
		      -EINVAL);
	zassert_equal(
		meshtastic_telemetry_encode_packet(0U, 0U, 0U, &telemetry, reply_payload, NULL),
		-EINVAL);
}
