/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <pb_decode.h>
#include <pb_encode.h>

#include <zephyr/meshtastic/meshtastic.h>
#include <zephyr/meshtastic/nodeinfo.h>

#include "meshtastic_modules.h"

#include "modules_fixture.h"

/* Fresh peers per test: the peer cache is not resettable. */
#define REQUEST_PEER    0xB0000001U
#define SUPPRESSED_PEER 0xB0000002U
#define UNKNOWN_PEER    0xB0000003U
#define KNOWN_PEER      0xB0000004U

static uint8_t tx_payload[MESHTASTIC_MAX_PAYLOAD_LEN];

static meshtastic_User decode_user(const struct meshtastic_packet *packet)
{
	meshtastic_User user = meshtastic_User_init_zero;
	pb_istream_t stream = pb_istream_from_buffer(packet->payload, packet->payload_len);

	zassert_true(pb_decode(&stream, meshtastic_User_fields, &user), "User decode failed");

	return user;
}

static struct meshtastic_packet text_from(uint32_t from)
{
	struct meshtastic_packet packet = {
		.from = from,
		.to = MESHTASTIC_NODE_BROADCAST,
		.id = from,
		.portnum = MESHTASTIC_PORT_TEXT_MESSAGE,
		.payload = (const uint8_t *)"hi",
		.payload_len = 2U,
		.channel_index = MESHTASTIC_CHANNEL_INDEX_INVALID,
	};

	return packet;
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	modules_reset();
}

ZTEST_SUITE(nodeinfo, NULL, modules_suite_setup, before, NULL, NULL);

ZTEST(nodeinfo, test_sending_node_info_broadcasts_our_user)
{
	struct meshtastic_packet sent;
	meshtastic_User user;

	zassert_ok(meshtastic_send_node_info(MESHTASTIC_NODE_BROADCAST));

	modules_decode_tx(1U, &sent, tx_payload, sizeof(tx_payload));
	zassert_equal(sent.portnum, MESHTASTIC_PORT_NODEINFO);
	zassert_equal(sent.to, MESHTASTIC_NODE_BROADCAST);
	zassert_false(sent.want_response);

	user = decode_user(&sent);
	zassert_str_equal(user.id, "!12345678");
	zassert_str_equal(user.long_name, "Zephyr Test Node");
	zassert_str_equal(user.short_name, "ZTN");
	/* Upstream derives the node number from the low four bytes of macaddr. */
	zassert_equal(user.macaddr[2], 0x12U);
	zassert_equal(user.macaddr[5], 0x78U);
}

ZTEST(nodeinfo, test_a_request_is_answered_with_our_user)
{
	struct meshtastic_packet request = {
		.from = REQUEST_PEER,
		.to = TEST_NODE_ID,
		.id = 0x5150U,
		.portnum = MESHTASTIC_PORT_NODEINFO,
		.channel_index = MESHTASTIC_CHANNEL_INDEX_INVALID,
		.want_response = true,
	};
	struct meshtastic_packet reply;

	meshtastic_dispatch_modules(&request);

	modules_decode_tx(1U, &reply, tx_payload, sizeof(tx_payload));
	zassert_equal(reply.to, REQUEST_PEER);
	zassert_equal(reply.portnum, MESHTASTIC_PORT_NODEINFO);
	zassert_equal(reply.request_id, 0x5150U);
	zassert_false(reply.want_response);
	zassert_str_equal(decode_user(&reply).short_name, "ZTN");
}

ZTEST(nodeinfo, test_a_repeated_request_is_suppressed)
{
	struct meshtastic_packet request = {
		.from = SUPPRESSED_PEER,
		.to = TEST_NODE_ID,
		.id = 1U,
		.portnum = MESHTASTIC_PORT_NODEINFO,
		.channel_index = MESHTASTIC_CHANNEL_INDEX_INVALID,
		.want_response = true,
	};

	meshtastic_dispatch_modules(&request);
	zassert_true(mock_lora_wait_for_send_count(1U, K_MSEC(500)),
		     "the radio did not transmit in time");

	request.id++;
	meshtastic_dispatch_modules(&request);
	k_msleep(100);

	zassert_equal(mock_lora_send_count(), 1U);
}

ZTEST(nodeinfo, test_an_unknown_peer_is_asked_for_its_node_info)
{
	struct meshtastic_packet text = text_from(UNKNOWN_PEER);
	struct meshtastic_packet request;

	meshtastic_dispatch_modules(&text);

	modules_decode_tx(1U, &request, tx_payload, sizeof(tx_payload));
	zassert_equal(request.to, UNKNOWN_PEER);
	zassert_equal(request.portnum, MESHTASTIC_PORT_NODEINFO);
	zassert_true(request.want_response, "the probe must ask the peer to answer");
}

ZTEST(nodeinfo, test_a_peer_that_already_introduced_itself_is_not_probed)
{
	struct meshtastic_packet text = text_from(KNOWN_PEER);

	modules_introduce_peer(KNOWN_PEER);
	k_msleep(100);
	zassert_equal(mock_lora_send_count(), 0U, "a NodeInfo broadcast needs no answer");

	meshtastic_dispatch_modules(&text);
	k_msleep(100);
	zassert_equal(mock_lora_send_count(), 0U);
}

ZTEST(nodeinfo, test_our_own_packets_never_trigger_a_probe)
{
	struct meshtastic_packet text = text_from(TEST_NODE_ID);

	meshtastic_dispatch_modules(&text);

	text.from = 0U;
	meshtastic_dispatch_modules(&text);
	k_msleep(100);

	zassert_equal(mock_lora_send_count(), 0U);
}
