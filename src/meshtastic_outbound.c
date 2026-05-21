/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

/*
 * Serialises all LoRa transmits through a single worker thread so application
 * logic on the RX path and periodic telemetry cannot block each other on the
 * radio driver.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "meshtastic_outbound.h"

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(meshtastic, CONFIG_MESHTASTIC_LOG_LEVEL);

struct mt_outbound_item {
	uint8_t wire[MESHTASTIC_PKT_MAX];
	uint32_t len;
	struct k_sem *done;
	int *result;
};

K_MSGQ_DEFINE(mt_outbound_msgq, sizeof(struct mt_outbound_item),
	      CONFIG_MESHTASTIC_OUTBOUND_QUEUE_DEPTH, 4);

static K_THREAD_STACK_DEFINE(mt_outbound_stack, CONFIG_MESHTASTIC_OUTBOUND_STACK_SIZE);
static struct k_thread mt_outbound_thread;

static void mt_outbound_thread_fn(void *p1, void *p2, void *p3)
{
	struct mt_outbound_item item;
	int ret;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		(void)k_msgq_get(&mt_outbound_msgq, &item, K_FOREVER);

		ret = meshtastic_radio_send_wire_now(item.wire, item.len);

		if (item.result != NULL) {
			*item.result = ret;
		}
		if (item.done != NULL) {
			k_sem_give(item.done);
		}
	}
}

static int outbound_enqueue(const uint8_t *pkt, uint32_t pkt_len, k_timeout_t wait)
{
	struct mt_outbound_item item;
	struct k_sem sem;
	int result = 0;
	int ret;

	if (pkt == NULL || pkt_len == 0U || pkt_len > MESHTASTIC_PKT_MAX) {
		return -EINVAL;
	}

	memcpy(item.wire, pkt, pkt_len);
	item.len = pkt_len;

	if (K_TIMEOUT_EQ(wait, K_NO_WAIT)) {
		item.done = NULL;
		item.result = NULL;
		ret = k_msgq_put(&mt_outbound_msgq, &item, K_NO_WAIT);
		return (ret == 0) ? 0 : -ENOMSG;
	}

	k_sem_init(&sem, 0, 1);
	item.done = &sem;
	item.result = &result;

	ret = k_msgq_put(&mt_outbound_msgq, &item, K_FOREVER);
	if (ret < 0) {
		return ret;
	}

	ret = k_sem_take(&sem, wait);
	if (ret == -EAGAIN) {
		return -EAGAIN;
	}

	return result;
}

int meshtastic_radio_send_wire(uint8_t *pkt, uint32_t pkt_len)
{
	return outbound_enqueue(pkt, pkt_len, K_NO_WAIT);
}

int meshtastic_radio_send_wire_wait(const uint8_t *pkt, uint32_t pkt_len, k_timeout_t timeout)
{
	return outbound_enqueue(pkt, pkt_len, timeout);
}

int meshtastic_outbound_init(void)
{
	k_thread_create(&mt_outbound_thread, mt_outbound_stack,
			K_THREAD_STACK_SIZEOF(mt_outbound_stack), mt_outbound_thread_fn, NULL, NULL,
			NULL, CONFIG_MESHTASTIC_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&mt_outbound_thread, "meshtastic_tx");

	return 0;
}
