/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

/**
 * @file meshtastic_reliable.h
 * @brief Internal acknowledgement tracking for locally originated packets.
 *
 * The receive side of the acknowledgement protocol lives in
 * meshtastic_routing.c, which replies with a ROUTING acknowledgement whenever a
 * packet addressed to this node asks for one. This module is the transmit side:
 * it remembers packets we sent with @c want_ack, retransmits them while no
 * acknowledgement comes back, and reports the final outcome as a stack event.
 */

#ifndef ZEPHYR_SUBSYS_MESHTASTIC_RELIABLE_H_
#define ZEPHYR_SUBSYS_MESHTASTIC_RELIABLE_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include <zephyr/sys/util.h>

#include "meshtastic_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(CONFIG_MESHTASTIC_RELIABLE)

/**
 * @brief Drop every tracked packet and stop the retry timer.
 *
 * Called from meshtastic_init() so a re-initialised stack does not retransmit
 * packets belonging to a previous run.
 */
void meshtastic_reliable_reset(void);

/**
 * @brief Start tracking a packet that was just handed to the radio.
 *
 * Ignores packets that cannot be acknowledged: broadcasts, packets addressed to
 * this node, ROUTING traffic, and packets that did not set @c want_ack.
 *
 * @param packet   The packet as transmitted, with @c from and @c id resolved.
 * @param wire     Wire frame to retransmit verbatim.
 * @param wire_len Length of @p wire in bytes.
 */
void meshtastic_reliable_track(const struct meshtastic_packet *packet, const uint8_t *wire,
			       uint32_t wire_len);

/**
 * @brief Resolve a tracked packet from an inbound ROUTING packet.
 *
 * Matches @c request_id against the tracking table. On a match the entry is
 * released and MESHTASTIC_EVENT_TX_ACKED (or MESHTASTIC_EVENT_TX_NO_ACK, for a
 * routing error) is emitted.
 *
 * @return true when @p packet acknowledged a tracked packet.
 */
bool meshtastic_reliable_on_routing(const struct meshtastic_packet *packet);

/** @brief Number of packets currently awaiting an acknowledgement. */
size_t meshtastic_reliable_pending(void);

#else /* CONFIG_MESHTASTIC_RELIABLE */

static inline void meshtastic_reliable_reset(void)
{
}

static inline void meshtastic_reliable_track(const struct meshtastic_packet *packet,
					     const uint8_t *wire, uint32_t wire_len)
{
	ARG_UNUSED(packet);
	ARG_UNUSED(wire);
	ARG_UNUSED(wire_len);
}

static inline bool meshtastic_reliable_on_routing(const struct meshtastic_packet *packet)
{
	ARG_UNUSED(packet);

	return false;
}

static inline size_t meshtastic_reliable_pending(void)
{
	return 0U;
}

#endif /* CONFIG_MESHTASTIC_RELIABLE */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_SUBSYS_MESHTASTIC_RELIABLE_H_ */
