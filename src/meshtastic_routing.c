/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

#include <errno.h>
#include <string.h>

#include <pb_encode.h>

#include "meshtastic_channels.h"
#include "meshtastic_core.h"
#include "meshtastic_packet.h"
#include "meshtastic_router.h"

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(meshtastic, CONFIG_MESHTASTIC_LOG_LEVEL);

static bool packet_is_to_us(const struct meshtastic_packet *packet)
{
	return packet != NULL &&
	       (packet->to == mt.node_id || packet->to == MESHTASTIC_NODE_BROADCAST);
}

static uint8_t routing_hop_limit_for_reply(const struct meshtastic_packet *req)
{
	if (req->hop_start > 0U && req->hop_start == req->hop_limit) {
		return 0U;
	}

	if (req->hop_start > 0U && req->hop_start > req->hop_limit &&
	    (req->hop_start - req->hop_limit + 2U) < mt.hop_limit) {
		return (uint8_t)(req->hop_start - req->hop_limit + 2U);
	}

	return mt.hop_limit;
}

static int routing_send_ack(const struct meshtastic_packet *req)
{
	meshtastic_MeshPacket mesh = meshtastic_MeshPacket_init_zero;
	meshtastic_Routing routing = meshtastic_Routing_init_zero;
	pb_ostream_t stream;
	uint8_t ch_index;

	if (req == NULL || req->to != mt.node_id) {
		return -EINVAL;
	}

	ch_index = (req->channel_index != MESHTASTIC_CHANNEL_INDEX_INVALID)
			   ? req->channel_index
			   : meshtastic_channels_primary_index();

	routing.error_reason = meshtastic_Routing_Error_NONE;
	routing.which_variant = meshtastic_Routing_error_reason_tag;

	mesh.from = mt.node_id;
	mesh.to = req->from;
	mesh.id = meshtastic_allocate_packet_id();
	mesh.channel = ch_index;
	mesh.hop_limit = routing_hop_limit_for_reply(req);
	mesh.hop_start = mesh.hop_limit;
	mesh.want_ack = false;
	mesh.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
	mesh.decoded.portnum = meshtastic_PortNum_ROUTING_APP;
	mesh.decoded.request_id = req->id;
	mesh.decoded.want_response = false;
	mesh.priority = meshtastic_MeshPacket_Priority_ACK;

	stream = pb_ostream_from_buffer(mesh.decoded.payload.bytes,
					sizeof(mesh.decoded.payload.bytes));
	if (!pb_encode(&stream, meshtastic_Routing_fields, &routing)) {
		LOG_ERR("Routing ACK encode failed: %s", PB_GET_ERROR(&stream));
		return -ENOMEM;
	}

	mesh.decoded.payload.size = (pb_size_t)stream.bytes_written;

	LOG_DBG("Sending ROUTING ACK to 0x%08x for id=0x%08x ch=%u", mesh.to, req->id, ch_index);

	return meshtastic_send_mesh_pb(&mesh);
}

void meshtastic_routing_on_decoded(const struct meshtastic_packet *packet)
{
	if (packet == NULL || !packet_is_to_us(packet) || packet->from == mt.node_id) {
		return;
	}

	if (packet->want_ack && packet->to == mt.node_id) {
		(void)routing_send_ack(packet);
	}
}

void meshtastic_routing_sniff(const struct meshtastic_wire_header *hdr, const uint8_t *wire,
			      size_t wire_len, const struct meshtastic_packet *packet, bool decoded)
{
	ARG_UNUSED(decoded);

	meshtastic_routing_sniff_rebroadcast(hdr, wire, wire_len, packet);
}
