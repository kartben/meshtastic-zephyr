/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

#ifndef ZEPHYR_SUBSYS_MESHTASTIC_TELEMETRY_INTERNAL_H_
#define ZEPHYR_SUBSYS_MESHTASTIC_TELEMETRY_INTERNAL_H_

#include "meshtastic_core.h"

#include "meshtastic/telemetry.pb.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Decode a port-67 Telemetry want_response probe.
 *
 * An empty payload decodes as @c Telemetry_init_zero (variant 0).
 */
bool meshtastic_telemetry_decode_request(const struct meshtastic_packet *packet,
					 meshtastic_Telemetry *request);

/**
 * @brief Encode a Telemetry protobuf into a port-67 reply packet.
 */
int meshtastic_telemetry_encode_packet(uint32_t dest, uint8_t channel, uint32_t response_to_id,
				       const meshtastic_Telemetry *telemetry, uint8_t *payload,
				       struct meshtastic_packet *packet);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_SUBSYS_MESHTASTIC_TELEMETRY_INTERNAL_H_ */
