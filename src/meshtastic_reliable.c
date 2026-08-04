/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

/*
 * Transmit-side acknowledgement tracking.
 *
 * Packets sent with want_ack are kept here, wire frame and all, until the
 * destination returns a ROUTING acknowledgement carrying our packet ID in
 * request_id. While no acknowledgement arrives the frame is retransmitted
 * unchanged: reusing the original packet ID is what lets relays and the
 * destination recognise the retry as a duplicate rather than a new message.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/util.h>

#include <pb_decode.h>

#include "meshtastic_core.h"
#include "meshtastic_outbound.h"
#include "meshtastic_packet.h"
#include "meshtastic_reliable.h"

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(meshtastic, CONFIG_MESHTASTIC_LOG_LEVEL);

struct reliable_entry {
	/* Packet ID we are waiting to see acknowledged; 0 marks a free slot. */
	uint32_t id;
	uint32_t dest;
	uint32_t portnum;
	int64_t due_ms;
	uint32_t wire_len;
	uint8_t attempts_left;
	uint8_t wire[MESHTASTIC_PKT_MAX];
};

/* Outcome captured under the lock and reported after releasing it. */
struct reliable_outcome {
	uint32_t id;
	uint32_t dest;
	uint32_t portnum;
};

static struct reliable_entry reliable_entries[CONFIG_MESHTASTIC_RELIABLE_MAX_PENDING];
static K_MUTEX_DEFINE(reliable_lock);

static void reliable_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(reliable_work, reliable_work_fn);

static int64_t reliable_retry_delay_ms(void)
{
	int64_t delay = CONFIG_MESHTASTIC_RELIABLE_RETRY_INTERVAL_MS;

	if (CONFIG_MESHTASTIC_RELIABLE_RETRY_JITTER_MS > 0) {
		delay += (int64_t)(sys_rand32_get() %
				   (uint32_t)(CONFIG_MESHTASTIC_RELIABLE_RETRY_JITTER_MS + 1));
	}

	return delay;
}

/*
 * Emit a delivery outcome. Never called with the table lock held: the event
 * callback runs application code that may call back into the stack.
 */
static void reliable_emit(const struct reliable_outcome *outcome, enum meshtastic_event_type type,
			  int err)
{
	struct meshtastic_packet packet = {
		.from = mt.node_id,
		.to = outcome->dest,
		.id = outcome->id,
		.portnum = outcome->portnum,
		.payload = NULL,
		.payload_len = 0U,
		.channel_index = MESHTASTIC_CHANNEL_INDEX_INVALID,
		.want_ack = true,
	};

	meshtastic_emit_event(type, err, &packet);
}

/* Re-arm the retry timer for the earliest pending entry. Call with lock held. */
static void reliable_reschedule(int64_t now)
{
	int64_t next = INT64_MAX;

	for (size_t i = 0U; i < ARRAY_SIZE(reliable_entries); i++) {
		if (reliable_entries[i].id != 0U) {
			next = MIN(next, reliable_entries[i].due_ms);
		}
	}

	if (next == INT64_MAX) {
		return;
	}

	(void)k_work_reschedule(&reliable_work, K_MSEC(MAX(next - now, (int64_t)0)));
}

/*
 * An acknowledgement is a Routing message whose error_reason is NONE. Anything
 * that does not decode to an explicit error is treated as success: matching
 * request_id is the authoritative signal, the payload only distinguishes an
 * ACK from a NAK.
 */
static bool reliable_routing_is_ack(const uint8_t *payload, size_t payload_len, int *err)
{
	meshtastic_Routing routing = meshtastic_Routing_init_zero;
	pb_istream_t stream;

	*err = 0;

	if (payload == NULL || payload_len == 0U) {
		return true;
	}

	stream = pb_istream_from_buffer(payload, payload_len);
	if (!pb_decode(&stream, meshtastic_Routing_fields, &routing)) {
		return true;
	}

	if (routing.which_variant != meshtastic_Routing_error_reason_tag ||
	    routing.error_reason == meshtastic_Routing_Error_NONE) {
		return true;
	}

	LOG_DBG("Routing error %d in acknowledgement", (int)routing.error_reason);
	*err = -EHOSTUNREACH;

	return false;
}

void meshtastic_reliable_reset(void)
{
	/*
	 * Clear before cancelling: a handler that is already running re-arms the
	 * timer as it finishes, and cancelling first would lose that race. With
	 * the table emptied first, any such run finds nothing to do.
	 */
	k_mutex_lock(&reliable_lock, K_FOREVER);
	memset(reliable_entries, 0, sizeof(reliable_entries));
	k_mutex_unlock(&reliable_lock);

	(void)k_work_cancel_delayable(&reliable_work);
}

void meshtastic_reliable_track(const struct meshtastic_packet *packet, const uint8_t *wire,
			       uint32_t wire_len)
{
	struct reliable_entry *entry = NULL;
	int64_t now;

	if (packet == NULL || wire == NULL || wire_len == 0U || wire_len > MESHTASTIC_PKT_MAX) {
		return;
	}

	/*
	 * Only unicast application traffic can be acknowledged. Excluding
	 * ROUTING keeps acknowledgements themselves out of the table, so a lost
	 * acknowledgement is recovered by the sender's retry rather than by a
	 * second retry loop running in the opposite direction.
	 */
	if (!packet->want_ack || packet->id == 0U || packet->to == MESHTASTIC_NODE_BROADCAST ||
	    packet->to == mt.node_id || packet->portnum == MESHTASTIC_PORT_ROUTING) {
		return;
	}

	now = k_uptime_get();

	k_mutex_lock(&reliable_lock, K_FOREVER);

	for (size_t i = 0U; i < ARRAY_SIZE(reliable_entries); i++) {
		if (reliable_entries[i].id == 0U) {
			entry = &reliable_entries[i];
			break;
		}
	}

	if (entry == NULL) {
		k_mutex_unlock(&reliable_lock);
		LOG_WRN("ACK table full, id=0x%08x sent without retry", packet->id);
		return;
	}

	entry->id = packet->id;
	entry->dest = packet->to;
	entry->portnum = packet->portnum;
	entry->attempts_left = (uint8_t)CONFIG_MESHTASTIC_RELIABLE_RETRANSMISSIONS;
	entry->due_ms = now + reliable_retry_delay_ms();
	entry->wire_len = wire_len;
	memcpy(entry->wire, wire, wire_len);

	reliable_reschedule(now);

	k_mutex_unlock(&reliable_lock);

	LOG_DBG("Awaiting ACK for id=0x%08x to 0x%08x", packet->id, packet->to);
}

bool meshtastic_reliable_on_routing(const struct meshtastic_packet *packet)
{
	struct reliable_outcome outcome;
	bool found = false;
	bool acked;
	int err;

	if (packet == NULL || packet->portnum != MESHTASTIC_PORT_ROUTING ||
	    packet->to != mt.node_id || packet->request_id == 0U) {
		return false;
	}

	acked = reliable_routing_is_ack(packet->payload, packet->payload_len, &err);

	k_mutex_lock(&reliable_lock, K_FOREVER);

	for (size_t i = 0U; i < ARRAY_SIZE(reliable_entries); i++) {
		if (reliable_entries[i].id != packet->request_id) {
			continue;
		}

		outcome.id = reliable_entries[i].id;
		outcome.dest = reliable_entries[i].dest;
		outcome.portnum = reliable_entries[i].portnum;
		reliable_entries[i].id = 0U;
		found = true;
		break;
	}

	k_mutex_unlock(&reliable_lock);

	if (!found) {
		return false;
	}

	if (acked) {
		LOG_INF("ACK from 0x%08x for id=0x%08x", packet->from, outcome.id);
		reliable_emit(&outcome, MESHTASTIC_EVENT_TX_ACKED, 0);
	} else {
		LOG_WRN("NAK from 0x%08x for id=0x%08x", packet->from, outcome.id);
		reliable_emit(&outcome, MESHTASTIC_EVENT_TX_NO_ACK, err);
	}

	return true;
}

size_t meshtastic_reliable_pending(void)
{
	size_t count = 0U;

	k_mutex_lock(&reliable_lock, K_FOREVER);

	for (size_t i = 0U; i < ARRAY_SIZE(reliable_entries); i++) {
		if (reliable_entries[i].id != 0U) {
			count++;
		}
	}

	k_mutex_unlock(&reliable_lock);

	return count;
}

static void reliable_work_fn(struct k_work *work)
{
	struct reliable_outcome expired[ARRAY_SIZE(reliable_entries)];
	size_t expired_count = 0U;
	int64_t now;
	int ret;

	ARG_UNUSED(work);

	now = k_uptime_get();

	k_mutex_lock(&reliable_lock, K_FOREVER);

	for (size_t i = 0U; i < ARRAY_SIZE(reliable_entries); i++) {
		struct reliable_entry *entry = &reliable_entries[i];

		if (entry->id == 0U || entry->due_ms > now) {
			continue;
		}

		if (entry->attempts_left == 0U) {
			expired[expired_count].id = entry->id;
			expired[expired_count].dest = entry->dest;
			expired[expired_count].portnum = entry->portnum;
			expired_count++;
			entry->id = 0U;
			continue;
		}

		entry->attempts_left--;
		entry->due_ms = now + reliable_retry_delay_ms();

		/*
		 * Queued without blocking: this runs on the system work queue,
		 * which must not stall on the radio.
		 */
		ret = meshtastic_radio_send_wire(entry->wire, entry->wire_len);
		if (ret < 0) {
			LOG_WRN("Retransmit of id=0x%08x failed (%d)", entry->id, ret);
		} else {
			LOG_DBG("Retransmit id=0x%08x to 0x%08x (%u left)", entry->id, entry->dest,
				entry->attempts_left);
		}
	}

	reliable_reschedule(now);

	k_mutex_unlock(&reliable_lock);

	for (size_t i = 0U; i < expired_count; i++) {
		LOG_WRN("No ACK for id=0x%08x to 0x%08x", expired[i].id, expired[i].dest);
		reliable_emit(&expired[i], MESHTASTIC_EVENT_TX_NO_ACK, -ETIMEDOUT);
	}
}
