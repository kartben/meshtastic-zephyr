#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/lora.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <zephyr/meshtastic/meshtastic.h>

#include "meshtastic_channels.h"
#include "meshtastic_core.h"
#include "meshtastic_packet.h"

#define TEST_NODE_ID 0x12345678U
#define PEER_NODE_ID 0x87654321U
#define OTHER_NODE_ID 0x13572468U

struct mock_lora_state {
	struct k_mutex lock;
	struct lora_modem_config config;
	lora_recv_cb rx_cb;
	void *rx_user_data;
	int send_result;
	uint32_t send_count;
	uint32_t config_count;
	uint8_t last_tx[MESHTASTIC_PKT_MAX];
	uint32_t last_tx_len;
};

static struct mock_lora_state mock_lora;

static int mock_lora_init(const struct device *dev)
{
	ARG_UNUSED(dev);

	k_mutex_init(&mock_lora.lock);
	mock_lora.send_result = 0;

	return 0;
}

static int mock_lora_config(const struct device *dev, const struct lora_modem_config *config)
{
	ARG_UNUSED(dev);

	k_mutex_lock(&mock_lora.lock, K_FOREVER);
	mock_lora.config = *config;
	mock_lora.config_count++;
	k_mutex_unlock(&mock_lora.lock);

	return 0;
}

static uint32_t mock_lora_airtime(const struct device *dev, uint32_t data_len)
{
	ARG_UNUSED(dev);

	return data_len;
}

static int mock_lora_send(const struct device *dev, uint8_t *data, uint32_t data_len)
{
	int ret;

	ARG_UNUSED(dev);

	k_mutex_lock(&mock_lora.lock, K_FOREVER);
	zassert_true(data_len <= sizeof(mock_lora.last_tx), "unexpected tx len %u", data_len);
	memcpy(mock_lora.last_tx, data, data_len);
	mock_lora.last_tx_len = data_len;
	mock_lora.send_count++;
	ret = mock_lora.send_result;
	k_mutex_unlock(&mock_lora.lock);

	return ret;
}

static int mock_lora_send_async(const struct device *dev, uint8_t *data, uint32_t data_len,
				struct k_poll_signal *async)
{
	int ret = mock_lora_send(dev, data, data_len);

	if (async != NULL) {
		k_poll_signal_raise(async, ret);
	}

	return ret;
}

static int mock_lora_recv(const struct device *dev, uint8_t *data, uint8_t size, k_timeout_t timeout,
			  int16_t *rssi, int8_t *snr)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(data);
	ARG_UNUSED(size);
	ARG_UNUSED(timeout);
	ARG_UNUSED(rssi);
	ARG_UNUSED(snr);

	return -ENOTSUP;
}

static int mock_lora_recv_async(const struct device *dev, lora_recv_cb cb, void *user_data)
{
	ARG_UNUSED(dev);

	k_mutex_lock(&mock_lora.lock, K_FOREVER);
	mock_lora.rx_cb = cb;
	mock_lora.rx_user_data = user_data;
	k_mutex_unlock(&mock_lora.lock);

	return 0;
}

static DEVICE_API(lora, mock_lora_api) = {
	.config = mock_lora_config,
	.airtime = mock_lora_airtime,
	.send = mock_lora_send,
	.send_async = mock_lora_send_async,
	.recv = mock_lora_recv,
	.recv_async = mock_lora_recv_async,
};

DEVICE_DEFINE(mock_lora, "mock_lora", mock_lora_init, NULL, NULL, NULL, POST_KERNEL,
	      CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &mock_lora_api);

static const struct device *const lora_dev = DEVICE_GET(mock_lora);

struct test_state {
	struct k_sem rx_sem;
	struct k_sem tx_sem;
	struct meshtastic_packet last_rx;
	uint8_t last_rx_payload[MESHTASTIC_MAX_PAYLOAD_LEN];
	size_t last_rx_payload_len;
	struct meshtastic_event last_event;
	uint32_t recv_count;
	uint32_t event_count;
};

static struct test_state state;

static void reset_callbacks_state(void)
{
	memset(&state.last_rx, 0, sizeof(state.last_rx));
	memset(state.last_rx_payload, 0, sizeof(state.last_rx_payload));
	memset(&state.last_event, 0, sizeof(state.last_event));
	state.last_rx_payload_len = 0U;
	state.recv_count = 0U;
	state.event_count = 0U;
	k_sem_reset(&state.rx_sem);
	k_sem_reset(&state.tx_sem);
}

static void reset_mock_lora(void)
{
	lora_recv_cb rx_cb;
	void *rx_user_data;

	k_mutex_lock(&mock_lora.lock, K_FOREVER);
	rx_cb = mock_lora.rx_cb;
	rx_user_data = mock_lora.rx_user_data;
	memset(&mock_lora.config, 0, sizeof(mock_lora.config));
	mock_lora.rx_cb = rx_cb;
	mock_lora.rx_user_data = rx_user_data;
	mock_lora.send_result = 0;
	mock_lora.send_count = 0U;
	mock_lora.config_count = 0U;
	mock_lora.last_tx_len = 0U;
	memset(mock_lora.last_tx, 0, sizeof(mock_lora.last_tx));
	k_mutex_unlock(&mock_lora.lock);
}

static void on_recv(uint32_t from, uint32_t to, uint32_t portnum, const uint8_t *payload,
		    size_t payload_len, int16_t rssi, int8_t snr)
{
	state.last_rx = (struct meshtastic_packet){
		.from = from,
		.to = to,
		.portnum = portnum,
		.payload = state.last_rx_payload,
		.payload_len = payload_len,
		.rssi = rssi,
		.snr = snr,
	};
	state.last_rx_payload_len = payload_len;
	if (payload_len > 0U) {
		memcpy(state.last_rx_payload, payload, payload_len);
	}
	state.recv_count++;
	k_sem_give(&state.rx_sem);
}

static void on_event(const struct meshtastic_event *event, void *user_data)
{
	ARG_UNUSED(user_data);

	state.last_event = *event;
	state.event_count++;
	if (event->type == MESHTASTIC_EVENT_TX_DONE) {
		k_sem_give(&state.tx_sem);
	}
}

static void *protocol_suite_setup(void)
{
	static struct meshtastic_config cfg = {
		.lora_dev = lora_dev,
		.node_id = TEST_NODE_ID,
		.psk = meshtastic_default_psk,
		.psk_len = sizeof(meshtastic_default_psk),
		.channel_name = MESHTASTIC_CHANNEL_LONGFAST,
		.frequency = MESHTASTIC_FREQ_EU,
	};
	int ret;

	k_sem_init(&state.rx_sem, 0, 1);
	k_sem_init(&state.tx_sem, 0, 1);

	zassert_true(device_is_ready(lora_dev), "mock lora device not ready");

	ret = meshtastic_init(&cfg);
	zassert_ok(ret, "meshtastic_init failed: %d", ret);

	meshtastic_set_recv_cb(on_recv);
	meshtastic_set_event_cb(on_event, NULL);

	reset_mock_lora();
	reset_callbacks_state();

	return NULL;
}

static void protocol_before(void *fixture)
{
	ARG_UNUSED(fixture);
	reset_mock_lora();
	reset_callbacks_state();
}

static void build_peer_wire_packet(uint32_t to, uint32_t id, uint8_t hop_limit, const char *text,
				   uint8_t *wire, uint32_t *wire_len)
{
	struct meshtastic_packet packet = {
		.from = PEER_NODE_ID,
		.to = to,
		.id = id,
		.portnum = MESHTASTIC_PORT_TEXT_MESSAGE,
		.payload = (const uint8_t *)text,
		.payload_len = strlen(text),
		.hop_limit = hop_limit,
		.hop_start = hop_limit,
		.channel_index = meshtastic_channels_primary_index(),
	};
	int ret;

	ret = meshtastic_build_wire_packet(&packet, wire, wire_len);
	zassert_ok(ret, "meshtastic_build_wire_packet failed: %d", ret);
}

static void inject_rx_frame(const uint8_t *wire, uint32_t wire_len, int16_t rssi, int8_t snr)
{
	lora_recv_cb cb;
	void *user_data;
	uint8_t frame[MESHTASTIC_PKT_MAX];

	zassert_true(wire_len <= sizeof(frame), "unexpected rx len %u", wire_len);

	k_mutex_lock(&mock_lora.lock, K_FOREVER);
	cb = mock_lora.rx_cb;
	user_data = mock_lora.rx_user_data;
	k_mutex_unlock(&mock_lora.lock);

	zassert_not_null(cb, "rx callback not armed");

	memcpy(frame, wire, wire_len);
	cb(lora_dev, frame, wire_len, rssi, snr, user_data);
}

ZTEST_SUITE(protocol_stack, NULL, protocol_suite_setup, protocol_before, NULL, NULL);

ZTEST(protocol_stack, test_send_text_uses_mock_lora_and_round_trips)
{
	struct meshtastic_packet decoded;
	uint8_t payload[MESHTASTIC_MAX_PAYLOAD_LEN];
	struct meshtastic_status before;
	struct meshtastic_status after;
	int ret;

	ret = meshtastic_get_status(&before);
	zassert_ok(ret, "status read failed: %d", ret);

	ret = meshtastic_send_text(MESHTASTIC_NODE_BROADCAST, "ping");
	zassert_ok(ret, "meshtastic_send_text failed: %d", ret);
	zassert_ok(k_sem_take(&state.tx_sem, K_SECONDS(1)), "timed out waiting for tx event");

	k_mutex_lock(&mock_lora.lock, K_FOREVER);
	zassert_equal(mock_lora.send_count, 1U, "expected one lora_send call");
	zassert_true(mock_lora.last_tx_len > MESHTASTIC_HDR_LEN, "expected wire payload");
	ret = meshtastic_decode_wire_packet(mock_lora.last_tx, mock_lora.last_tx_len, 0, 0, &decoded,
						    payload, sizeof(payload));
	k_mutex_unlock(&mock_lora.lock);
	zassert_ok(ret, "meshtastic_decode_wire_packet failed: %d", ret);

	zassert_equal(decoded.from, TEST_NODE_ID, "unexpected source");
	zassert_equal(decoded.to, MESHTASTIC_NODE_BROADCAST, "unexpected destination");
	zassert_equal(decoded.portnum, MESHTASTIC_PORT_TEXT_MESSAGE, "unexpected port");
	zassert_equal(decoded.payload_len, 4U, "unexpected payload len");
	zassert_mem_equal(decoded.payload, "ping", 4U, "unexpected payload");
	zassert_equal(state.last_event.type, MESHTASTIC_EVENT_TX_DONE, "unexpected event");

	ret = meshtastic_get_status(&after);
	zassert_ok(ret, "status read failed: %d", ret);
	zassert_equal(after.tx_packets, before.tx_packets + 1U, "tx counter not incremented");
}

ZTEST(protocol_stack, test_mock_radio_receive_delivers_packet)
{
	uint8_t wire[MESHTASTIC_PKT_MAX];
	uint32_t wire_len;
	struct meshtastic_status before;
	struct meshtastic_status after;
	int ret;

	ret = meshtastic_get_status(&before);
	zassert_ok(ret, "status read failed: %d", ret);

	build_peer_wire_packet(TEST_NODE_ID, 0x1001U, 3U, "pong", wire, &wire_len);
	inject_rx_frame(wire, wire_len, -42, 7);

	zassert_ok(k_sem_take(&state.rx_sem, K_SECONDS(1)), "timed out waiting for rx callback");
	zassert_equal(state.recv_count, 1U, "expected one delivery");
	zassert_equal(state.last_rx.from, PEER_NODE_ID, "unexpected source");
	zassert_equal(state.last_rx.to, TEST_NODE_ID, "unexpected destination");
	zassert_equal(state.last_rx.portnum, MESHTASTIC_PORT_TEXT_MESSAGE, "unexpected port");
	zassert_equal(state.last_rx_payload_len, 4U, "unexpected payload len");
	zassert_mem_equal(state.last_rx_payload, "pong", 4U, "unexpected payload");
	zassert_equal(state.last_rx.rssi, -42, "unexpected rssi");
	zassert_equal(state.last_rx.snr, 7, "unexpected snr");
	zassert_equal(state.last_event.type, MESHTASTIC_EVENT_PACKET_RECEIVED, "unexpected event");

	ret = meshtastic_get_status(&after);
	zassert_ok(ret, "status read failed: %d", ret);
	zassert_equal(after.rx_packets, before.rx_packets + 1U, "rx counter not incremented");
}

ZTEST(protocol_stack, test_duplicate_packets_are_suppressed)
{
	uint8_t wire[MESHTASTIC_PKT_MAX];
	uint32_t wire_len;
	struct meshtastic_status before;
	struct meshtastic_status after;
	int ret;

	ret = meshtastic_get_status(&before);
	zassert_ok(ret, "status read failed: %d", ret);

	build_peer_wire_packet(TEST_NODE_ID, 0x2002U, 3U, "dupe", wire, &wire_len);
	inject_rx_frame(wire, wire_len, -30, 5);
	zassert_ok(k_sem_take(&state.rx_sem, K_SECONDS(1)), "timed out waiting for first rx");

	inject_rx_frame(wire, wire_len, -30, 5);
	zassert_equal(-EAGAIN, k_sem_take(&state.rx_sem, K_MSEC(100)),
		      "duplicate unexpectedly delivered");

	ret = meshtastic_get_status(&after);
	zassert_ok(ret, "status read failed: %d", ret);
	zassert_equal(after.duplicate_packets, before.duplicate_packets + 1U,
		      "duplicate counter not incremented");
	zassert_equal(state.recv_count, 1U, "expected single delivery");
}

ZTEST(protocol_stack, test_foreign_unicast_is_relayed_with_decremented_hop_limit)
{
	const struct meshtastic_wire_header *hdr;
	uint8_t wire[MESHTASTIC_PKT_MAX];
	uint32_t wire_len;
	struct meshtastic_status before;
	struct meshtastic_status after;
	int ret;

	ret = meshtastic_get_status(&before);
	zassert_ok(ret, "status read failed: %d", ret);

	build_peer_wire_packet(OTHER_NODE_ID, 0x3003U, 3U, "relay", wire, &wire_len);
	inject_rx_frame(wire, wire_len, -55, 2);
	k_sleep(K_MSEC(100));

	k_mutex_lock(&mock_lora.lock, K_FOREVER);
	zassert_equal(mock_lora.send_count, 1U, "expected relay transmission");
	hdr = (const struct meshtastic_wire_header *)mock_lora.last_tx;
	zassert_equal(hdr->flags & MESHTASTIC_FLAGS_HOP_LIMIT_MASK, 2U,
		      "relay hop limit not decremented");
	k_mutex_unlock(&mock_lora.lock);
	zassert_equal(state.recv_count, 0U, "foreign unicast should not be delivered locally");

	ret = meshtastic_get_status(&after);
	zassert_ok(ret, "status read failed: %d", ret);
	zassert_equal(after.relayed_packets, before.relayed_packets + 1U,
		      "relay counter not incremented");
}
