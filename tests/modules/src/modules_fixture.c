/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <pb_encode.h>

#include "meshtastic_core.h"
#include "meshtastic_modules.h"
#include "meshtastic_packet.h"

#include "modules_fixture.h"

static struct {
	struct meshtastic_event event;
	struct meshtastic_packet packet;
	uint8_t payload[MESHTASTIC_MAX_PAYLOAD_LEN];
	size_t payload_len;
	uint32_t count;
} events;

static void on_event(const struct meshtastic_event *event, void *user_data)
{
	ARG_UNUSED(user_data);

	events.event = *event;
	events.payload_len = 0U;
	if (event->packet != NULL) {
		events.packet = *event->packet;
		events.packet.payload = events.payload;
		events.payload_len = event->packet->payload_len;
		memcpy(events.payload, event->packet->payload, events.payload_len);
	} else {
		memset(&events.packet, 0, sizeof(events.packet));
	}
	events.count++;
}

void *modules_suite_setup(void)
{
	static struct meshtastic_config cfg = {
		.node_id = TEST_NODE_ID,
		.psk = meshtastic_default_psk,
		.psk_len = sizeof(meshtastic_default_psk),
		.channel_name = MESHTASTIC_CHANNEL_LONGFAST,
		.frequency = MESHTASTIC_FREQ_EU,
		.long_name = "Zephyr Test Node",
		.short_name = "ZTN",
	};
	static bool initialized;

	if (!initialized) {
		cfg.lora_dev = mock_lora_device();
		zassert_ok(meshtastic_init(&cfg), "meshtastic_init failed");
		meshtastic_set_event_cb(on_event, NULL);
		initialized = true;
	}

	return NULL;
}

void modules_reset(void)
{
	mock_lora_reset();
	memset(&events, 0, sizeof(events));
}

uint32_t modules_event_count(void)
{
	return events.count;
}

bool modules_last_event(struct meshtastic_event *event, struct meshtastic_packet *packet,
			uint8_t *payload, size_t payload_len)
{
	if (events.count == 0U) {
		return false;
	}

	*event = events.event;
	if (packet != NULL) {
		*packet = events.packet;
		packet->payload = payload;
	}
	if (payload != NULL) {
		zassert_true(events.payload_len <= payload_len, "event payload buffer too small");
		memcpy(payload, events.payload, events.payload_len);
	}

	return true;
}

void modules_introduce_peer(uint32_t node)
{
	meshtastic_User user = meshtastic_User_init_zero;
	uint8_t payload[MESHTASTIC_MAX_PAYLOAD_LEN];
	pb_ostream_t stream = pb_ostream_from_buffer(payload, sizeof(payload));
	struct meshtastic_packet packet = {
		.from = node,
		.to = MESHTASTIC_NODE_BROADCAST,
		.id = node,
		.portnum = MESHTASTIC_PORT_NODEINFO,
		.payload = payload,
		.channel_index = MESHTASTIC_CHANNEL_INDEX_INVALID,
	};

	snprintk(user.id, sizeof(user.id), "!%08x", node);
	strcpy(user.long_name, "Peer");
	strcpy(user.short_name, "PR");

	zassert_true(pb_encode(&stream, meshtastic_User_fields, &user), "User encode failed");
	packet.payload_len = stream.bytes_written;

	meshtastic_dispatch_modules(&packet);
}

void modules_decode_tx(uint32_t expected, struct meshtastic_packet *packet, uint8_t *payload,
		       size_t payload_len)
{
	uint8_t wire[MESHTASTIC_PKT_MAX];
	uint32_t wire_len;

	mock_lora_wait_for_send_count(expected, K_MSEC(500));
	zassert_equal(mock_lora_send_count(), expected, "unexpected lora_send count");

	wire_len = mock_lora_last_tx(wire, sizeof(wire));
	zassert_ok(meshtastic_decode_wire_packet(wire, (int)wire_len, 0, 0, packet, payload,
						 payload_len),
		   "could not decode the transmitted frame");
}
