/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <zephyr/meshtastic/meshtastic.h>

#include "meshtastic_modules.h"

#include "modules_fixture.h"

#define PEER_NODE_ID  0xC0000001U
#define OTHER_NODE_ID 0xC0000002U

static const char text[] = "hello mesh";

static struct meshtastic_packet text_packet(uint32_t from, uint32_t to)
{
	struct meshtastic_packet packet = {
		.from = from,
		.to = to,
		.id = 0x77U,
		.portnum = MESHTASTIC_PORT_TEXT_MESSAGE,
		.payload = (const uint8_t *)text,
		.payload_len = strlen(text),
		.channel_index = MESHTASTIC_CHANNEL_INDEX_INVALID,
	};

	return packet;
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	modules_reset();
}

ZTEST_SUITE(text_message, NULL, modules_suite_setup, before, NULL, NULL);

ZTEST(text_message, test_a_direct_message_raises_a_text_event)
{
	struct meshtastic_packet packet = text_packet(PEER_NODE_ID, TEST_NODE_ID);
	struct meshtastic_packet received;
	uint8_t payload[MESHTASTIC_MAX_PAYLOAD_LEN];
	struct meshtastic_event event;

	meshtastic_dispatch_modules(&packet);

	zassert_true(modules_last_event(&event, &received, payload, sizeof(payload)));
	zassert_equal(event.type, MESHTASTIC_EVENT_TEXT_MESSAGE);
	zassert_equal(received.from, PEER_NODE_ID);
	zassert_equal(received.payload_len, strlen(text));
	zassert_mem_equal(payload, text, strlen(text));
}

ZTEST(text_message, test_a_broadcast_message_raises_a_text_event)
{
	struct meshtastic_packet packet = text_packet(PEER_NODE_ID, MESHTASTIC_NODE_BROADCAST);
	struct meshtastic_event event;

	meshtastic_dispatch_modules(&packet);

	zassert_true(modules_last_event(&event, NULL, NULL, 0U));
	zassert_equal(event.type, MESHTASTIC_EVENT_TEXT_MESSAGE);
}

ZTEST(text_message, test_messages_for_other_nodes_are_not_delivered_locally)
{
	struct meshtastic_packet packet = text_packet(PEER_NODE_ID, OTHER_NODE_ID);

	meshtastic_dispatch_modules(&packet);

	zassert_equal(modules_event_count(), 0U);
}

ZTEST(text_message, test_our_own_and_empty_messages_are_ignored)
{
	struct meshtastic_packet packet = text_packet(TEST_NODE_ID, TEST_NODE_ID);

	meshtastic_dispatch_modules(&packet);

	packet.from = 0U;
	meshtastic_dispatch_modules(&packet);

	packet.from = PEER_NODE_ID;
	packet.payload = NULL;
	packet.payload_len = 0U;
	meshtastic_dispatch_modules(&packet);

	zassert_equal(modules_event_count(), 0U);
}

ZTEST(text_message, test_other_ports_do_not_raise_a_text_event)
{
	struct meshtastic_packet packet = text_packet(PEER_NODE_ID, TEST_NODE_ID);

	packet.portnum = MESHTASTIC_PORT_PRIVATE;
	meshtastic_dispatch_modules(&packet);

	zassert_equal(modules_event_count(), 0U);
}
