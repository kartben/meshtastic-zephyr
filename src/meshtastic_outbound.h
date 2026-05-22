/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

#ifndef ZEPHYR_SUBSYS_MESHTASTIC_OUTBOUND_H_
#define ZEPHYR_SUBSYS_MESHTASTIC_OUTBOUND_H_

#include <stdint.h>

#include <zephyr/kernel.h>

#include "meshtastic_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Queue a wire frame for transmission on the Meshtastic radio worker thread.
 * Returns 0 when queued, -ENOMSG if the async TX queue is full, and does not
 * block on the radio driver.
 */
int meshtastic_radio_send_wire(const uint8_t *pkt, uint32_t pkt_len);

/*
 * Send a wire frame synchronously, serializing with the radio state lock.
 * When called from the radio worker thread this transmits inline to avoid
 * self-deadlock.  Returns -EAGAIN if @p timeout expires before radio access is
 * acquired.
 */
int meshtastic_radio_send_wire_wait(const uint8_t *pkt, uint32_t pkt_len, k_timeout_t timeout);

/* Driver-level TX; used by the radio worker and synchronous send path. */
int meshtastic_radio_send_wire_now(const uint8_t *pkt, uint32_t pkt_len);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_SUBSYS_MESHTASTIC_OUTBOUND_H_ */
