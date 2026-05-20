/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

/*
 * Meshtastic MQTT gateway bridge (native broker client).
 *
 * Publishes and subscribes to ServiceEnvelope protobuf payloads on topics
 * compatible with official Meshtastic firmware (msh/.../2/e/...).
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/version.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/socket.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <pb_decode.h>
#include <pb_encode.h>

#include "meshtastic_channels.h"
#include "meshtastic_gnss.h"
#include "meshtastic_mqtt.h"
#include "meshtastic_packet.h"
#include "meshtastic_router.h"
#include "meshtastic/config.pb.h"
#include "meshtastic/mqtt.pb.h"

LOG_MODULE_REGISTER(meshtastic_mqtt, CONFIG_MESHTASTIC_LOG_LEVEL);

#define MQTT_DEFAULT_BROKER            "mqtt.meshtastic.org"
#define CRYPT_TOPIC_SUFFIX             "/2/e/"
#define MAP_TOPIC_SUFFIX               "/2/map/"
#define MQTT_NET_WAIT_MS               2000
#define MQTT_MAP_WARN_MS               15000
#define MQTT_WIRE_MAX                  255U
/* Limit broker publishes per MQTT thread loop to avoid TCP pkt pool exhaustion. */
#define MQTT_PUBLISH_BUDGET_PER_LOOP   2
#define MQTT_PUBLISH_BUDGET_ON_CONNECT 8

struct mqtt_pub_entry {
	char topic[128];
	uint8_t payload[CONFIG_MESHTASTIC_MQTT_TX_BUFFER_SIZE];
	uint16_t len;
};

static struct {
	struct mqtt_client client;
	struct sockaddr_storage broker;
	uint8_t rx_buf[CONFIG_MESHTASTIC_MQTT_RX_BUFFER_SIZE];
	uint8_t tx_buf[CONFIG_MESHTASTIC_MQTT_TX_BUFFER_SIZE];
	uint8_t client_id[16];
	struct mqtt_utf8 username;
	struct mqtt_utf8 password;
	char sub_topic[128];
	char crypt_prefix[64];
	char map_topic[80];
	int64_t last_map_report_ms;
	int64_t last_map_no_position_ms;
	struct k_mutex lock;
	struct mqtt_pub_entry queue[CONFIG_MESHTASTIC_MQTT_PUBLISH_QUEUE_SIZE];
	uint8_t queue_head;
	uint8_t queue_tail;
	uint8_t queue_count;
	bool connected;
	bool thread_running;
	bool started;
	bool disconnect_pending;
} mqtt_ctx;

static struct net_mgmt_event_callback mqtt_net_mgmt_cb;
static bool mqtt_net_has_ipv4;

static struct k_thread mqtt_thread;
static K_THREAD_STACK_DEFINE(mqtt_stack, CONFIG_MESHTASTIC_MQTT_THREAD_STACK_SIZE);

/* Wake the MQTT thread when uplink or publish queues gain work (max 1). */
static K_SEM_DEFINE(mqtt_work_sem, 0, 1);

static struct zsock_pollfd mqtt_fds[1];
static int mqtt_nfds;

static void mqtt_work_notify(void)
{
	(void)k_sem_give(&mqtt_work_sem);
}

static void mqtt_drain_queue(int max_publish);

static void mqtt_net_event_handler(struct net_mgmt_event_callback *cb, uint64_t mgmt_event,
				   struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);

	if (mgmt_event == NET_EVENT_IPV4_ADDR_ADD) {
		mqtt_net_has_ipv4 = true;
		LOG_INF("Network ready for MQTT");
	} else if (mgmt_event == NET_EVENT_IPV4_ADDR_DEL) {
		mqtt_net_has_ipv4 = false;
		mqtt_ctx.disconnect_pending = true;
		LOG_INF("Network lost, MQTT will disconnect");
	}
}

static bool mqtt_network_is_ready(void)
{
	struct net_if *iface = net_if_get_default();

	if (!mqtt_net_has_ipv4 || iface == NULL || !net_if_is_up(iface)) {
		return false;
	}

	return net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED) != NULL;
}

static void mqtt_prepare_fds(void)
{
	if (mqtt_ctx.client.transport.type == MQTT_TRANSPORT_NON_SECURE) {
		mqtt_fds[0].fd = mqtt_ctx.client.transport.tcp.sock;
	} else {
		mqtt_fds[0].fd = -1;
	}

	mqtt_fds[0].events = ZSOCK_POLLIN;
	mqtt_nfds = (mqtt_fds[0].fd >= 0) ? 1 : 0;
}

static void mqtt_clear_fds(void)
{
	mqtt_nfds = 0;
}

static void mqtt_handle_disconnect_pending(void)
{
	if (!mqtt_ctx.disconnect_pending) {
		return;
	}

	mqtt_ctx.disconnect_pending = false;

	if (!mqtt_ctx.connected) {
		return;
	}

	LOG_DBG("Disconnecting MQTT (network lost or pending)");
	(void)mqtt_disconnect(&mqtt_ctx.client, NULL);
	mqtt_ctx.connected = false;
	mqtt_clear_fds();
}

static int mqtt_poll_socket(int timeout_ms)
{
	if (mqtt_nfds <= 0) {
		return 0;
	}

	return zsock_poll(mqtt_fds, mqtt_nfds, timeout_ms);
}

static void mqtt_node_id_str(char *buf, size_t len)
{
	snprintk(buf, len, "!%08x", meshtastic_get_node_id());
}

static bool mqtt_is_default_broker(void)
{
	return strcmp(CONFIG_MESHTASTIC_MQTT_BROKER_HOST, MQTT_DEFAULT_BROKER) == 0;
}

static bool mqtt_should_skip_portnum(uint32_t portnum, bool from_us)
{
	if (from_us || !mqtt_is_default_broker()) {
		return false;
	}

	return portnum == meshtastic_PortNum_RANGE_TEST_APP ||
	       portnum == meshtastic_PortNum_DETECTION_SENSOR_APP;
}

static int mqtt_build_publish_topic(char *topic, size_t topic_len)
{
	char gateway_id[12];
	const char *channel = meshtastic_runtime_channel_name();

	if (channel == NULL) {
		return -EINVAL;
	}

	mqtt_node_id_str(gateway_id, sizeof(gateway_id));

	return snprintk(topic, topic_len, "%s%s%s/%s", mqtt_ctx.crypt_prefix, CRYPT_TOPIC_SUFFIX,
			channel, gateway_id);
}

static int mqtt_build_map_topic(char *topic, size_t topic_len)
{
	return snprintk(topic, topic_len, "%s%s", mqtt_ctx.crypt_prefix, MAP_TOPIC_SUFFIX);
}

static int mqtt_build_subscribe_topic(char *topic, size_t topic_len)
{
	const char *channel = meshtastic_runtime_channel_name();

	if (channel == NULL) {
		return -EINVAL;
	}

	return snprintk(topic, topic_len, "%s%s%s/+", mqtt_ctx.crypt_prefix, CRYPT_TOPIC_SUFFIX,
			channel);
}

static int mqtt_mesh_from_wire(const uint8_t *wire, size_t wire_len, meshtastic_MeshPacket *mesh)
{
	const struct __packed mt_wire_hdr {
		uint32_t dest;
		uint32_t src;
		uint32_t id;
		uint8_t flags;
		uint8_t channel;
		uint8_t next_hop;
		uint8_t relay_node;
	} *hdr;

	if (wire_len < sizeof(*hdr)) {
		return -EINVAL;
	}

	hdr = (const void *)wire;

	*mesh = (meshtastic_MeshPacket)meshtastic_MeshPacket_init_zero;
	mesh->which_payload_variant = meshtastic_MeshPacket_encrypted_tag;
	mesh->from = sys_le32_to_cpu(hdr->src);
	mesh->to = sys_le32_to_cpu(hdr->dest);
	mesh->id = sys_le32_to_cpu(hdr->id);
	mesh->hop_limit = hdr->flags & 0x07U;
	mesh->hop_start = (hdr->flags >> 5) & 0x07U;
	mesh->want_ack = (hdr->flags & BIT(3)) != 0U;
	mesh->via_mqtt = (hdr->flags & BIT(4)) != 0U;
	mesh->channel = hdr->channel;
	mesh->next_hop = hdr->next_hop;
	mesh->relay_node = hdr->relay_node;
	mesh->transport_mechanism = meshtastic_MeshPacket_TransportMechanism_TRANSPORT_LORA;

	mesh->encrypted.size = (pb_size_t)(wire_len - sizeof(*hdr));
	if (mesh->encrypted.size > sizeof(mesh->encrypted.bytes)) {
		return -ENOMEM;
	}

	memcpy(mesh->encrypted.bytes, wire + sizeof(*hdr), mesh->encrypted.size);

	return 0;
}

static int mqtt_encode_envelope(const meshtastic_MeshPacket *mesh, const char *channel_id,
				const char *gateway_id, uint8_t *out, size_t out_len,
				size_t *encoded_len)
{
	meshtastic_ServiceEnvelope env = meshtastic_ServiceEnvelope_init_zero;
	pb_ostream_t stream;

	env.packet = (meshtastic_MeshPacket *)mesh;
	env.channel_id = (char *)channel_id;
	env.gateway_id = (char *)gateway_id;

	stream = pb_ostream_from_buffer(out, out_len);
	if (!pb_encode(&stream, meshtastic_ServiceEnvelope_fields, &env)) {
		LOG_ERR("ServiceEnvelope encode failed: %s", PB_GET_ERROR(&stream));
		return -ENOMEM;
	}

	*encoded_len = stream.bytes_written;
	return 0;
}

static void mqtt_queue_publish(const char *topic, const uint8_t *payload, size_t len)
{
	struct mqtt_pub_entry entry;

	if (len > sizeof(entry.payload)) {
		LOG_WRN("MQTT publish too large (%zu), dropped", len);
		return;
	}

	strncpy(entry.topic, topic, sizeof(entry.topic) - 1U);
	entry.topic[sizeof(entry.topic) - 1U] = '\0';
	memcpy(entry.payload, payload, len);
	entry.len = (uint16_t)len;

	k_mutex_lock(&mqtt_ctx.lock, K_FOREVER);

	if (mqtt_ctx.queue_count >= CONFIG_MESHTASTIC_MQTT_PUBLISH_QUEUE_SIZE) {
		LOG_WRN("MQTT publish queue full, dropping oldest");
		mqtt_ctx.queue_head =
			(mqtt_ctx.queue_head + 1U) % CONFIG_MESHTASTIC_MQTT_PUBLISH_QUEUE_SIZE;
		mqtt_ctx.queue_count--;
	}

	mqtt_ctx.queue[mqtt_ctx.queue_tail] = entry;
	mqtt_ctx.queue_tail =
		(mqtt_ctx.queue_tail + 1U) % CONFIG_MESHTASTIC_MQTT_PUBLISH_QUEUE_SIZE;
	mqtt_ctx.queue_count++;

	LOG_DBG("MQTT publish queued: %s (%u bytes, depth %u)", entry.topic, entry.len,
		mqtt_ctx.queue_count);

	k_mutex_unlock(&mqtt_ctx.lock);
	mqtt_work_notify();
}

static int mqtt_do_publish(const char *topic, const uint8_t *payload, size_t len)
{
	struct mqtt_publish_param param = {0};
	struct mqtt_topic pub_topic;
	int ret;

	if (!mqtt_ctx.connected) {
		return -ENOTCONN;
	}

	pub_topic.topic.utf8 = (uint8_t *)topic;
	pub_topic.topic.size = strlen(topic);
	pub_topic.qos = MQTT_QOS_0_AT_MOST_ONCE;

	param.message.topic = pub_topic;
	param.message.payload.data = (uint8_t *)payload;
	param.message.payload.len = len;
	param.message_id = sys_rand16_get();
	param.dup_flag = 0U;
	param.retain_flag = 0U;

	ret = mqtt_publish(&mqtt_ctx.client, &param);
	if (ret != 0) {
		LOG_WRN("mqtt_publish failed (%d)", ret);
	} else {
		LOG_DBG("MQTT published %s (%zu bytes)", topic, len);
	}

	return ret;
}

static void mqtt_drain_queue(int max_publish)
{
	struct mqtt_pub_entry entry;
	int published = 0;

	if (max_publish <= 0) {
		return;
	}

	while (mqtt_ctx.connected && published < max_publish) {
		k_mutex_lock(&mqtt_ctx.lock, K_FOREVER);
		if (mqtt_ctx.queue_count == 0U) {
			k_mutex_unlock(&mqtt_ctx.lock);
			break;
		}

		entry = mqtt_ctx.queue[mqtt_ctx.queue_head];
		mqtt_ctx.queue_head =
			(mqtt_ctx.queue_head + 1U) % CONFIG_MESHTASTIC_MQTT_PUBLISH_QUEUE_SIZE;
		mqtt_ctx.queue_count--;
		k_mutex_unlock(&mqtt_ctx.lock);

		LOG_DBG("Flushing queued MQTT publish: %s (%u bytes)", entry.topic, entry.len);
		(void)mqtt_do_publish(entry.topic, entry.payload, entry.len);
		published++;

		/*
		 * Sleep briefly between publishes to let the TCP/IP stack
		 * process packets and avoid TCP buffer exhaustion.
		 */
		k_msleep(20);
	}
}

#if IS_ENABLED(CONFIG_MESHTASTIC_MQTT_MAP_REPORT)

static meshtastic_Config_LoRaConfig_RegionCode mqtt_lora_region(void)
{
	uint32_t hz = meshtastic_runtime_frequency();

	if (hz >= 902000000U && hz <= 928000000U) {
		return meshtastic_Config_LoRaConfig_RegionCode_US;
	}
	if (hz >= 869000000U && hz <= 870000000U) {
		return meshtastic_Config_LoRaConfig_RegionCode_EU_868;
	}
	if (hz >= 433000000U && hz <= 434000000U) {
		return meshtastic_Config_LoRaConfig_RegionCode_EU_433;
	}

	return meshtastic_Config_LoRaConfig_RegionCode_UNSET;
}

static bool mqtt_has_default_channel(void)
{
	const meshtastic_Channel *ch;
	struct meshtastic_channel_key key;

	ch = meshtastic_channels_get(meshtastic_channels_primary_index());
	if (ch == NULL || ch->role == meshtastic_Channel_Role_DISABLED) {
		return false;
	}

	if (strcmp(meshtastic_channels_get_name(meshtastic_channels_primary_index()),
		   MESHTASTIC_CHANNEL_LONGFAST) != 0) {
		return false;
	}

	if (meshtastic_channels_primary_key(&key) < 0) {
		return false;
	}

	return (key.len == sizeof(meshtastic_default_psk)) &&
	       (memcmp(key.bytes, meshtastic_default_psk, key.len) == 0);
}

static uint32_t mqtt_map_position_precision(void)
{
	uint32_t precision = CONFIG_MESHTASTIC_MQTT_MAP_REPORT_POSITION_PRECISION;

	if (precision < 12U || precision > 15U) {
		precision = 14U;
	}

	return precision;
}

static void mqtt_apply_map_position_precision(int32_t *latitude_i, int32_t *longitude_i,
					      uint32_t precision)
{
	uint32_t mask;

	if (precision >= 32U) {
		return;
	}

	mask = UINT32_MAX << (32U - precision);
	*latitude_i &= (int32_t)mask;
	*longitude_i &= (int32_t)mask;
	*latitude_i += (int32_t)(1U << (31U - precision));
	*longitude_i += (int32_t)(1U << (31U - precision));
}

static void mqtt_build_map_report(meshtastic_MapReport *report, const meshtastic_Position *pos)
{
	const char *long_name = meshtastic_long_name();
	const char *short_name = meshtastic_short_name();
	uint32_t precision = mqtt_map_position_precision();

	*report = (meshtastic_MapReport)meshtastic_MapReport_init_zero;

	if (long_name != NULL) {
		strncpy(report->long_name, long_name, sizeof(report->long_name) - 1U);
	}
	if (short_name != NULL) {
		strncpy(report->short_name, short_name, sizeof(report->short_name) - 1U);
	}

	report->role = meshtastic_device_role();
	report->hw_model = meshtastic_hw_model();
	// TODO report a more sensible firmware version than Zephyr version :)
	strncpy(report->firmware_version, KERNEL_VERSION_STRING,
		sizeof(report->firmware_version) - 1U);
	report->region = mqtt_lora_region();
	report->modem_preset = meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST;
	report->has_default_channel = mqtt_has_default_channel();
	report->has_opted_report_location = true;
	report->position_precision = precision;
	report->num_online_local_nodes = 0U;

	if (pos != NULL && pos->has_latitude_i && pos->has_longitude_i) {
		report->latitude_i = pos->latitude_i;
		report->longitude_i = pos->longitude_i;
		mqtt_apply_map_position_precision(&report->latitude_i, &report->longitude_i,
						  precision);
		if (pos->has_altitude) {
			report->altitude = pos->altitude;
		}
	}
}

static void mqtt_perhaps_report_to_map(void)
{
	meshtastic_Position position = meshtastic_Position_init_zero;
	meshtastic_MapReport map_report = meshtastic_MapReport_init_zero;
	meshtastic_MeshPacket mesh = meshtastic_MeshPacket_init_zero;
	meshtastic_Data *decoded;
	uint8_t env_buf[CONFIG_MESHTASTIC_MQTT_TX_BUFFER_SIZE];
	char gateway_id[12];
	const char *channel_id = meshtastic_runtime_channel_name();
	size_t env_len = 0U;
	int64_t now = k_uptime_get();
	int64_t interval_ms =
		(int64_t)CONFIG_MESHTASTIC_MQTT_MAP_REPORT_INTERVAL_SEC * MSEC_PER_SEC;

	if (!mqtt_ctx.connected || channel_id == NULL) {
		return;
	}

	if (mqtt_ctx.last_map_report_ms != 0 && (now - mqtt_ctx.last_map_report_ms) < interval_ms) {
		return;
	}

#if defined(CONFIG_MESHTASTIC_GNSS)
	int ret;

	ret = meshtastic_gnss_get_last_position(&position);
	if (ret < 0) {
		if (mqtt_ctx.last_map_no_position_ms == 0 ||
		    (now - mqtt_ctx.last_map_no_position_ms) >= MQTT_MAP_WARN_MS) {
			LOG_WRN("MapReport enabled but no GNSS position (%d)", ret);
			mqtt_ctx.last_map_no_position_ms = now;
		}
		return;
	}
#else
	if (mqtt_ctx.last_map_no_position_ms == 0 ||
	    (now - mqtt_ctx.last_map_no_position_ms) >= MQTT_MAP_WARN_MS) {
		LOG_WRN("MapReport enabled but GNSS support is not compiled in");
		mqtt_ctx.last_map_no_position_ms = now;
	}
	return;
#endif

	if (!position.has_latitude_i || !position.has_longitude_i ||
	    (position.latitude_i == 0 && position.longitude_i == 0)) {
		if (mqtt_ctx.last_map_no_position_ms == 0 ||
		    (now - mqtt_ctx.last_map_no_position_ms) >= MQTT_MAP_WARN_MS) {
			LOG_WRN("MapReport enabled but position is zero");
			mqtt_ctx.last_map_no_position_ms = now;
		}
		return;
	}

	mqtt_build_map_report(&map_report, &position);

	mesh.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
	mesh.from = meshtastic_get_node_id();
	mesh.to = MESHTASTIC_NODE_BROADCAST;
	mesh.id = (uint32_t)now;
	mesh.want_ack = false;
	mesh.hop_limit = meshtastic_runtime_hop_limit();
	mesh.hop_start = mesh.hop_limit;
	mesh.channel = 0U;

	decoded = &mesh.decoded;
	decoded->portnum = meshtastic_PortNum_MAP_REPORT_APP;
	{
		pb_ostream_t stream = pb_ostream_from_buffer(decoded->payload.bytes,
							     sizeof(decoded->payload.bytes));

		if (!pb_encode(&stream, meshtastic_MapReport_fields, &map_report)) {
			LOG_ERR("MapReport encode failed: %s", PB_GET_ERROR(&stream));
			return;
		}
		decoded->payload.size = (pb_size_t)stream.bytes_written;
	}

	mqtt_node_id_str(gateway_id, sizeof(gateway_id));

	ret = mqtt_encode_envelope(&mesh, channel_id, gateway_id, env_buf, sizeof(env_buf),
				   &env_len);
	if (ret < 0) {
		return;
	}

	ret = mqtt_do_publish(mqtt_ctx.map_topic, env_buf, env_len);
	if (ret != 0) {
		mqtt_queue_publish(mqtt_ctx.map_topic, env_buf, env_len);
	}

	mqtt_ctx.last_map_report_ms = now;
	LOG_INF("MapReport published to %s (%u bytes)", mqtt_ctx.map_topic, (unsigned int)env_len);
}

#else

static void mqtt_perhaps_report_to_map(void)
{
}

#endif /* CONFIG_MESHTASTIC_MQTT_MAP_REPORT */

#if IS_ENABLED(CONFIG_MESHTASTIC_MQTT_UPLINK_ENABLED)
static void mqtt_queue_uplink(const struct meshtastic_packet *packet, const uint8_t *wire,
			      size_t wire_len)
{
	uint8_t ch_index = meshtastic_channels_primary_index();

	if (packet == NULL || wire == NULL || wire_len == 0U) {
		return;
	}

	if (packet->channel_index != MESHTASTIC_CHANNEL_INDEX_INVALID) {
		ch_index = packet->channel_index;
	}

	if (!meshtastic_channels_uplink_enabled(ch_index)) {
		LOG_DBG("MQTT uplink disabled for channel %u", ch_index);
		return;
	}

	bool from_us = (packet->from == meshtastic_get_node_id());

	if (packet->via_mqtt) {
		LOG_DBG("Skipping MQTT uplink 0x%08x->0x%08x id=0x%08x (via_mqtt set)",
			packet->from, packet->to, packet->id);
		return;
	}

	if (mqtt_should_skip_portnum(packet->portnum, from_us)) {
		LOG_DBG("Skipping MQTT uplink port=%u on default broker", packet->portnum);
		return;
	}

	char topic[128];
	int ret = mqtt_build_publish_topic(topic, sizeof(topic));
	if (ret < 0 || ret >= (int)sizeof(topic)) {
		LOG_DBG("MQTT uplink topic build failed (%d)", ret);
		return;
	}

	meshtastic_MeshPacket mesh = meshtastic_MeshPacket_init_zero;
	if (IS_ENABLED(CONFIG_MESHTASTIC_MQTT_ENCRYPTION_ENABLED)) {
		ret = mqtt_mesh_from_wire(wire, wire_len, &mesh);
		if (ret < 0) {
			LOG_DBG("MQTT uplink wire parse failed (%d) len=%zu", ret, wire_len);
			return;
		}
	} else {
		ret = meshtastic_packet_to_mesh_pb(packet, &mesh);
		if (ret < 0) {
			LOG_DBG("MQTT uplink mesh encode failed (%d)", ret);
			return;
		}
	}

	const char *channel_id = meshtastic_runtime_channel_name();
	char gateway_id[12];
	mqtt_node_id_str(gateway_id, sizeof(gateway_id));

	k_mutex_lock(&mqtt_ctx.lock, K_FOREVER);

	if (mqtt_ctx.queue_count >= CONFIG_MESHTASTIC_MQTT_PUBLISH_QUEUE_SIZE) {
		LOG_WRN("MQTT publish queue full, dropping oldest");
		mqtt_ctx.queue_head =
			(mqtt_ctx.queue_head + 1U) % CONFIG_MESHTASTIC_MQTT_PUBLISH_QUEUE_SIZE;
		mqtt_ctx.queue_count--;
	}

	struct mqtt_pub_entry *entry = &mqtt_ctx.queue[mqtt_ctx.queue_tail];

	strncpy(entry->topic, topic, sizeof(entry->topic) - 1U);
	entry->topic[sizeof(entry->topic) - 1U] = '\0';

	size_t env_len = 0U;
	ret = mqtt_encode_envelope(&mesh, channel_id, gateway_id, entry->payload,
				   sizeof(entry->payload), &env_len);
	if (ret < 0) {
		k_mutex_unlock(&mqtt_ctx.lock);
		return;
	}

	entry->len = (uint16_t)env_len;

	mqtt_ctx.queue_tail =
		(mqtt_ctx.queue_tail + 1U) % CONFIG_MESHTASTIC_MQTT_PUBLISH_QUEUE_SIZE;
	mqtt_ctx.queue_count++;

	LOG_DBG("MQTT uplink queued: %s (%u bytes, depth %u)", entry->topic, entry->len,
		mqtt_ctx.queue_count);

	k_mutex_unlock(&mqtt_ctx.lock);
	mqtt_work_notify();
}
#endif

static void mqtt_handle_publish(const uint8_t *payload, size_t len, const char *topic)
{
	meshtastic_ServiceEnvelope env = meshtastic_ServiceEnvelope_init_zero;
	pb_istream_t stream;
	char local_id[12];
	meshtastic_MeshPacket inject;
	int ret;

	if (!IS_ENABLED(CONFIG_MESHTASTIC_MQTT_DOWNLINK_ENABLED)) {
		return;
	}

	stream = pb_istream_from_buffer(payload, len);
	if (!pb_decode(&stream, meshtastic_ServiceEnvelope_fields, &env)) {
		LOG_DBG("ServiceEnvelope decode failed: %s", PB_GET_ERROR(&stream));
		return;
	}

	if (env.channel_id == NULL || env.gateway_id == NULL || env.packet == NULL) {
		LOG_DBG("MQTT downlink missing envelope fields");
		pb_release(meshtastic_ServiceEnvelope_fields, &env);
		return;
	}

	if (!meshtastic_channels_matches_mqtt_name(env.channel_id)) {
		LOG_DBG("MQTT downlink channel mismatch (got %s)", env.channel_id);
		pb_release(meshtastic_ServiceEnvelope_fields, &env);
		return;
	}

	if (!meshtastic_channels_downlink_enabled(meshtastic_channels_primary_index())) {
		LOG_DBG("MQTT downlink disabled on primary channel");
		pb_release(meshtastic_ServiceEnvelope_fields, &env);
		return;
	}

	mqtt_node_id_str(local_id, sizeof(local_id));
	if (strcmp(env.gateway_id, local_id) == 0) {
		LOG_DBG("Ignoring MQTT echo from our gateway id");
		pb_release(meshtastic_ServiceEnvelope_fields, &env);
		return;
	}

	if (env.packet->from == meshtastic_get_node_id()) {
		LOG_DBG("Ignoring downlink we originally sent");
		pb_release(meshtastic_ServiceEnvelope_fields, &env);
		return;
	}

	if (IS_ENABLED(CONFIG_MESHTASTIC_MQTT_ENCRYPTION_ENABLED) &&
	    env.packet->which_payload_variant == meshtastic_MeshPacket_decoded_tag) {
		LOG_DBG("Ignoring decoded MQTT packet while encryption is enabled");
		pb_release(meshtastic_ServiceEnvelope_fields, &env);
		return;
	}

	meshtastic_mesh_packet_copy(&inject, env.packet);
	inject.via_mqtt = true;
	inject.transport_mechanism = meshtastic_MeshPacket_TransportMechanism_TRANSPORT_MQTT;

	if (inject.which_payload_variant == meshtastic_MeshPacket_decoded_tag) {
		inject.channel = meshtastic_channels_primary_index();
	}

	LOG_DBG("MQTT downlink 0x%08x->0x%08x id=0x%08x via %s topic %s", inject.from, inject.to,
		inject.id, env.gateway_id, topic);
	LOG_INF("MQTT downlink topic %s len %zu", topic, len);
	ret = meshtastic_inject_downlink_mesh_packet(&inject);
	if (ret != 0) {
		LOG_DBG("MQTT downlink inject failed (%d)", ret);
	}

	pb_release(meshtastic_ServiceEnvelope_fields, &env);
}

static void mqtt_on_publish_evt(struct mqtt_client *client, const struct mqtt_evt *evt)
{
	uint8_t payload[CONFIG_MESHTASTIC_MQTT_RX_BUFFER_SIZE];
	int rc;
	char topic_buf[128];
	size_t topic_len;

	if (evt->param.publish.message.topic.topic.size >= sizeof(topic_buf)) {
		LOG_DBG("MQTT publish topic too long (%u bytes)",
			evt->param.publish.message.topic.topic.size);
		return;
	}

	topic_len = evt->param.publish.message.topic.topic.size;
	memcpy(topic_buf, evt->param.publish.message.topic.topic.utf8, topic_len);
	topic_buf[topic_len] = '\0';

	rc = mqtt_read_publish_payload(client, payload, sizeof(payload));
	if (rc < 0) {
		LOG_WRN("mqtt_read_publish_payload failed (%d)", rc);
		return;
	}

	mqtt_handle_publish(payload, (size_t)rc, topic_buf);

	if (evt->param.publish.message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE) {
		const struct mqtt_puback_param ack = {
			.message_id = evt->param.publish.message_id,
		};

		(void)mqtt_publish_qos1_ack(client, &ack);
	}
}

static void mqtt_evt_handler(struct mqtt_client *client, const struct mqtt_evt *evt)
{
	switch (evt->type) {
	case MQTT_EVT_CONNACK:
		if (evt->result != 0) {
			LOG_WRN("MQTT connect failed (%d)", evt->result);
			mqtt_ctx.connected = false;
			break;
		}

		mqtt_ctx.connected = true;
		LOG_INF("MQTT connected to %s", CONFIG_MESHTASTIC_MQTT_BROKER_HOST);
		mqtt_prepare_fds();
		mqtt_drain_queue(MQTT_PUBLISH_BUDGET_ON_CONNECT);
		mqtt_work_notify();
		break;

	case MQTT_EVT_DISCONNECT:
		mqtt_ctx.connected = false;
		mqtt_clear_fds();
		LOG_INF("MQTT disconnected");
		break;

	case MQTT_EVT_PUBLISH:
		mqtt_on_publish_evt(client, evt);
		break;

	default:
		break;
	}
}

static int mqtt_resolve_broker(void)
{
	struct zsock_addrinfo *result = NULL;
	const struct zsock_addrinfo hints = {
		.ai_family = NET_PF_INET,
		.ai_socktype = NET_SOCK_STREAM,
	};
	char port_str[8];
	int rc;

	snprintk(port_str, sizeof(port_str), "%d", CONFIG_MESHTASTIC_MQTT_BROKER_PORT);

	rc = zsock_getaddrinfo(CONFIG_MESHTASTIC_MQTT_BROKER_HOST, port_str, &hints, &result);
	if (rc != 0) {
		if (result != NULL) {
			zsock_freeaddrinfo(result);
		}
		LOG_ERR("MQTT broker resolve failed (%d %s)", rc, zsock_gai_strerror(rc));
		return -ENOENT;
	}

	if (result == NULL || result->ai_addr == NULL) {
		if (result != NULL) {
			zsock_freeaddrinfo(result);
		}
		LOG_ERR("MQTT broker resolve returned no address");
		return -ENOENT;
	}

	if (result->ai_addrlen > sizeof(mqtt_ctx.broker)) {
		zsock_freeaddrinfo(result);
		return -ENOMEM;
	}

	memset(&mqtt_ctx.broker, 0, sizeof(mqtt_ctx.broker));
	memcpy(&mqtt_ctx.broker, result->ai_addr, result->ai_addrlen);
	zsock_freeaddrinfo(result);

	LOG_DBG("MQTT broker %s:%d resolved", CONFIG_MESHTASTIC_MQTT_BROKER_HOST,
		CONFIG_MESHTASTIC_MQTT_BROKER_PORT);
	return 0;
}

static void mqtt_client_configure(void)
{
	mqtt_client_init(&mqtt_ctx.client);

	mqtt_ctx.client.broker = &mqtt_ctx.broker;
	mqtt_ctx.client.evt_cb = mqtt_evt_handler;
	mqtt_ctx.client.client_id.utf8 = mqtt_ctx.client_id;
	mqtt_ctx.client.client_id.size = strlen((char *)mqtt_ctx.client_id);
	mqtt_ctx.username.utf8 = (uint8_t *)CONFIG_MESHTASTIC_MQTT_USERNAME;
	mqtt_ctx.username.size = strlen(CONFIG_MESHTASTIC_MQTT_USERNAME);
	mqtt_ctx.password.utf8 = (uint8_t *)CONFIG_MESHTASTIC_MQTT_PASSWORD;
	mqtt_ctx.password.size = strlen(CONFIG_MESHTASTIC_MQTT_PASSWORD);
	mqtt_ctx.client.user_name = &mqtt_ctx.username;
	mqtt_ctx.client.password = &mqtt_ctx.password;
	mqtt_ctx.client.protocol_version = MQTT_VERSION_3_1_1;
	mqtt_ctx.client.rx_buf = mqtt_ctx.rx_buf;
	mqtt_ctx.client.rx_buf_size = sizeof(mqtt_ctx.rx_buf);
	mqtt_ctx.client.tx_buf = mqtt_ctx.tx_buf;
	mqtt_ctx.client.tx_buf_size = sizeof(mqtt_ctx.tx_buf);
	mqtt_ctx.client.transport.type = MQTT_TRANSPORT_NON_SECURE;
}

static int mqtt_subscribe_downlink(void)
{
	struct mqtt_topic topic = {0};
	struct mqtt_subscription_list sub = {0};
	int ret;

	ret = mqtt_build_subscribe_topic(mqtt_ctx.sub_topic, sizeof(mqtt_ctx.sub_topic));
	if (ret < 0 || ret >= (int)sizeof(mqtt_ctx.sub_topic)) {
		return -EINVAL;
	}

	topic.topic.utf8 = (uint8_t *)mqtt_ctx.sub_topic;
	topic.topic.size = strlen(mqtt_ctx.sub_topic);
	topic.qos = MQTT_QOS_0_AT_MOST_ONCE;

	sub.list = &topic;
	sub.list_count = 1U;
	sub.message_id = sys_rand16_get();

	ret = mqtt_subscribe(&mqtt_ctx.client, &sub);
	if (ret != 0) {
		LOG_ERR("mqtt_subscribe failed (%d)", ret);
	}

	LOG_INF("MQTT subscribed to %s", mqtt_ctx.sub_topic);
	return ret;
}

static int mqtt_try_connect(void)
{
	int ret;

	mqtt_client_configure();

	ret = mqtt_connect(&mqtt_ctx.client);
	if (ret != 0) {
		return ret;
	}

	mqtt_prepare_fds();
	if (mqtt_poll_socket(5000) > 0) {
		(void)mqtt_input(&mqtt_ctx.client);
	}

	if (!mqtt_ctx.connected) {
		mqtt_abort(&mqtt_ctx.client);
		return -ENOTCONN;
	}

	if (IS_ENABLED(CONFIG_MESHTASTIC_MQTT_DOWNLINK_ENABLED)) {
		ret = mqtt_subscribe_downlink();
		if (ret != 0) {
			mqtt_disconnect(&mqtt_ctx.client, NULL);
			mqtt_ctx.connected = false;
			return ret;
		}
	}

	return 0;
}

static int mqtt_process_once(int timeout_ms)
{
	int ret;

	if (!mqtt_ctx.connected) {
		return -ENOTCONN;
	}

	ret = mqtt_poll_socket(timeout_ms);
	if (ret > 0 && (mqtt_fds[0].revents & ZSOCK_POLLIN)) {
		ret = mqtt_input(&mqtt_ctx.client);
		if (ret != 0) {
			return ret;
		}
	} else if (ret == 0) {
		ret = mqtt_live(&mqtt_ctx.client);
		if (ret != 0 && ret != -EAGAIN) {
			return ret;
		}
	}

	return 0;
}

static void mqtt_thread_fn(void *p1, void *p2, void *p3)
{
	int ret;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	mqtt_ctx.thread_running = true;

	while (mqtt_ctx.thread_running) {
		mqtt_handle_disconnect_pending();

		if (!mqtt_ctx.connected) {
			if (!mqtt_network_is_ready()) {
				k_sleep(K_MSEC(MQTT_NET_WAIT_MS));
				continue;
			}

			LOG_DBG("Connecting to MQTT broker %s:%d",
				CONFIG_MESHTASTIC_MQTT_BROKER_HOST,
				CONFIG_MESHTASTIC_MQTT_BROKER_PORT);
			ret = mqtt_resolve_broker();
			if (ret == 0) {
				ret = mqtt_try_connect();
			}
			if (ret != 0) {
				LOG_DBG("MQTT connect failed (%d), retry in %d ms", ret,
					CONFIG_MESHTASTIC_MQTT_RECONNECT_INTERVAL_MS);
				k_sleep(K_MSEC(CONFIG_MESHTASTIC_MQTT_RECONNECT_INTERVAL_MS));
				continue;
			}
		}

		if (!mqtt_network_is_ready()) {
			mqtt_ctx.disconnect_pending = true;
			mqtt_handle_disconnect_pending();
			k_sleep(K_MSEC(MQTT_NET_WAIT_MS));
			continue;
		}

		mqtt_drain_queue(MQTT_PUBLISH_BUDGET_PER_LOOP);

		ret = mqtt_process_once(0);
		if (ret != 0) {
			LOG_DBG("MQTT session error (%d), reconnecting", ret);
			mqtt_disconnect(&mqtt_ctx.client, NULL);
			mqtt_ctx.connected = false;
			mqtt_clear_fds();
			k_sleep(K_MSEC(CONFIG_MESHTASTIC_MQTT_RECONNECT_INTERVAL_MS));
		} else {
			mqtt_drain_queue(MQTT_PUBLISH_BUDGET_PER_LOOP);
			mqtt_perhaps_report_to_map();

			bool has_pending = false;
			k_mutex_lock(&mqtt_ctx.lock, K_FOREVER);
			has_pending = (mqtt_ctx.queue_count > 0);
			k_mutex_unlock(&mqtt_ctx.lock);

			if (!has_pending) {
				(void)k_sem_take(&mqtt_work_sem, K_MSEC(200));
			}
		}
	}
}

void meshtastic_mqtt_on_tx(const struct meshtastic_packet *packet, const uint8_t *wire,
			   size_t wire_len)
{
#if IS_ENABLED(CONFIG_MESHTASTIC_MQTT_UPLINK_ENABLED)
	/*
	 * Match official firmware: uplink on LoRa TX only for packets we originate
	 * (Router::send). Heard packets are uplinked from meshtastic_mqtt_on_rx().
	 */
	if (packet != NULL && packet->from != meshtastic_get_node_id()) {
		LOG_DBG("MQTT uplink skipped on TX (from 0x%08x, not us)", packet->from);
		return;
	}

	mqtt_queue_uplink(packet, wire, wire_len);
#else
	ARG_UNUSED(packet);
	ARG_UNUSED(wire);
	ARG_UNUSED(wire_len);
#endif
}

void meshtastic_mqtt_on_rx(const struct meshtastic_packet *packet, const uint8_t *wire,
			   size_t wire_len)
{
#if IS_ENABLED(CONFIG_MESHTASTIC_MQTT_UPLINK_ENABLED)
	/*
	 * Match official firmware: uplink on LoRa RX only for packets from other nodes.
	 */
	if (packet != NULL && packet->from == meshtastic_get_node_id()) {
		LOG_DBG("MQTT uplink skipped on RX (packet from us)");
		return;
	}

	mqtt_queue_uplink(packet, wire, wire_len);
#else
	ARG_UNUSED(packet);
	ARG_UNUSED(wire);
	ARG_UNUSED(wire_len);
#endif
}

int meshtastic_mqtt_init(void)
{
	int ret;

	if (mqtt_ctx.started) {
		return 0;
	}

	k_mutex_init(&mqtt_ctx.lock);

	net_mgmt_init_event_callback(&mqtt_net_mgmt_cb, mqtt_net_event_handler,
				     NET_EVENT_IPV4_ADDR_ADD | NET_EVENT_IPV4_ADDR_DEL);
	net_mgmt_add_event_callback(&mqtt_net_mgmt_cb);
	mqtt_net_has_ipv4 = mqtt_network_is_ready();

	strncpy(mqtt_ctx.crypt_prefix, CONFIG_MESHTASTIC_MQTT_ROOT,
		sizeof(mqtt_ctx.crypt_prefix) - 1U);
	mqtt_ctx.crypt_prefix[sizeof(mqtt_ctx.crypt_prefix) - 1U] = '\0';

	mqtt_node_id_str((char *)mqtt_ctx.client_id, sizeof(mqtt_ctx.client_id));

	ret = mqtt_build_subscribe_topic(mqtt_ctx.sub_topic, sizeof(mqtt_ctx.sub_topic));
	if (ret < 0) {
		LOG_ERR("MQTT topic setup failed (%d)", ret);
		return ret;
	}

	if (IS_ENABLED(CONFIG_MESHTASTIC_MQTT_MAP_REPORT)) {
		ret = mqtt_build_map_topic(mqtt_ctx.map_topic, sizeof(mqtt_ctx.map_topic));
		if (ret < 0 || ret >= (int)sizeof(mqtt_ctx.map_topic)) {
			LOG_ERR("MQTT map topic setup failed (%d)", ret);
			return -EINVAL;
		}
	}

	k_thread_create(&mqtt_thread, mqtt_stack, K_THREAD_STACK_SIZEOF(mqtt_stack), mqtt_thread_fn,
			NULL, NULL, NULL, CONFIG_MESHTASTIC_MQTT_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&mqtt_thread, "meshtastic_mqtt");
	mqtt_ctx.started = true;

	LOG_INF("Meshtastic MQTT bridge started (broker %s:%d root %s)",
		CONFIG_MESHTASTIC_MQTT_BROKER_HOST, CONFIG_MESHTASTIC_MQTT_BROKER_PORT,
		mqtt_ctx.crypt_prefix);

	return 0;
}
