/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <pb_decode.h>
#include <pb_encode.h>

#include "meshtastic_channels.h"
#include "meshtastic_modules.h"

#include <zephyr/meshtastic/meshtastic.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(meshtastic, CONFIG_MESHTASTIC_LOG_LEVEL);

struct nodeinfo_peer {
	uint32_t node;
	int64_t last_seen_ms;
	int64_t last_request_ms;
	int64_t last_reply_ms;
	bool has_user;
	bool request_time_valid;
	bool reply_time_valid;
};

static K_MUTEX_DEFINE(nodeinfo_lock);
static struct nodeinfo_peer nodeinfo_peers[CONFIG_MESHTASTIC_NODEINFO_PEER_CACHE_SIZE];

static bool interval_elapsed(bool valid, int64_t last_ms, int64_t now_ms, int64_t interval_ms)
{
	return !valid || (now_ms - last_ms) >= interval_ms;
}

static struct nodeinfo_peer *peer_get_locked(uint32_t node, int64_t now_ms)
{
	struct nodeinfo_peer *oldest = &nodeinfo_peers[0];

	for (int i = 0; i < ARRAY_SIZE(nodeinfo_peers); i++) {
		if (nodeinfo_peers[i].node == node) {
			nodeinfo_peers[i].last_seen_ms = now_ms;
			return &nodeinfo_peers[i];
		}

		if (nodeinfo_peers[i].node == 0U) {
			nodeinfo_peers[i] = (struct nodeinfo_peer){
				.node = node,
				.last_seen_ms = now_ms,
			};
			return &nodeinfo_peers[i];
		}

		if (nodeinfo_peers[i].last_seen_ms < oldest->last_seen_ms) {
			oldest = &nodeinfo_peers[i];
		}
	}

	*oldest = (struct nodeinfo_peer){
		.node = node,
		.last_seen_ms = now_ms,
	};

	return oldest;
}

static bool packet_decode_user(const struct meshtastic_packet *packet, meshtastic_User *user)
{
	pb_istream_t stream;

	if (packet->payload == NULL || packet->payload_len == 0U) {
		return false;
	}

	stream = pb_istream_from_buffer(packet->payload, packet->payload_len);
	if (!pb_decode(&stream, meshtastic_User_fields, user)) {
		const char *err = PB_GET_ERROR(&stream);

		LOG_DBG("NodeInfo decode failed: %s", err);
		return false;
	}

	return true;
}

static void log_nodeinfo_user(uint32_t from, uint32_t request_id, const meshtastic_User *user)
{
	if (request_id != 0U) {
		LOG_INF("NodeInfo reply from 0x%08x (request_id 0x%08x): id=%s long_name=%s "
			"short_name=%s hw_model=%d role=%d",
			from, request_id, user->id, user->long_name, user->short_name,
			user->hw_model, user->role);
	} else {
		LOG_INF("NodeInfo from 0x%08x: id=%s long_name=%s short_name=%s hw_model=%d "
			"role=%d",
			from, user->id, user->long_name, user->short_name, user->hw_model,
			user->role);
	}
}

static int nodeinfo_build_packet(uint32_t dest, bool want_response, uint8_t channel,
				 uint32_t response_to_id, uint8_t *payload,
				 struct meshtastic_packet *packet)
{
	meshtastic_User user;
	pb_ostream_t stream;

	meshtastic_fill_user(&user);

	stream = pb_ostream_from_buffer(payload, MESHTASTIC_MAX_PAYLOAD_LEN);
	if (!pb_encode(&stream, meshtastic_User_fields, &user)) {
		const char *err = PB_GET_ERROR(&stream);

		LOG_ERR("NodeInfo encode failed: %s", err);
		return -ENOMEM;
	}

	*packet = (struct meshtastic_packet){
		.to = dest,
		.portnum = MESHTASTIC_PORT_NODEINFO,
		.payload = payload,
		.payload_len = stream.bytes_written,
		.channel = channel,
		.want_response = want_response,
		.request_id = response_to_id,
	};

	return 0;
}

int meshtastic_send_node_info_ex(uint32_t dest, bool want_response, uint8_t channel,
				 uint32_t response_to_id)
{
	uint8_t payload[MESHTASTIC_MAX_PAYLOAD_LEN];
	struct meshtastic_packet packet;
	int ret;

	ret = nodeinfo_build_packet(dest, want_response, channel, response_to_id, payload, &packet);
	if (ret < 0) {
		return ret;
	}

	return meshtastic_send_packet(&packet, K_FOREVER);
}

int meshtastic_send_node_info(uint32_t dest)
{
	return meshtastic_send_node_info_ex(dest, false, 0U, 0U);
}

static void meshtastic_module_nodeinfo_on_packet(const struct meshtastic_packet *packet)
{
	const bool is_nodeinfo = packet != NULL && packet->portnum == MESHTASTIC_PORT_NODEINFO;
	bool should_request = false;
	bool has_user = false;
	bool log_user = false;
	uint8_t channel = 0U;
	int64_t now_ms;
	meshtastic_User user = meshtastic_User_init_zero;
	struct nodeinfo_peer *peer;

	if (packet == NULL || packet->from == 0U || packet->from == meshtastic_get_node_id()) {
		return;
	}

	if (is_nodeinfo) {
		has_user = packet_decode_user(packet, &user);
		if (!has_user && packet->request_id != 0U) {
			LOG_WRN("NodeInfo reply from 0x%08x (request_id 0x%08x) decode failed",
				packet->from, packet->request_id);
		}
	}

	now_ms = k_uptime_get();
	k_mutex_lock(&nodeinfo_lock, K_FOREVER);

	peer = peer_get_locked(packet->from, now_ms);
	if (has_user) {
		log_user = packet->request_id != 0U || !peer->has_user;
		peer->has_user = true;
	}

	if (!peer->has_user && !is_nodeinfo &&
	    interval_elapsed(peer->request_time_valid, peer->last_request_ms, now_ms,
			     (int64_t)CONFIG_MESHTASTIC_NODEINFO_UNKNOWN_SUPPRESS_SEC *
				     MSEC_PER_SEC)) {
		should_request = true;
		channel = (packet->channel_index != MESHTASTIC_CHANNEL_INDEX_INVALID)
				  ? packet->channel_index
				  : meshtastic_channels_primary_index();
		peer->request_time_valid = true;
		peer->last_request_ms = now_ms;
	}

	k_mutex_unlock(&nodeinfo_lock);

	if (log_user) {
		log_nodeinfo_user(packet->from, packet->request_id, &user);
	}

	if (should_request) {
		uint8_t payload[MESHTASTIC_MAX_PAYLOAD_LEN];
		struct meshtastic_packet nodeinfo_packet;
		int send_ret;

		LOG_INF("Heard unknown node 0x%08x, asking for NodeInfo", packet->from);
		send_ret = nodeinfo_build_packet(packet->from, true, channel, 0U, payload,
						 &nodeinfo_packet);
		if (send_ret == 0) {
			(void)meshtastic_send_packet(&nodeinfo_packet, K_NO_WAIT);
		}
	}
}

static int meshtastic_module_nodeinfo_alloc_reply(const struct meshtastic_packet *req,
						  struct meshtastic_packet *reply)
{
	static uint8_t payload[MESHTASTIC_MAX_PAYLOAD_LEN];
	int64_t now_ms;
	struct nodeinfo_peer *peer;
	int ret;

	if (req == NULL || reply == NULL || req->from == 0U ||
	    req->from == meshtastic_get_node_id()) {
		return -EINVAL;
	}

	now_ms = k_uptime_get();
	k_mutex_lock(&nodeinfo_lock, K_FOREVER);

	peer = peer_get_locked(req->from, now_ms);
	if (!interval_elapsed(peer->reply_time_valid, peer->last_reply_ms, now_ms,
			      (int64_t)CONFIG_MESHTASTIC_NODEINFO_REPLY_SUPPRESS_SEC *
				      MSEC_PER_SEC)) {
		k_mutex_unlock(&nodeinfo_lock);
		return -ENOENT;
	}

	peer->reply_time_valid = true;
	peer->last_reply_ms = now_ms;
	k_mutex_unlock(&nodeinfo_lock);

	LOG_INF("NodeInfo request from 0x%08x, sending response", req->from);

	ret = nodeinfo_build_packet(req->from, false, req->channel, req->id, payload, reply);
	if (ret < 0) {
		return ret;
	}

	return 0;
}

MESHTASTIC_MODULE_DEFINE(nodeinfo, MESHTASTIC_PORT_NODEINFO, MESHTASTIC_MODULE_ALL_PACKETS,
			 meshtastic_module_nodeinfo_on_packet,
			 meshtastic_module_nodeinfo_alloc_reply);

#if defined(CONFIG_MESHTASTIC_NODEINFO_AUTO_SEND)
static K_THREAD_STACK_DEFINE(nodeinfo_stack, 2048);
static struct k_thread nodeinfo_thread;

static void nodeinfo_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	/*
	 * Delay the first announcement so the radio/network has time to
	 * settle, mirroring the upstream NodeInfoModule start delay.
	 */
	k_sleep(K_SECONDS(CONFIG_MESHTASTIC_NODEINFO_START_DELAY_SEC));

	while (true) {
		(void)meshtastic_send_node_info(MESHTASTIC_NODE_BROADCAST);
		k_sleep(K_SECONDS(CONFIG_MESHTASTIC_NODEINFO_INTERVAL_SEC));
	}
}
#endif

int meshtastic_nodeinfo_init(void)
{
#if defined(CONFIG_MESHTASTIC_NODEINFO_AUTO_SEND)
	k_thread_create(&nodeinfo_thread, nodeinfo_stack, K_THREAD_STACK_SIZEOF(nodeinfo_stack),
			nodeinfo_thread_fn, NULL, NULL, NULL, CONFIG_MESHTASTIC_THREAD_PRIORITY, 0,
			K_NO_WAIT);
	k_thread_name_set(&nodeinfo_thread, "meshtastic_nodeinfo");
#endif

	return 0;
}
