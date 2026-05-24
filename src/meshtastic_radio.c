/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

/*
 * LoRa radio access, RX handoff, and TX/RX state serialization.
 */

#include <string.h>

#include <zephyr/drivers/lora.h>
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/stats/stats.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "meshtastic_core.h"
#include "meshtastic_outbound.h"
#include "meshtastic_packet.h"
#include "meshtastic_router.h"
#include "meshtastic_airtime.h"

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(meshtastic, CONFIG_MESHTASTIC_LOG_LEVEL);

static struct lora_modem_config mt_lora_cfg = {
	.bandwidth = BW_250_KHZ,
	.datarate = SF_11,
	.coding_rate = CR_4_5,
	.preamble_len = 16U,
	.iq_inverted = false,
	.public_network = false,
	.sync_word = 0x2b,
	.cad =
		{
			.mode = LORA_CAD_MODE_NONE,
			.symbol_num = 0,
		},
};

static K_THREAD_STACK_DEFINE(mt_stack, CONFIG_MESHTASTIC_THREAD_STACK_SIZE);
static struct k_thread mt_thread;

#if defined(CONFIG_MESHTASTIC_STATS)
STATS_SECT_START(meshtastic_stats)
STATS_SECT_ENTRY(tx_packets)
STATS_SECT_ENTRY(tx_failures)
STATS_SECT_ENTRY(rx_packets)
STATS_SECT_ENTRY(relayed_packets)
STATS_SECT_ENTRY(duplicate_packets)
STATS_SECT_ENTRY(decode_failures)
STATS_SECT_ENTRY(rx_dropped)
STATS_SECT_ENTRY(rx_rearm_failures)
STATS_SECT_ENTRY(packet_loss)
STATS_SECT_ENTRY(last_rx_from)
STATS_SECT_ENTRY(last_rssi)
STATS_SECT_ENTRY(last_snr)
STATS_SECT_END;

STATS_NAME_START(meshtastic_stats)
STATS_NAME(meshtastic_stats, tx_packets)
STATS_NAME(meshtastic_stats, tx_failures)
STATS_NAME(meshtastic_stats, rx_packets)
STATS_NAME(meshtastic_stats, relayed_packets)
STATS_NAME(meshtastic_stats, duplicate_packets)
STATS_NAME(meshtastic_stats, decode_failures)
STATS_NAME(meshtastic_stats, rx_dropped)
STATS_NAME(meshtastic_stats, rx_rearm_failures)
STATS_NAME(meshtastic_stats, packet_loss)
STATS_NAME(meshtastic_stats, last_rx_from)
STATS_NAME(meshtastic_stats, last_rssi)
STATS_NAME(meshtastic_stats, last_snr)
STATS_NAME_END(meshtastic_stats);

STATS_SECT_DECL(meshtastic_stats) meshtastic_stats;
#endif

#if defined(CONFIG_MESHTASTIC_STATS)
#define MT_STATS_DO(...)                                                                            \
	do {                                                                                       \
		__VA_ARGS__                                                                        \
	} while (0)
#else
#define MT_STATS_DO(...)                                                                            \
	do {                                                                                       \
	} while (0)
#endif

void meshtastic_stats_record_tx_done(void)
{
	MT_STATS_DO(STATS_INC(meshtastic_stats, tx_packets););
}

void meshtastic_stats_record_tx_failure(void)
{
	MT_STATS_DO(STATS_INC(meshtastic_stats, tx_failures); STATS_INC(meshtastic_stats, packet_loss););
}

void meshtastic_stats_record_rx_drop(void)
{
	MT_STATS_DO(STATS_INC(meshtastic_stats, rx_dropped); STATS_INC(meshtastic_stats, packet_loss););
}

void meshtastic_stats_record_rx_rearm_failure(void)
{
	MT_STATS_DO(STATS_INC(meshtastic_stats, rx_rearm_failures););
}

void meshtastic_stats_record_duplicate(void)
{
	MT_STATS_DO(STATS_INC(meshtastic_stats, duplicate_packets););
}

void meshtastic_stats_record_relayed(void)
{
	MT_STATS_DO(STATS_INC(meshtastic_stats, relayed_packets););
}

void meshtastic_stats_record_decode_failure(void)
{
	MT_STATS_DO(STATS_INC(meshtastic_stats, decode_failures););
}

void meshtastic_stats_record_rx(uint32_t src, int16_t rssi, int8_t snr)
{
	MT_STATS_DO(STATS_INC(meshtastic_stats, rx_packets);
		    meshtastic_stats.s.last_rx_from = src;
		    meshtastic_stats.s.last_rssi = (uint32_t)(int32_t)rssi;
		    meshtastic_stats.s.last_snr = (uint32_t)(int32_t)snr;);
}

void meshtastic_stats_fill_status(struct meshtastic_status *status)
{
	MT_STATS_DO(status->tx_packets = meshtastic_stats.s.tx_packets;
		    status->tx_failures = meshtastic_stats.s.tx_failures;
		    status->rx_packets = meshtastic_stats.s.rx_packets;
		    status->relayed_packets = meshtastic_stats.s.relayed_packets;
		    status->duplicate_packets = meshtastic_stats.s.duplicate_packets;
		    status->decode_failures = meshtastic_stats.s.decode_failures;
		    status->rx_dropped = meshtastic_stats.s.rx_dropped;
		    status->rx_rearm_failures = meshtastic_stats.s.rx_rearm_failures;
		    status->last_rx_from = meshtastic_stats.s.last_rx_from;
		    status->last_rssi = (int16_t)(int32_t)meshtastic_stats.s.last_rssi;
		    status->last_snr = (int8_t)(int32_t)meshtastic_stats.s.last_snr;);
}

/*
 * Serialises radio state transitions.  Continuous async RX runs in the LoRa
 * driver; only TX and the surrounding stop/re-arm of async RX touch radio
 * state and must not race each other (the SX126x driver rejects lora_send()
 * /lora_config() with -EBUSY while async RX is active).
 */
static K_SEM_DEFINE(mt_radio_sem, 1, 1);

/* Raw frame handed from the driver RX callback to the processing thread. */
struct mt_rx_slot {
	uint16_t len;
	int16_t rssi;
	int8_t snr;
	uint8_t buf[MESHTASTIC_PKT_MAX];
};

K_MSGQ_DEFINE(mt_rx_msgq, sizeof(struct mt_rx_slot), CONFIG_MESHTASTIC_RX_QUEUE_DEPTH, 4);

static void mt_rx_cb(const struct device *dev, uint8_t *data, uint16_t size, int16_t rssi,
		     int8_t snr, void *user_data);

#if defined(CONFIG_MESHTASTIC_PACKET_HEXDUMP)
static void log_wire_tx(const uint8_t *pkt, uint32_t pkt_len)
{
	const struct meshtastic_wire_header *hdr = (const struct meshtastic_wire_header *)pkt;

	LOG_DBG("LoRa TX %08x->%08x id=%08x ch=0x%02x len=%u",
		(unsigned int)sys_le32_to_cpu(hdr->src), (unsigned int)sys_le32_to_cpu(hdr->dest),
		(unsigned int)sys_le32_to_cpu(hdr->id), hdr->channel, (unsigned int)pkt_len);
	LOG_HEXDUMP_DBG(pkt, pkt_len, "LoRa TX");
}
#endif /* CONFIG_MESHTASTIC_PACKET_HEXDUMP */

static int mt_radio_arm_rx(void)
{
	int ret = lora_recv_async(mt.lora_dev, mt_rx_cb, NULL);

	if (ret < 0) {
		mt.radio_rx_armed = false;
		meshtastic_stats_record_rx_rearm_failure();
		LOG_ERR("lora_recv_async arm failed (%d)", ret);
	} else {
		mt.radio_rx_armed = true;
	}

	return ret;
}

static uint32_t mt_busy_backoff_ms(void)
{
	uint32_t min_ms = CONFIG_MESHTASTIC_TX_BUSY_BACKOFF_MIN_MS;
	uint32_t max_ms = CONFIG_MESHTASTIC_TX_BUSY_BACKOFF_MAX_MS;

	if (max_ms <= min_ms) {
		return min_ms;
	}

	return min_ms + (sys_rand32_get() % (max_ms - min_ms + 1U));
}

int meshtastic_radio_send_wire_now(uint8_t *pkt, uint32_t pkt_len)
{
	int ret;
	int retries;
	int busy_retries = 0;

#if defined(CONFIG_MESHTASTIC_PACKET_HEXDUMP)
	log_wire_tx(pkt, pkt_len);
#endif

	(void)k_sem_take(&mt_radio_sem, K_FOREVER);

	/*
	 * Continuous async RX must be stopped first: the SX126x driver
	 * rejects lora_config()/lora_send() with -EBUSY while it is active.
	 */
	(void)lora_recv_async(mt.lora_dev, NULL, NULL);
	mt.radio_rx_armed = false;

	k_mutex_lock(&mt.lock, K_FOREVER);

	mt_lora_cfg.frequency = mt.frequency;
	mt_lora_cfg.tx_power = mt.tx_power;
	mt_lora_cfg.tx = true;
	mt_lora_cfg.cad.mode = LORA_CAD_MODE_LBT;
	mt_lora_cfg.cad.symbol_num = LORA_CAD_SYMB_2;

	ret = lora_config(mt.lora_dev, &mt_lora_cfg);
	if (ret == 0) {
		retries = CONFIG_MESHTASTIC_TX_BUSY_RETRIES;
		for (;;) {
			ret = lora_send(mt.lora_dev, pkt, pkt_len);
			if (ret != -EBUSY || retries == 0) {
				break;
			}

			retries--;
			busy_retries++;
			k_sleep(K_MSEC(mt_busy_backoff_ms()));
		}
	}

	mt_lora_cfg.tx = false;
	mt_lora_cfg.cad.mode = LORA_CAD_MODE_NONE;
	(void)lora_config(mt.lora_dev, &mt_lora_cfg);

	k_mutex_unlock(&mt.lock);

	(void)mt_radio_arm_rx();

	(void)k_sem_give(&mt_radio_sem);

	if (busy_retries > 0) {
		LOG_DBG("TX deferred by CAD busy channel (%d retries)", busy_retries);
	}

	if (ret < 0) {
		if (ret == -EBUSY) {
			LOG_DBG("TX failed: channel busy after retries exhausted");
		}
		meshtastic_stats_record_tx_failure();
		meshtastic_emit_event(MESHTASTIC_EVENT_TX_FAILED, ret, NULL);
	} else {
		meshtastic_stats_record_tx_done();
#if defined(CONFIG_MESHTASTIC_AIRTIME)
		meshtastic_airtime_log(MESHTASTIC_AIRTIME_TX,
				       meshtastic_airtime_packet_ms(pkt_len));
#endif
	}

	return ret;
}

/*
 * LoRa driver receive callback.  Runs on the driver's system workqueue (not
 * an ISR), and the driver auto-restarts continuous RX as soon as this returns
 * - so do the minimum: copy the frame out (the driver reuses its RX buffer
 * immediately) and hand it to the processing thread.
 */
static void mt_rx_cb(const struct device *dev, uint8_t *data, uint16_t size, int16_t rssi,
		     int8_t snr, void *user_data)
{
	struct mt_rx_slot slot;

	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	if (size == 0U || size > sizeof(slot.buf)) {
		return;
	}

	slot.len = size;
	slot.rssi = rssi;
	slot.snr = snr;
	memcpy(slot.buf, data, size);

	if (k_msgq_put(&mt_rx_msgq, &slot, K_NO_WAIT) != 0) {
		meshtastic_stats_record_rx_drop();
		LOG_DBG("RX queue full, dropped %u-byte frame", size);
	}
}

static void mt_thread_fn(void *p1, void *p2, void *p3)
{
	struct mt_rx_slot slot;
	int ret;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		ret = k_msgq_get(&mt_rx_msgq, &slot, K_MSEC(CONFIG_MESHTASTIC_RX_REARM_RETRY_MS));
		if (ret == 0) {
			meshtastic_router_process_lora_rx(slot.buf, slot.len, slot.rssi, slot.snr);
			continue;
		}

		/*
		 * No packet within the retry window.  If a TX left async RX
		 * un-armed (re-arm failed), the radio is deaf - recover it.
		 */
		if (!mt.radio_rx_armed) {
			(void)k_sem_take(&mt_radio_sem, K_FOREVER);
			if (!mt.radio_rx_armed) {
				(void)mt_radio_arm_rx();
			}
			(void)k_sem_give(&mt_radio_sem);
		}
	}
}

int meshtastic_radio_init(void)
{
	int ret;

	mt_lora_cfg.frequency = mt.frequency;
	mt_lora_cfg.tx_power = mt.tx_power;
	mt_lora_cfg.tx = false;

	ret = lora_config(mt.lora_dev, &mt_lora_cfg);
	if (ret < 0) {
		LOG_ERR("Initial lora_config failed (%d)", ret);
		return -EIO;
	}

	ret = meshtastic_outbound_init();
	if (ret < 0) {
		return ret;
	}

	k_thread_create(&mt_thread, mt_stack, K_THREAD_STACK_SIZEOF(mt_stack), mt_thread_fn, NULL,
			NULL, NULL, CONFIG_MESHTASTIC_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&mt_thread, "meshtastic");
#if defined(CONFIG_MESHTASTIC_STATS)
	ret = STATS_INIT_AND_REG(meshtastic_stats, STATS_SIZE_32, "meshtastic",
				 &meshtastic_stats);
	if (ret < 0) {
		LOG_WRN("Failed to register meshtastic communication stats (%d)", ret);
	}
#endif

	/*
	 * Arm continuous async RX.  A failure here is non-fatal: TX still
	 * works and the processing thread re-attempts the arm periodically.
	 */
	(void)k_sem_take(&mt_radio_sem, K_FOREVER);
	(void)mt_radio_arm_rx();
	(void)k_sem_give(&mt_radio_sem);

	return 0;
}
