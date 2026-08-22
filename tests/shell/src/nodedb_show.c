/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

/**
 * @file
 * @brief What "nodedb show" prints once a peer has been heard from.
 */

#include <zephyr/ztest.h>

#include <pb_encode.h>

#include "meshtastic_core.h"
#include "meshtastic_modules.h"

#include "shell_fixture.h"

#include "meshtastic/telemetry.pb.h"

int meshtastic_nodedb_init(void);

#define SHOW_PEER "meshtastic nodedb show 0x87654321"

static uint8_t payload[MESHTASTIC_MAX_PAYLOAD_LEN];

static void deliver(uint32_t portnum, const pb_msgdesc_t *fields, const void *message)
{
	pb_ostream_t stream = pb_ostream_from_buffer(payload, sizeof(payload));
	struct meshtastic_packet packet = {
		.from = SHELL_TEST_PEER_ID,
		.to = MESHTASTIC_NODE_BROADCAST,
		.portnum = portnum,
		.payload = payload,
	};

	zassert_true(pb_encode(&stream, fields, message), "protobuf encode failed");
	packet.payload_len = stream.bytes_written;

	meshtastic_dispatch_modules(&packet);
}

static void introduce_peer(void)
{
	meshtastic_User user = meshtastic_User_init_zero;

	snprintk(user.id, sizeof(user.id), "!%08x", SHELL_TEST_PEER_ID);
	strcpy(user.long_name, "Peer Node");
	strcpy(user.short_name, "PEER");
	user.hw_model = meshtastic_HardwareModel_TBEAM;

	deliver(MESHTASTIC_PORT_NODEINFO, meshtastic_User_fields, &user);
}

static void deliver_environment(float temperature)
{
	meshtastic_Telemetry telemetry = meshtastic_Telemetry_init_zero;

	telemetry.which_variant = meshtastic_Telemetry_environment_metrics_tag;
	telemetry.variant.environment_metrics.has_temperature = true;
	telemetry.variant.environment_metrics.temperature = temperature;
	telemetry.variant.environment_metrics.has_relative_humidity = true;
	telemetry.variant.environment_metrics.relative_humidity = 48.0f;
	telemetry.variant.environment_metrics.has_barometric_pressure = true;
	telemetry.variant.environment_metrics.barometric_pressure = 1013.25f;

	deliver(MESHTASTIC_PORT_TELEMETRY, meshtastic_Telemetry_fields, &telemetry);
}

static void before(void *fixture)
{
	shell_test_before(fixture);
	zassert_ok(meshtastic_nodedb_init(), "nodedb reset failed");
	introduce_peer();
}

ZTEST_SUITE(shell_nodedb_show, NULL, shell_test_setup, before, NULL, NULL);

ZTEST(shell_nodedb_show, test_the_last_known_fix_is_printed)
{
	meshtastic_Position position = meshtastic_Position_init_zero;

	position.has_latitude_i = true;
	position.latitude_i = 486000000;
	position.has_longitude_i = true;
	position.longitude_i = 23000000;
	position.has_altitude = true;
	position.altitude = 340;
	position.time = 1700000000U;
	position.precision_bits = 13U;

	deliver(MESHTASTIC_PORT_POSITION, meshtastic_Position_fields, &position);

	zassert_ok(shell_run(SHOW_PEER));
	shell_expect("position: lat_i=486000000 lon_i=23000000 alt=340 "
		     "time=1700000000 precision=13\r\n");
}

ZTEST(shell_nodedb_show, test_device_metrics_are_printed)
{
	meshtastic_Telemetry telemetry = meshtastic_Telemetry_init_zero;

	telemetry.which_variant = meshtastic_Telemetry_device_metrics_tag;
	telemetry.variant.device_metrics.has_battery_level = true;
	telemetry.variant.device_metrics.battery_level = 87U;
	telemetry.variant.device_metrics.has_voltage = true;
	telemetry.variant.device_metrics.voltage = 3.5f;
	telemetry.variant.device_metrics.has_uptime_seconds = true;
	telemetry.variant.device_metrics.uptime_seconds = 3600U;

	deliver(MESHTASTIC_PORT_TELEMETRY, meshtastic_Telemetry_fields, &telemetry);

	zassert_ok(shell_run(SHOW_PEER));
	shell_expect("battery: 87%\r\n");
	shell_expect("voltage: 3.500\r\n");
	shell_expect("uptime: 3600s\r\n");
}

ZTEST(shell_nodedb_show, test_environment_metrics_are_printed)
{
	deliver_environment(21.5f);

	zassert_ok(shell_run(SHOW_PEER));
	shell_expect("temperature: 21.5\r\n");
	shell_expect("humidity: 48.0\r\n");
	shell_expect("pressure: 1013.2\r\n");
}

ZTEST(shell_nodedb_show, test_a_reading_below_zero_keeps_its_sign)
{
	deliver_environment(-5.5f);

	zassert_ok(shell_run(SHOW_PEER));
	shell_expect("temperature: -5.5\r\n");
}
