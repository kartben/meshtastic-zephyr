/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

/*
 * Meshtastic protobuf, crypto, and wire-packet conversion helpers.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <psa/crypto.h>

#include <pb_decode.h>
#include <pb_encode.h>

#include "meshtastic_channels.h"
#include "meshtastic_core.h"
#include "meshtastic_outbound.h"
#include "meshtastic_packet.h"
#include "meshtastic_mqtt.h"

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(meshtastic, CONFIG_MESHTASTIC_LOG_LEVEL);

BUILD_ASSERT(sizeof(struct meshtastic_wire_header) == MESHTASTIC_HDR_LEN,
	     "Meshtastic wire header must be exactly 16 bytes");

static int ctr_crypt(const uint8_t *key, size_t key_len, const uint8_t nonce[16], const uint8_t *in,
		     uint8_t *out, size_t len)
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_cipher_operation_t op = PSA_CIPHER_OPERATION_INIT;
	psa_key_id_t kid = PSA_KEY_ID_NULL;
	psa_status_t status;
	size_t out_len;
	size_t finish_len;
	int ret = 0;

	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT);
	psa_set_key_algorithm(&attr, PSA_ALG_CTR);
	psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
	psa_set_key_bits(&attr, (uint32_t)(key_len * 8U));
	psa_set_key_lifetime(&attr, PSA_KEY_LIFETIME_VOLATILE);

	status = psa_import_key(&attr, key, key_len, &kid);
	if (status != PSA_SUCCESS) {
		LOG_ERR("psa_import_key failed (%d)", (int)status);
		return -EIO;
	}

	status = psa_cipher_encrypt_setup(&op, kid, PSA_ALG_CTR);
	if (status != PSA_SUCCESS) {
		LOG_ERR("psa_cipher_encrypt_setup failed (%d)", (int)status);
		ret = -EIO;
		goto destroy;
	}

	status = psa_cipher_set_iv(&op, nonce, 16U);
	if (status != PSA_SUCCESS) {
		LOG_ERR("psa_cipher_set_iv failed (%d)", (int)status);
		(void)psa_cipher_abort(&op);
		ret = -EIO;
		goto destroy;
	}

	status = psa_cipher_update(&op, in, len, out, len + 16U, &out_len);
	if (status != PSA_SUCCESS) {
		LOG_ERR("psa_cipher_update failed (%d)", (int)status);
		(void)psa_cipher_abort(&op);
		ret = -EIO;
		goto destroy;
	}

	status = psa_cipher_finish(&op, out + out_len, 16U, &finish_len);
	if (status != PSA_SUCCESS) {
		LOG_ERR("psa_cipher_finish failed (%d)", (int)status);
		ret = -EIO;
	}

destroy:
	(void)psa_destroy_key(kid);
	return ret;
}

static int encode_packet_data(const struct meshtastic_packet *packet, uint8_t *buf, size_t buf_len,
			      size_t *encoded_len)
{
	meshtastic_Data data = meshtastic_Data_init_zero;
	pb_ostream_t stream;

	if (packet == NULL || buf == NULL || encoded_len == NULL) {
		return -EINVAL;
	}

	if ((packet->payload == NULL && packet->payload_len != 0U) ||
	    packet->payload_len > MESHTASTIC_MAX_PAYLOAD_LEN) {
		return -EINVAL;
	}

	data.portnum = (meshtastic_PortNum)packet->portnum;
	data.payload.size = (pb_size_t)packet->payload_len;
	if (packet->payload_len > 0U) {
		memcpy(data.payload.bytes, packet->payload, packet->payload_len);
	}
	data.want_response = packet->want_response;
	data.dest = packet->data_dest;
	data.source = packet->data_source;
	data.request_id = packet->request_id;
	data.reply_id = packet->reply_id;

	stream = pb_ostream_from_buffer(buf, buf_len);
	if (!pb_encode(&stream, meshtastic_Data_fields, &data)) {
		const char *err = PB_GET_ERROR(&stream);

		LOG_ERR("Data encode failed: %s", err);
		return -ENOMEM;
	}

	*encoded_len = stream.bytes_written;
	return 0;
}

int meshtastic_encode_data(uint32_t portnum, const uint8_t *payload, size_t payload_len,
			   uint8_t *buf, size_t buf_len, size_t *encoded_len)
{
	struct meshtastic_packet packet = {
		.portnum = portnum,
		.payload = payload,
		.payload_len = payload_len,
	};

	return encode_packet_data(&packet, buf, buf_len, encoded_len);
}

int meshtastic_packet_to_mesh_pb(const struct meshtastic_packet *packet,
				 meshtastic_MeshPacket *mesh)
{
	meshtastic_Data *decoded;

	if (packet == NULL || mesh == NULL ||
	    (packet->payload == NULL && packet->payload_len != 0U) ||
	    packet->payload_len > MESHTASTIC_MAX_PAYLOAD_LEN) {
		return -EINVAL;
	}

	*mesh = (meshtastic_MeshPacket)meshtastic_MeshPacket_init_zero;
	mesh->from = packet->from;
	mesh->to = packet->to;
	mesh->channel = (packet->channel_index != MESHTASTIC_CHANNEL_INDEX_INVALID)
				? packet->channel_index
				: 0U;
	mesh->id = packet->id;
	mesh->rx_snr = (float)packet->snr;
	mesh->rx_rssi = packet->rssi;
	mesh->hop_limit = packet->hop_limit;
	mesh->hop_start = packet->hop_start;
	mesh->want_ack = packet->want_ack;
	mesh->via_mqtt = packet->via_mqtt;
	mesh->next_hop = packet->next_hop;
	mesh->relay_node = packet->relay_node;
	mesh->transport_mechanism =
		packet->via_mqtt ? meshtastic_MeshPacket_TransportMechanism_TRANSPORT_MQTT
				 : meshtastic_MeshPacket_TransportMechanism_TRANSPORT_LORA;
	mesh->which_payload_variant = meshtastic_MeshPacket_decoded_tag;

	decoded = &mesh->decoded;
	*decoded = (meshtastic_Data)meshtastic_Data_init_zero;
	decoded->portnum = (meshtastic_PortNum)packet->portnum;
	decoded->payload.size = (pb_size_t)packet->payload_len;
	if (packet->payload_len > 0U) {
		memcpy(decoded->payload.bytes, packet->payload, packet->payload_len);
	}
	decoded->want_response = packet->want_response;
	decoded->dest = packet->data_dest;
	decoded->source = packet->data_source;
	decoded->request_id = packet->request_id;
	decoded->reply_id = packet->reply_id;

	return 0;
}

void meshtastic_mesh_packet_copy(meshtastic_MeshPacket *dst, const meshtastic_MeshPacket *src)
{
	if (dst == NULL || src == NULL) {
		return;
	}

	*dst = *src;
	/*
	 * Match official firmware: copy the full decoded/encrypted union backing
	 * store so payload bytes survive even if which_payload_variant is unset.
	 */
	memcpy(&dst->decoded, &src->decoded, MAX(sizeof(dst->decoded), sizeof(dst->encrypted)));
	dst->which_payload_variant = src->which_payload_variant;
}

static int decrypt_mesh_encrypted_key(uint32_t from, uint32_t id, const uint8_t *enc,
				      size_t enc_len, const struct meshtastic_channel_key *key,
				      meshtastic_Data *data)
{
	uint8_t nonce[16];
	pb_istream_t stream;
	int ret;

	if (enc == NULL || data == NULL || enc_len == 0U || key == NULL) {
		return -EINVAL;
	}

	if (enc_len > MESHTASTIC_PAYLOAD_MAX) {
		return -EINVAL;
	}

	if (key->len == 0U) {
		return -ENOTSUP;
	}

	if (key->len != 16U && key->len != 32U) {
		return -EINVAL;
	}

	k_mutex_lock(&mt_ws.lock, K_FOREVER);

	memset(nonce, 0, sizeof(nonce));
	sys_put_le32(id, nonce);
	sys_put_le32(from, nonce + 8U);

	ret = ctr_crypt(key->bytes, key->len, nonce, enc, mt_ws.rx_dec, enc_len);
	if (ret < 0) {
		k_mutex_unlock(&mt_ws.lock);
		return ret;
	}

	stream = pb_istream_from_buffer(mt_ws.rx_dec, enc_len);
	if (!pb_decode(&stream, meshtastic_Data_fields, data)) {
		k_mutex_unlock(&mt_ws.lock);
		return -EIO;
	}

	k_mutex_unlock(&mt_ws.lock);

	if (data->portnum == meshtastic_PortNum_UNKNOWN_APP) {
		return -EIO;
	}

	return 0;
}

uint8_t meshtastic_packet_wire_hash_for_index(uint8_t channel_index)
{
	return meshtastic_channels_get_hash(channel_index);
}

static int try_decrypt_wire_hash(uint8_t wire_hash, uint32_t from, uint32_t id, const uint8_t *enc,
				 size_t enc_len, meshtastic_Data *data, uint8_t *channel_index_out)
{
	struct meshtastic_channel_key key;

	for (uint8_t ch = 0; ch < MESHTASTIC_MAX_CHANNELS; ch++) {
		const meshtastic_Channel *slot = meshtastic_channels_get(ch);
		int ret;

		if (slot == NULL || slot->role == meshtastic_Channel_Role_DISABLED) {
			continue;
		}

		if (!meshtastic_channels_decrypt_for_hash(ch, wire_hash)) {
			continue;
		}

		ret = meshtastic_channels_get_key(ch, &key);
		if (ret < 0) {
			continue;
		}

		ret = decrypt_mesh_encrypted_key(from, id, enc, enc_len, &key, data);
		if (ret == 0) {
			*channel_index_out = ch;
			return 0;
		}
	}

	return -EBADMSG;
}

int meshtastic_mesh_pb_try_decode(meshtastic_MeshPacket *mesh)
{
	meshtastic_Data data = meshtastic_Data_init_zero;
	size_t enc_len;
	int ret;

	if (mesh == NULL) {
		return -EINVAL;
	}

	if (mesh->which_payload_variant == meshtastic_MeshPacket_decoded_tag) {
		return 0;
	}

	if (mesh->which_payload_variant != meshtastic_MeshPacket_encrypted_tag) {
		if (mesh->encrypted.size > 0U) {
			mesh->which_payload_variant = meshtastic_MeshPacket_encrypted_tag;
		} else {
			return -ENOTSUP;
		}
	}

	enc_len = mesh->encrypted.size;
	if (enc_len == 0U) {
		return -EINVAL;
	}

	{
		uint8_t ch_index = MESHTASTIC_CHANNEL_INDEX_INVALID;
		uint8_t wire_hash = (mesh->channel != 0U) ? (uint8_t)mesh->channel : mt.ch_hash;

		ret = try_decrypt_wire_hash(wire_hash, mesh->from, mesh->id, mesh->encrypted.bytes,
					    enc_len, &data, &ch_index);
		if (ret < 0) {
			return ret;
		}

		mesh->decoded = data;
		mesh->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
		mesh->channel = ch_index;
	}

	return 0;
}

static int copy_data_payload(const meshtastic_Data *data, uint8_t *payload, size_t payload_len,
			     const uint8_t **packet_payload)
{
	if (data->payload.size == 0U) {
		*packet_payload = NULL;
		return 0;
	}

	if (payload == NULL || data->payload.size > payload_len) {
		return -ENOMEM;
	}

	memcpy(payload, data->payload.bytes, data->payload.size);
	*packet_payload = payload;
	return 0;
}

int meshtastic_mesh_pb_to_packet(const meshtastic_MeshPacket *mesh,
				 struct meshtastic_packet *packet, uint8_t *payload,
				 size_t payload_len)
{
	const uint8_t *packet_payload = NULL;
	int ret;

	if (mesh == NULL || packet == NULL) {
		return -EINVAL;
	}

	if (mesh->which_payload_variant == meshtastic_MeshPacket_decoded_tag) {
		ret = copy_data_payload(&mesh->decoded, payload, payload_len, &packet_payload);
		if (ret < 0) {
			return ret;
		}

		*packet = (struct meshtastic_packet){
			.from = mesh->from,
			.to = mesh->to,
			.id = mesh->id,
			.portnum = (uint32_t)mesh->decoded.portnum,
			.payload = packet_payload,
			.payload_len = mesh->decoded.payload.size,
			.data_dest = mesh->decoded.dest,
			.data_source = mesh->decoded.source,
			.request_id = mesh->decoded.request_id,
			.reply_id = mesh->decoded.reply_id,
			.hop_limit = mesh->hop_limit,
			.hop_start = mesh->hop_start,
			.channel = (mesh->channel < MESHTASTIC_MAX_CHANNELS)
					   ? meshtastic_channels_get_hash((uint8_t)mesh->channel)
					   : ((mesh->channel != 0U) ? (uint8_t)mesh->channel
								    : mt.ch_hash),
			.channel_index = (mesh->channel < MESHTASTIC_MAX_CHANNELS)
						 ? (uint8_t)mesh->channel
						 : MESHTASTIC_CHANNEL_INDEX_INVALID,
			.next_hop = mesh->next_hop,
			.relay_node = mesh->relay_node,
			.want_ack = mesh->want_ack,
			.via_mqtt = mesh->via_mqtt,
			.want_response = mesh->decoded.want_response,
			.rssi = 0,
			.snr = 0,
		};
		return 0;
	}

	{
		meshtastic_MeshPacket work;

		meshtastic_mesh_packet_copy(&work, mesh);
		ret = meshtastic_mesh_pb_try_decode(&work);
		if (ret < 0) {
			return ret;
		}

		return meshtastic_mesh_pb_to_packet(&work, packet, payload, payload_len);
	}
}

static void fill_packet_from_header(const struct meshtastic_wire_header *hdr, int16_t rssi,
				    int8_t snr, struct meshtastic_packet *packet)
{
	packet->from = sys_le32_to_cpu(hdr->src);
	packet->to = sys_le32_to_cpu(hdr->dest);
	packet->id = sys_le32_to_cpu(hdr->id);
	packet->hop_limit = hdr->flags & MESHTASTIC_FLAGS_HOP_LIMIT_MASK;
	packet->hop_start =
		(hdr->flags & MESHTASTIC_FLAGS_HOP_START_MASK) >> MESHTASTIC_FLAGS_HOP_START_SHIFT;
	packet->channel = hdr->channel;
	packet->channel_index = MESHTASTIC_CHANNEL_INDEX_INVALID;
	packet->next_hop = hdr->next_hop;
	packet->relay_node = hdr->relay_node;
	packet->want_ack = (hdr->flags & MESHTASTIC_FLAGS_WANT_ACK) != 0U;
	packet->via_mqtt = (hdr->flags & MESHTASTIC_FLAGS_VIA_MQTT) != 0U;
	packet->rssi = rssi;
	packet->snr = snr;
}

int meshtastic_try_decode_wire_packet(const uint8_t *buf, int len, int16_t rssi, int8_t snr,
				      struct meshtastic_packet *packet, uint8_t *payload,
				      size_t payload_len, bool *decoded)
{
	const struct meshtastic_wire_header *hdr;
	meshtastic_Data data = meshtastic_Data_init_zero;
	const uint8_t *packet_payload = NULL;
	uint8_t channel_index = MESHTASTIC_CHANNEL_INDEX_INVALID;
	int ret;

	if (decoded != NULL) {
		*decoded = false;
	}

	if (buf == NULL || packet == NULL || len < (int)MESHTASTIC_HDR_LEN) {
		return -EINVAL;
	}

	hdr = (const struct meshtastic_wire_header *)buf;
	fill_packet_from_header(hdr, rssi, snr, packet);
	packet->payload = NULL;
	packet->payload_len = 0U;

	if (!meshtastic_decode_known_only(packet->from)) {
		return 0;
	}

	ret = try_decrypt_wire_hash(hdr->channel, packet->from, packet->id,
				    buf + MESHTASTIC_HDR_LEN,
				    (size_t)(len - (int)MESHTASTIC_HDR_LEN), &data, &channel_index);
	if (ret < 0) {
		return 0;
	}

	ret = copy_data_payload(&data, payload, payload_len, &packet_payload);
	if (ret < 0) {
		return ret;
	}

	packet->portnum = (uint32_t)data.portnum;
	packet->payload = packet_payload;
	packet->payload_len = data.payload.size;
	packet->data_dest = data.dest;
	packet->data_source = data.source;
	packet->request_id = data.request_id;
	packet->reply_id = data.reply_id;
	packet->want_response = data.want_response;
	packet->channel_index = channel_index;

	if (decoded != NULL) {
		*decoded = true;
	}

	return 0;
}

int meshtastic_decode_wire_packet(const uint8_t *buf, int len, int16_t rssi, int8_t snr,
				  struct meshtastic_packet *packet, uint8_t *payload,
				  size_t payload_len)
{
	bool decoded = false;
	int ret;

	ret = meshtastic_try_decode_wire_packet(buf, len, rssi, snr, packet, payload, payload_len,
						&decoded);
	if (ret < 0) {
		return ret;
	}

	if (!decoded) {
		return -EBADMSG;
	}

	return 0;
}

int meshtastic_build_wire_packet(const struct meshtastic_packet *packet, uint8_t *out,
				 uint32_t *out_len)
{
	uint8_t nonce[16];
	struct meshtastic_wire_header *hdr;
	struct meshtastic_channel_key key;
	uint8_t ch_index;
	uint8_t wire_hash;
	size_t encoded_len;
	int ret;

	ret = encode_packet_data(packet, mt_ws.pb_buf, sizeof(mt_ws.pb_buf), &encoded_len);
	if (ret < 0) {
		return ret;
	}

	ch_index = meshtastic_channels_resolve_send_index(packet->to, packet->channel_index,
							  packet->channel);
	ret = meshtastic_channels_get_key(ch_index, &key);
	if (ret < 0) {
		return ret;
	}

	wire_hash = meshtastic_channels_get_hash(ch_index);

	memset(nonce, 0, sizeof(nonce));
	sys_put_le32(packet->id, nonce);
	sys_put_le32(packet->from, nonce + 8U);

	ret = ctr_crypt(key.bytes, key.len, nonce, mt_ws.pb_buf, mt_ws.enc_buf, encoded_len);
	if (ret < 0) {
		return ret;
	}

	hdr = (struct meshtastic_wire_header *)out;
	hdr->dest = sys_cpu_to_le32(packet->to);
	hdr->src = sys_cpu_to_le32(packet->from);
	hdr->id = sys_cpu_to_le32(packet->id);
	hdr->flags = (packet->hop_limit & MESHTASTIC_FLAGS_HOP_LIMIT_MASK) |
		     ((packet->hop_start & 0x07U) << MESHTASTIC_FLAGS_HOP_START_SHIFT);
	if (packet->want_ack) {
		hdr->flags |= MESHTASTIC_FLAGS_WANT_ACK;
	}
	if (packet->via_mqtt) {
		hdr->flags |= MESHTASTIC_FLAGS_VIA_MQTT;
	}
	hdr->channel = wire_hash;
	hdr->next_hop = packet->next_hop;
	hdr->relay_node = packet->relay_node;

	memcpy(out + MESHTASTIC_HDR_LEN, mt_ws.enc_buf, encoded_len);
	*out_len = (uint32_t)(MESHTASTIC_HDR_LEN + encoded_len);

	return 0;
}

int meshtastic_send_mesh_pb(const meshtastic_MeshPacket *mesh)
{
	struct meshtastic_packet packet;
	uint32_t encrypted_len;
	struct meshtastic_wire_header *hdr;
	uint8_t hop_limit;
	uint8_t hop_start;
	int ret;

	if (mesh == NULL) {
		return -EINVAL;
	}

	if (mesh->which_payload_variant == meshtastic_MeshPacket_decoded_tag) {
		packet = (struct meshtastic_packet){
			.from = mesh->from,
			.to = (mesh->to != 0U) ? mesh->to : MESHTASTIC_NODE_BROADCAST,
			.id = mesh->id,
			.portnum = (uint32_t)mesh->decoded.portnum,
			.payload = mesh->decoded.payload.bytes,
			.payload_len = mesh->decoded.payload.size,
			.data_dest = mesh->decoded.dest,
			.data_source = mesh->decoded.source,
			.request_id = mesh->decoded.request_id,
			.reply_id = mesh->decoded.reply_id,
			.hop_limit = mesh->hop_limit,
			.hop_start = mesh->hop_start,
			.channel = 0U,
			.channel_index = (mesh->channel < MESHTASTIC_MAX_CHANNELS)
						 ? (uint8_t)mesh->channel
						 : MESHTASTIC_CHANNEL_INDEX_INVALID,
			.want_ack = mesh->want_ack,
			.via_mqtt = mesh->via_mqtt,
			.want_response = mesh->decoded.want_response,
			.next_hop = mesh->next_hop,
			.relay_node = mesh->relay_node,
		};

		return meshtastic_send_packet(&packet, K_FOREVER);
	}

	if (mesh->which_payload_variant != meshtastic_MeshPacket_encrypted_tag) {
		return -ENOTSUP;
	}

	encrypted_len = mesh->encrypted.size;
	if (encrypted_len > MESHTASTIC_PAYLOAD_MAX) {
		return -EINVAL;
	}

	hop_limit = (mesh->hop_limit == 0U) ? mt.hop_limit : mesh->hop_limit;
	hop_start = (mesh->hop_start == 0U) ? hop_limit : mesh->hop_start;

	k_mutex_lock(&mt_ws.lock, K_FOREVER);

	hdr = (struct meshtastic_wire_header *)mt_ws.wire;
	hdr->dest = sys_cpu_to_le32((mesh->to != 0U) ? mesh->to : MESHTASTIC_NODE_BROADCAST);
	hdr->src = sys_cpu_to_le32((mesh->from != 0U) ? mesh->from : mt.node_id);
	hdr->id = sys_cpu_to_le32((mesh->id != 0U) ? mesh->id : meshtastic_allocate_packet_id());
	hdr->flags = (hop_limit & MESHTASTIC_FLAGS_HOP_LIMIT_MASK) |
		     ((hop_start & 0x07U) << MESHTASTIC_FLAGS_HOP_START_SHIFT);
	if (mesh->want_ack) {
		hdr->flags |= MESHTASTIC_FLAGS_WANT_ACK;
	}
	if (mesh->via_mqtt) {
		hdr->flags |= MESHTASTIC_FLAGS_VIA_MQTT;
	}
	if (mesh->channel < MESHTASTIC_MAX_CHANNELS) {
		hdr->channel = meshtastic_channels_get_hash((uint8_t)mesh->channel);
	} else {
		hdr->channel = (mesh->channel != 0U) ? (uint8_t)mesh->channel : mt.ch_hash;
	}
	hdr->next_hop = mesh->next_hop;
	hdr->relay_node = mesh->relay_node;
	memcpy(mt_ws.wire + MESHTASTIC_HDR_LEN, mesh->encrypted.bytes, encrypted_len);

	{
		uint8_t wire[MESHTASTIC_PKT_MAX];
		const uint32_t wire_len = MESHTASTIC_HDR_LEN + encrypted_len;

		memcpy(wire, mt_ws.wire, wire_len);
		k_mutex_unlock(&mt_ws.lock);

		ret = meshtastic_radio_send_wire_wait(wire, wire_len, K_FOREVER);

		if (ret == 0) {
#if defined(CONFIG_MESHTASTIC_MQTT)
			struct meshtastic_packet tx_packet = {
				.from = (mesh->from != 0U) ? mesh->from : mt.node_id,
				.to = (mesh->to != 0U) ? mesh->to : MESHTASTIC_NODE_BROADCAST,
				.id = sys_le32_to_cpu(hdr->id),
				.hop_limit = hop_limit,
				.hop_start = hop_start,
				.channel = hdr->channel,
				.want_ack = mesh->want_ack,
				.via_mqtt = mesh->via_mqtt,
				.next_hop = mesh->next_hop,
				.relay_node = mesh->relay_node,
			};

			meshtastic_mqtt_on_tx(&tx_packet, wire, wire_len);
#endif
		}
	}

	return ret;
}
