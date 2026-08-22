/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gnss.h>
#include <zephyr/drivers/gnss/gnss_publish.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <pb_decode.h>

#include <zephyr/meshtastic/gnss.h>
#include <zephyr/meshtastic/meshtastic.h>

#include "meshtastic_core.h"
#include "meshtastic_gnss.h"
#include "meshtastic_modules.h"
#include "meshtastic_packet.h"

#include "mock_lora.h"

#define TEST_NODE_ID 0x12345678U

/* Fresh peers per test: the reply-suppression window is not resettable. */
#define COLD_PEER       0xD0000001U
#define REQUEST_PEER    0xD0000002U
#define SUPPRESSED_PEER 0xD0000003U

/* Grenoble, 212 m up, heading north-east at 12 m/s. */
#define FIX_LATITUDE_NDEG  45188500000LL
#define FIX_LONGITUDE_NDEG 5724500000LL
#define FIX_ALTITUDE_MM    212000
#define FIX_BEARING_MDEG   45000U
#define FIX_SPEED_MMS      12000U
#define FIX_GEOID_MM       50000
#define FIX_HDOP_MILLI     1200U

static const struct device *const gnss = DEVICE_DT_GET(DT_ALIAS(gnss));

static struct meshtastic_config cfg = {
	.node_id = TEST_NODE_ID,
	.psk = meshtastic_default_psk,
	.psk_len = sizeof(meshtastic_default_psk),
	.channel_name = MESHTASTIC_CHANNEL_LONGFAST,
	.frequency = MESHTASTIC_FREQ_EU,
};

static uint8_t tx_payload[MESHTASTIC_MAX_PAYLOAD_LEN];
static uint32_t gnss_fix_events;

static void on_event(const struct meshtastic_event *event, void *user_data)
{
	ARG_UNUSED(user_data);

	if (event->type == MESHTASTIC_EVENT_GNSS_FIX) {
		gnss_fix_events++;
	}
}

static void publish_fix_at(enum gnss_fix_status status, int64_t latitude_ndeg)
{
	struct gnss_data data = {
		.nav_data =
			{
				.latitude = latitude_ndeg,
				.longitude = FIX_LONGITUDE_NDEG,
				.bearing = FIX_BEARING_MDEG,
				.speed = FIX_SPEED_MMS,
				.altitude = FIX_ALTITUDE_MM,
			},
		.info =
			{
				.satellites_cnt = 9U,
				.hdop = FIX_HDOP_MILLI,
				.geoid_separation = FIX_GEOID_MM,
				.fix_status = status,
				.fix_quality = GNSS_FIX_QUALITY_GNSS_SPS,
			},
	};

	gnss_publish_data(gnss, &data);
	k_msleep(20);
}

static void publish_fix(enum gnss_fix_status status)
{
	publish_fix_at(status, FIX_LATITUDE_NDEG);
}

/*
 * Position replies are rate limited for the whole mesh rather than per peer,
 * so a test that expects an answer has to let the previous window expire.
 */
static void wait_out_the_reply_window(void)
{
	k_msleep((CONFIG_MESHTASTIC_GNSS_REPLY_SUPPRESS_SEC * MSEC_PER_SEC) + 100);
}

static meshtastic_Position decode_position(const struct meshtastic_packet *packet)
{
	meshtastic_Position position = meshtastic_Position_init_zero;
	pb_istream_t stream = pb_istream_from_buffer(packet->payload, packet->payload_len);

	zassert_true(pb_decode(&stream, meshtastic_Position_fields, &position),
		     "Position decode failed");

	return position;
}

static void decode_tx(struct meshtastic_packet *packet)
{
	uint8_t wire[MESHTASTIC_PKT_MAX];
	uint32_t wire_len;

	zassert_true(mock_lora_wait_for_send_count(1U, K_MSEC(1000)),
		     "the radio did not transmit in time");
	wire_len = mock_lora_last_tx(wire, sizeof(wire));
	zassert_ok(meshtastic_decode_wire_packet(wire, (int)wire_len, 0, 0, packet, tx_payload,
						 sizeof(tx_payload)),
		   "could not decode the transmitted frame");
}

static struct meshtastic_packet position_request(uint32_t from)
{
	struct meshtastic_packet packet = {
		.from = from,
		.to = TEST_NODE_ID,
		.id = from,
		.portnum = MESHTASTIC_PORT_POSITION,
		.channel_index = MESHTASTIC_CHANNEL_INDEX_INVALID,
		.want_response = true,
	};

	return packet;
}

static void *setup(void)
{
	cfg.lora_dev = mock_lora_device();
	zassert_ok(meshtastic_init(&cfg), "meshtastic_init failed");
	meshtastic_set_event_cb(on_event, NULL);
	zassert_true(device_is_ready(gnss), "emulated GNSS device not ready");

	return NULL;
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);

	mock_lora_reset();
	gnss_fix_events = 0U;
}

ZTEST_SUITE(gnss_position, NULL, setup, before, NULL, NULL);

/*
 * ztest runs a suite's tests in name order and the module keeps its last fix
 * for the lifetime of the process, so this name sorts first: the cold-start
 * state only exists until another test publishes a fix.
 */
ZTEST(gnss_position, test_a_cold_start_reports_no_position)
{
	meshtastic_Position position;
	struct meshtastic_packet request = position_request(COLD_PEER);

	zassert_equal(meshtastic_gnss_get_last_position(&position), -ENODATA);
	zassert_equal(meshtastic_send_position(MESHTASTIC_NODE_BROADCAST), -ENODATA);

	meshtastic_dispatch_modules(&request);
	k_msleep(100);
	zassert_equal(mock_lora_send_count(), 0U, "a request cannot be answered without a fix");
}

ZTEST(gnss_position, test_a_differential_fix_reports_fix_type_three)
{
	meshtastic_Position position;

	publish_fix(GNSS_FIX_STATUS_DGNSS_FIX);

	zassert_ok(meshtastic_gnss_get_last_position(&position));
	zassert_equal(position.fix_type, 3U);
}

ZTEST(gnss_position, test_a_fix_is_converted_to_meshtastic_units)
{
	meshtastic_Position position;

	publish_fix(GNSS_FIX_STATUS_GNSS_FIX);

	zassert_equal(gnss_fix_events, 1U, "a usable fix should raise an event");
	zassert_ok(meshtastic_gnss_get_last_position(&position));

	/* Zephyr reports nanodegrees, the Meshtastic fields are 1e-7 degrees. */
	zassert_true(position.has_latitude_i);
	zassert_equal(position.latitude_i, (int32_t)(FIX_LATITUDE_NDEG / 100));
	zassert_true(position.has_longitude_i);
	zassert_equal(position.longitude_i, (int32_t)(FIX_LONGITUDE_NDEG / 100));
	/* Millimetres to metres. */
	zassert_true(position.has_altitude);
	zassert_equal(position.altitude, FIX_ALTITUDE_MM / 1000);
	zassert_true(position.has_altitude_geoidal_separation);
	zassert_equal(position.altitude_geoidal_separation, FIX_GEOID_MM / 1000);
	/* Millimetres per second to metres per second. */
	zassert_true(position.has_ground_speed);
	zassert_equal(position.ground_speed, FIX_SPEED_MMS / 1000U);
	/* Millidegrees to 1/100 degrees. */
	zassert_true(position.has_ground_track);
	zassert_equal(position.ground_track, FIX_BEARING_MDEG / 10U);
	/* HDOP is 1/1000 in Zephyr and 1/100 on the wire. */
	zassert_equal(position.HDOP, FIX_HDOP_MILLI / 10U);

	zassert_equal(position.sats_in_view, 9U);
	zassert_equal(position.fix_type, 2U, "a plain GNSS fix reports 2D/3D");
	zassert_equal(position.location_source, meshtastic_Position_LocSource_LOC_INTERNAL);
	zassert_equal(position.altitude_source, meshtastic_Position_AltSource_ALT_INTERNAL);
	zassert_equal(position.precision_bits, 32U);
}

ZTEST(gnss_position, test_a_lost_fix_keeps_the_last_known_position)
{
	meshtastic_Position position;

	publish_fix(GNSS_FIX_STATUS_GNSS_FIX);
	gnss_fix_events = 0U;

	publish_fix_at(GNSS_FIX_STATUS_NO_FIX, FIX_LATITUDE_NDEG + 1000000000LL);

	zassert_equal(gnss_fix_events, 0U, "a lost fix must not be reported as a new one");
	zassert_ok(meshtastic_gnss_get_last_position(&position));
	zassert_equal(position.latitude_i, (int32_t)(FIX_LATITUDE_NDEG / 100),
		      "the last usable fix should survive");
}

ZTEST(gnss_position, test_a_null_position_pointer_is_rejected)
{
	zassert_equal(meshtastic_gnss_get_last_position(NULL), -EINVAL);
}

ZTEST(gnss_position, test_a_position_request_is_answered_with_the_last_fix)
{
	struct meshtastic_packet request = position_request(REQUEST_PEER);
	struct meshtastic_packet reply;

	publish_fix(GNSS_FIX_STATUS_GNSS_FIX);
	wait_out_the_reply_window();
	mock_lora_reset();

	meshtastic_dispatch_modules(&request);

	decode_tx(&reply);
	zassert_equal(reply.to, REQUEST_PEER);
	zassert_equal(reply.portnum, MESHTASTIC_PORT_POSITION);
	zassert_equal(reply.request_id, request.id);
	zassert_false(reply.want_response);
	zassert_equal(decode_position(&reply).longitude_i, (int32_t)(FIX_LONGITUDE_NDEG / 100));
}

ZTEST(gnss_position, test_a_repeated_position_request_is_suppressed)
{
	struct meshtastic_packet request = position_request(SUPPRESSED_PEER);

	publish_fix(GNSS_FIX_STATUS_GNSS_FIX);
	wait_out_the_reply_window();
	mock_lora_reset();

	meshtastic_dispatch_modules(&request);
	zassert_true(mock_lora_wait_for_send_count(1U, K_MSEC(1000)),
		     "the radio did not transmit in time");

	request.id++;
	meshtastic_dispatch_modules(&request);
	k_msleep(100);

	zassert_equal(mock_lora_send_count(), 1U);
}

ZTEST(gnss_position, test_our_own_position_request_is_ignored)
{
	struct meshtastic_packet request = position_request(TEST_NODE_ID);

	publish_fix(GNSS_FIX_STATUS_GNSS_FIX);
	mock_lora_reset();

	meshtastic_dispatch_modules(&request);
	k_msleep(100);

	zassert_equal(mock_lora_send_count(), 0U);
}

ZTEST(gnss_position, test_sending_a_position_broadcasts_the_last_fix)
{
	struct meshtastic_packet sent;
	meshtastic_Position position;

	publish_fix(GNSS_FIX_STATUS_GNSS_FIX);
	mock_lora_reset();

	zassert_ok(meshtastic_send_position(MESHTASTIC_NODE_BROADCAST));

	decode_tx(&sent);
	zassert_equal(sent.portnum, MESHTASTIC_PORT_POSITION);
	zassert_equal(sent.to, MESHTASTIC_NODE_BROADCAST);

	position = decode_position(&sent);
	zassert_equal(position.latitude_i, (int32_t)(FIX_LATITUDE_NDEG / 100));
	zassert_true(position.seq_number > 0U, "each position carries a sequence number");
}
