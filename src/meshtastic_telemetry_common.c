/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

#include <errno.h>

#include <pb_decode.h>
#include <pb_encode.h>

#include "meshtastic_telemetry_internal.h"

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(meshtastic, CONFIG_MESHTASTIC_LOG_LEVEL);

bool meshtastic_telemetry_decode_request(const struct meshtastic_packet *packet,
					 meshtastic_Telemetry *request)
{
	pb_istream_t stream;

	if (request == NULL) {
		return false;
	}

	if (packet == NULL || packet->payload == NULL || packet->payload_len == 0U) {
		*request = (meshtastic_Telemetry)meshtastic_Telemetry_init_zero;
		return true;
	}

	stream = pb_istream_from_buffer(packet->payload, packet->payload_len);
	if (!pb_decode(&stream, meshtastic_Telemetry_fields, request)) {
		LOG_DBG("Telemetry request decode failed: %s", PB_GET_ERROR(&stream));
		return false;
	}

	return true;
}

int meshtastic_telemetry_encode_packet(uint32_t dest, uint8_t channel, uint32_t response_to_id,
				       const meshtastic_Telemetry *telemetry, uint8_t *payload,
				       struct meshtastic_packet *packet)
{
	pb_ostream_t stream;

	if (telemetry == NULL || payload == NULL || packet == NULL) {
		return -EINVAL;
	}

	stream = pb_ostream_from_buffer(payload, MESHTASTIC_MAX_PAYLOAD_LEN);
	if (!pb_encode(&stream, meshtastic_Telemetry_fields, telemetry)) {
		LOG_ERR("Telemetry encode failed: %s", PB_GET_ERROR(&stream));
		return -ENOMEM;
	}

	*packet = (struct meshtastic_packet){
		.to = dest,
		.portnum = MESHTASTIC_PORT_TELEMETRY,
		.payload = payload,
		.payload_len = stream.bytes_written,
		.channel = channel,
		.want_response = false,
		.request_id = response_to_id,
	};

	return 0;
}
