/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

#include "meshtastic_modules.h"

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(meshtastic, CONFIG_MESHTASTIC_LOG_LEVEL);

static bool packet_is_for_us(const struct meshtastic_packet *packet)
{
	return packet->to == meshtastic_get_node_id() || packet->to == MESHTASTIC_NODE_BROADCAST;
}

static void meshtastic_module_message_on_packet(const struct meshtastic_packet *packet)
{
	if (packet == NULL || packet->payload == NULL || packet->payload_len == 0U) {
		return;
	}

	if (packet->portnum != MESHTASTIC_PORT_TEXT_MESSAGE) {
		return;
	}

	if (packet->from == 0U || packet->from == meshtastic_get_node_id()) {
		return;
	}

	if (!packet_is_for_us(packet)) {
		return;
	}

	LOG_INF("Text from 0x%08x: %.*s", packet->from, (int)packet->payload_len,
		(const char *)packet->payload);

	meshtastic_emit_event(MESHTASTIC_EVENT_TEXT_MESSAGE, 0, packet);
}

MESHTASTIC_MODULE_DEFINE(message, MESHTASTIC_PORT_TEXT_MESSAGE, 0,
			 meshtastic_module_message_on_packet, NULL);

int meshtastic_message_init(void)
{
	return 0;
}
