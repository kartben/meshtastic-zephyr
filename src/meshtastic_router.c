/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

/*
 * Duplicate filtering, delivery, relay, and gateway injection paths.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "meshtastic_channels.h"
#include "meshtastic_core.h"
#include "meshtastic_modules.h"
#include "meshtastic_outbound.h"
#include "meshtastic_packet.h"

#include "meshtastic_mqtt.h"
#include "meshtastic_phoneapi.h"
#include "meshtastic_router.h"

#if defined(CONFIG_MESHTASTIC_AIRTIME)
#include "meshtastic_airtime.h"
#endif

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(meshtastic, CONFIG_MESHTASTIC_LOG_LEVEL);

static bool dup_check(uint32_t src, uint32_t id)
{
	for (int i = 0; i < CONFIG_MESHTASTIC_DUP_CACHE_SIZE; i++) {
		if (mt.dup_cache[i].src == src && mt.dup_cache[i].id == id) {
			mt.status.duplicate_packets++;
			return true;
		}
	}

	return false;
}

static void dup_add(uint32_t src, uint32_t id)
{
	mt.dup_cache[mt.dup_head].src = src;
	mt.dup_cache[mt.dup_head].id = id;
	mt.dup_head = (uint8_t)((mt.dup_head + 1U) % CONFIG_MESHTASTIC_DUP_CACHE_SIZE);
}

#if defined(CONFIG_MESHTASTIC_PACKET_HEXDUMP)
static void log_wire_rx(const uint8_t *pkt, int len, int16_t rssi, int8_t snr)
{
	const struct meshtastic_wire_header *hdr = (const struct meshtastic_wire_header *)pkt;

	LOG_DBG("LoRa RX %08x->%08x id=%08x ch=0x%02x len=%d rssi=%d snr=%d",
		(unsigned int)sys_le32_to_cpu(hdr->src), (unsigned int)sys_le32_to_cpu(hdr->dest),
		(unsigned int)sys_le32_to_cpu(hdr->id), hdr->channel, len, (int)rssi, (int)snr);
	LOG_HEXDUMP_DBG(pkt, len, "LoRa RX");
}
#endif /* CONFIG_MESHTASTIC_PACKET_HEXDUMP */

static void relay_packet(const uint8_t *buf, int len, const struct meshtastic_wire_header *hdr,
			 uint8_t hop_limit)
{
	uint8_t relay_buf[MESHTASTIC_PKT_MAX];
	struct meshtastic_wire_header *relay_hdr;
	int ret;

	if (len > (int)MESHTASTIC_PKT_MAX || hop_limit == 0U) {
		return;
	}

	memcpy(relay_buf, buf, (size_t)len);
	relay_hdr = (struct meshtastic_wire_header *)relay_buf;
	relay_hdr->flags = (hdr->flags & ~MESHTASTIC_FLAGS_HOP_LIMIT_MASK) |
			   ((hop_limit - 1U) & MESHTASTIC_FLAGS_HOP_LIMIT_MASK);

	ret = meshtastic_radio_send_wire(relay_buf, (uint32_t)len);
	if (ret < 0) {
		LOG_ERR("Relay TX failed (%d)", ret);
	} else {
		mt.status.relayed_packets++;
	}
}

static int relay_injected_encrypted_mesh_packet(const meshtastic_MeshPacket *mesh)
{
	struct meshtastic_wire_header hdr;
	uint8_t wire[MESHTASTIC_PKT_MAX];
	size_t enc_len;
	uint8_t wire_hash;

	if (mesh == NULL || mesh->which_payload_variant != meshtastic_MeshPacket_encrypted_tag) {
		return -EINVAL;
	}

	enc_len = mesh->encrypted.size;
	if (enc_len > MESHTASTIC_PAYLOAD_MAX) {
		return -EINVAL;
	}

	if (mesh->channel < MESHTASTIC_MAX_CHANNELS) {
		wire_hash = meshtastic_channels_get_hash((uint8_t)mesh->channel);
	} else {
		wire_hash = (mesh->channel != 0U) ? (uint8_t)mesh->channel : mt.ch_hash;
	}

	hdr.dest = sys_cpu_to_le32((mesh->to != 0U) ? mesh->to : MESHTASTIC_NODE_BROADCAST);
	hdr.src = sys_cpu_to_le32((mesh->from != 0U) ? mesh->from : mt.node_id);
	hdr.id = sys_cpu_to_le32((mesh->id != 0U) ? mesh->id : meshtastic_allocate_packet_id());
	hdr.flags = (mesh->hop_limit & MESHTASTIC_FLAGS_HOP_LIMIT_MASK) |
		    ((mesh->hop_start & 0x07U) << MESHTASTIC_FLAGS_HOP_START_SHIFT);
	if (mesh->want_ack) {
		hdr.flags |= MESHTASTIC_FLAGS_WANT_ACK;
	}
	if (mesh->via_mqtt) {
		hdr.flags |= MESHTASTIC_FLAGS_VIA_MQTT;
	}
	hdr.channel = wire_hash;
	hdr.next_hop = mesh->next_hop;
	hdr.relay_node = mesh->relay_node;

	memcpy(wire, &hdr, sizeof(hdr));
	memcpy(wire + MESHTASTIC_HDR_LEN, mesh->encrypted.bytes, enc_len);

	return meshtastic_radio_send_wire(wire, MESHTASTIC_HDR_LEN + (uint32_t)enc_len);
}

static void deliver_packet(const struct meshtastic_packet *packet)
{
	if (mt.recv_cb != NULL) {
		mt.recv_cb(packet->from, packet->to, packet->portnum, packet->payload,
			   packet->payload_len, packet->rssi, packet->snr);
	}

	meshtastic_emit_event(MESHTASTIC_EVENT_PACKET_RECEIVED, 0, packet);
	meshtastic_phoneapi_on_packet(packet);
}

void meshtastic_routing_sniff_rebroadcast(const struct meshtastic_wire_header *hdr,
					  const uint8_t *wire, size_t wire_len,
					  const struct meshtastic_packet *packet)
{
	uint32_t src;
	uint32_t dest;
	uint8_t hop_limit;

	if (hdr == NULL || wire == NULL || wire_len < MESHTASTIC_HDR_LEN || packet == NULL) {
		return;
	}

	src = sys_le32_to_cpu(hdr->src);
	dest = sys_le32_to_cpu(hdr->dest);
	hop_limit = hdr->flags & MESHTASTIC_FLAGS_HOP_LIMIT_MASK;

	if (!meshtastic_is_rebroadcaster()) {
		return;
	}

	if (src == mt.node_id || dest == mt.node_id) {
		return;
	}

	if (hop_limit == 0U) {
		return;
	}

	if (dest == MESHTASTIC_NODE_BROADCAST && packet->id == 0U) {
		LOG_DBG("Ignore id=0 broadcast relay");
		return;
	}

	if (meshtastic_rebroadcast_mode() ==
	    meshtastic_Config_DeviceConfig_RebroadcastMode_LOCAL_ONLY) {
		return;
	}

	relay_packet(wire, (int)wire_len, hdr, hop_limit);
}

void meshtastic_router_process_lora_rx(const uint8_t *buf, int len, int16_t rssi, int8_t snr)
{
	const struct meshtastic_wire_header *hdr;
	uint32_t src;
	uint32_t pkt_id;
	struct meshtastic_packet packet;
	uint8_t payload[MESHTASTIC_MAX_PAYLOAD_LEN];
	bool decoded = false;
	int ret;
#if defined(CONFIG_MESHTASTIC_AIRTIME)
	uint32_t airtime_ms;
#endif

	if (buf == NULL || len < (int)MESHTASTIC_HDR_LEN) {
		LOG_DBG("Packet too short (%d bytes)", len);
		return;
	}

	hdr = (const struct meshtastic_wire_header *)buf;
	src = sys_le32_to_cpu(hdr->src);
	pkt_id = sys_le32_to_cpu(hdr->id);

#if defined(CONFIG_MESHTASTIC_AIRTIME)
	airtime_ms = meshtastic_airtime_packet_ms((uint32_t)len);
#endif

#if defined(CONFIG_MESHTASTIC_PACKET_HEXDUMP)
	log_wire_rx(buf, len, rssi, snr);
#endif

	if (IS_ENABLED(CONFIG_MESHTASTIC_MQTT_IGNORE_MQTT) &&
	    ((hdr->flags & MESHTASTIC_FLAGS_VIA_MQTT) != 0U)) {
		LOG_DBG("Ignoring packet with via_mqtt set");
#if defined(CONFIG_MESHTASTIC_AIRTIME)
		meshtastic_airtime_log(MESHTASTIC_AIRTIME_RX_ALL, airtime_ms);
#endif
		return;
	}

	if (dup_check(src, pkt_id)) {
		LOG_DBG("Duplicate (src=0x%08x id=0x%08x)", src, pkt_id);
#if defined(CONFIG_MESHTASTIC_AIRTIME)
		meshtastic_airtime_log(MESHTASTIC_AIRTIME_RX_ALL, airtime_ms);
#endif
		return;
	}

	dup_add(src, pkt_id);

	ret = meshtastic_try_decode_wire_packet(buf, len, rssi, snr, &packet, payload,
						sizeof(payload), &decoded);
	if (ret < 0) {
		LOG_DBG("RX header parse failed (%d)", ret);
#if defined(CONFIG_MESHTASTIC_AIRTIME)
		meshtastic_airtime_log(MESHTASTIC_AIRTIME_RX_ALL, airtime_ms);
#endif
		return;
	}

#if defined(CONFIG_MESHTASTIC_AIRTIME)
	if (packet.from == 0U) {
		meshtastic_airtime_log(MESHTASTIC_AIRTIME_RX_ALL, airtime_ms);
	} else {
		meshtastic_airtime_log(MESHTASTIC_AIRTIME_RX, airtime_ms);
	}
#endif

	if (!decoded) {
		mt.status.decode_failures++;
	}

	mt.status.rx_packets++;
	mt.status.last_rx_from = src;
	mt.status.last_rssi = rssi;
	mt.status.last_snr = snr;

	meshtastic_handle_inbound_packet(&packet, buf, (size_t)len, decoded);
}

static void log_inject_mesh_packet(const char *phase, const meshtastic_MeshPacket *mesh)
{
	unsigned int portnum = 0U;
	const char *payload_kind = "unknown";
	size_t enc_len = 0U;

	if (mesh == NULL) {
		LOG_DBG("inject %s: (null mesh)", phase);
		return;
	}

	if (mesh->which_payload_variant == meshtastic_MeshPacket_decoded_tag) {
		payload_kind = "decoded";
		portnum = (unsigned int)mesh->decoded.portnum;
	} else if (mesh->which_payload_variant == meshtastic_MeshPacket_encrypted_tag) {
		payload_kind = "encrypted";
		enc_len = mesh->encrypted.size;
	} else if (mesh->encrypted.size > 0U) {
		payload_kind = "encrypted?";
		enc_len = mesh->encrypted.size;
	}

	LOG_DBG("inject %s: 0x%08x->0x%08x id=0x%08x port=%u %s enc=%zu hop=%u/%u ch=0x%02x "
		"via_mqtt=%d",
		phase, mesh->from, mesh->to, mesh->id, portnum, payload_kind, enc_len,
		mesh->hop_limit, mesh->hop_start, mesh->channel, mesh->via_mqtt ? 1 : 0);
}

int meshtastic_inject_downlink_mesh_packet(const meshtastic_MeshPacket *mesh)
{
	meshtastic_MeshPacket work;
	struct meshtastic_packet packet;
	uint8_t payload[MESHTASTIC_MAX_PAYLOAD_LEN];
	bool local;
	bool relay;
	bool decoded = false;
	int ret;

	if (mesh == NULL || !mt.initialized) {
		LOG_DBG("inject rejected: mesh=%p initialized=%d", (void *)mesh, mt.initialized);
		return -EINVAL;
	}

	meshtastic_mesh_packet_copy(&work, mesh);
	work.via_mqtt = true;
	work.transport_mechanism = meshtastic_MeshPacket_TransportMechanism_TRANSPORT_MQTT;

	log_inject_mesh_packet("entry", &work);

	if (dup_check(work.from, work.id)) {
		LOG_DBG("inject duplicate (src=0x%08x id=0x%08x)", work.from, work.id);
		return -EALREADY;
	}

	dup_add(work.from, work.id);

	local = (work.to == mt.node_id || work.to == MESHTASTIC_NODE_BROADCAST);
	relay = (work.to != mt.node_id &&
		 (work.hop_limit > 0U ||
		  (work.hop_limit == 0U &&
		   work.which_payload_variant == meshtastic_MeshPacket_encrypted_tag)));

	LOG_DBG("inject plan: local=%d relay=%d (node=0x%08x)", local, relay, mt.node_id);

	if (relay) {
		if (work.hop_limit == 0U &&
		    work.which_payload_variant == meshtastic_MeshPacket_encrypted_tag) {
			LOG_DBG("inject relaying terminal MQTT hop onto LoRa (hop_limit=0)");
			ret = relay_injected_encrypted_mesh_packet(&work);
		} else {
			LOG_DBG("inject relaying onto LoRa (hop_limit=%u)", work.hop_limit);
			ret = meshtastic_send_mesh_pb(&work);
		}
		if (ret < 0) {
			LOG_DBG("inject relay TX failed (%d)", ret);
			return ret;
		}

		LOG_DBG("inject relay TX ok");
	}

	if (!local) {
		LOG_DBG("inject done (relayed only, not for us)");
		return 0;
	}

	if (work.which_payload_variant == meshtastic_MeshPacket_decoded_tag) {
		decoded = true;
	} else {
		ret = meshtastic_mesh_pb_try_decode(&work);
		if (ret < 0) {
			if (ret == -ENOTSUP) {
				LOG_DBG("inject ignored: packet carries no payload (e.g. "
					"ACK/control)");
			} else {
				LOG_DBG("inject decode failed (%d), dropping local delivery", ret);
			}
			return 0;
		}

		decoded = true;
	}

	ret = meshtastic_mesh_pb_to_packet(&work, &packet, payload, sizeof(payload));
	if (ret < 0) {
		LOG_DBG("inject mesh_pb_to_packet failed (%d)", ret);
		return ret;
	}

	packet.via_mqtt = true;

	LOG_DBG("inject delivering locally port=%u payload_len=%zu", (unsigned int)packet.portnum,
		packet.payload_len);
	meshtastic_handle_inbound_packet(&packet, NULL, 0U, decoded);
	LOG_DBG("inject done (local delivery)");

	return 0;
}

void meshtastic_handle_inbound_packet(const struct meshtastic_packet *packet, const uint8_t *wire,
				      size_t wire_len, bool decoded)
{
	const struct meshtastic_wire_header *hdr = NULL;

	if (packet == NULL) {
		return;
	}

	if (wire != NULL && wire_len >= MESHTASTIC_HDR_LEN) {
		hdr = (const struct meshtastic_wire_header *)wire;
	}

	if (decoded) {
		LOG_INF("RX from 0x%08x to 0x%08x port=%u len=%zu ch_idx=%u", packet->from,
			packet->to, (unsigned int)packet->portnum, packet->payload_len,
			packet->channel_index);

		if (meshtastic_rebroadcast_mode() ==
			    meshtastic_Config_DeviceConfig_RebroadcastMode_CORE_PORTNUMS_ONLY &&
		    packet->portnum != MESHTASTIC_PORT_TEXT_MESSAGE &&
		    packet->portnum != MESHTASTIC_PORT_POSITION &&
		    packet->portnum != MESHTASTIC_PORT_NODEINFO &&
		    packet->portnum != MESHTASTIC_PORT_ROUTING &&
		    packet->portnum != MESHTASTIC_PORT_TELEMETRY) {
			LOG_DBG("CORE_PORTNUMS_ONLY: drop port %u", (unsigned int)packet->portnum);
		} else if (packet->to == mt.node_id || packet->to == MESHTASTIC_NODE_BROADCAST) {
			deliver_packet(packet);
		}

#if defined(CONFIG_MESHTASTIC_MQTT)
		meshtastic_mqtt_on_rx(packet, wire, wire_len);
#endif
		meshtastic_routing_on_decoded(packet);
		meshtastic_dispatch_modules(packet);
	} else if (hdr != NULL) {
		LOG_DBG("RX encrypted relay 0x%08x->0x%08x id=0x%08x", packet->from, packet->to,
			packet->id);
	}

	if (hdr != NULL) {
		meshtastic_routing_sniff_rebroadcast(hdr, wire, wire_len, packet);
	}
}
