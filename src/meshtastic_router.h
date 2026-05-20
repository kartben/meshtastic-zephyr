/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

/**
 * @file meshtastic_router.h
 * @brief Internal packet ingress, delivery, relay, and MQTT downlink injection.
 *
 * The router sits between the LoRa radio driver and higher layers. Responsibilities include
 * duplicate suppression, wire decode, local delivery, flood rebroadcast, and injecting packets
 * received from MQTT back onto the mesh.
 */

#ifndef ZEPHYR_SUBSYS_MESHTASTIC_ROUTER_H_
#define ZEPHYR_SUBSYS_MESHTASTIC_ROUTER_H_

#include <stddef.h>
#include <stdint.h>

#include "meshtastic_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Process one raw LoRa frame from the radio RX path.
 *
 * Filters duplicates, decodes the wire packet, then passes the result to
 * @ref meshtastic_handle_inbound_packet.
 */
void meshtastic_router_process_lora_rx(const uint8_t *buf, int len, int16_t rssi, int8_t snr);

/**
 * @brief Common inbound path after LoRa RX or MQTT downlink injection.
 *
 * Delivers locally and runs port/routing hooks when @p decoded is true; may flood-relay the
 * original wire frame when @p wire is non-NULL. @p wire may be @c NULL for gateway- injected
 * packets.
 */
void meshtastic_handle_inbound_packet(const struct meshtastic_packet *packet, const uint8_t *wire,
				      size_t wire_len, bool decoded);

/**
 * @brief Inject a @c MeshPacket from a gateway (e.g. MQTT) into the mesh pipeline.
 *
 * Applies duplicate filtering, optionally re-transmits onto LoRa, then delivers locally when
 * addressed to this node or broadcast.
 */
int meshtastic_inject_downlink_mesh_packet(const meshtastic_MeshPacket *mesh);

/**
 * @brief Flood-relay a foreign packet when rebroadcast policy allows.
 *
 * Re-sends the wire frame with hop limit decremented; does not require payload decode.
 */
void meshtastic_routing_sniff_rebroadcast(const struct meshtastic_wire_header *hdr,
					  const uint8_t *wire, size_t wire_len,
					  const struct meshtastic_packet *packet);

/** @brief Routing-port handling for decoded packets (e.g. ACK replies). */
void meshtastic_routing_on_decoded(const struct meshtastic_packet *packet);

/**
 * @brief Post-RX hook for relay policy.
 *
 * Currently delegates to @ref meshtastic_routing_sniff_rebroadcast.
 */
void meshtastic_routing_sniff(const struct meshtastic_wire_header *hdr, const uint8_t *wire,
			      size_t wire_len, const struct meshtastic_packet *packet,
			      bool decoded);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_SUBSYS_MESHTASTIC_ROUTER_H_ */
