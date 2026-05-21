/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

/*
 * Channel and TX airtime accounting (ported from Meshtastic firmware airtime.cpp).
 */

#include <string.h>

#include <zephyr/drivers/lora.h>
#include <zephyr/kernel.h>

#include "meshtastic_airtime.h"
#include "meshtastic_core.h"

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(meshtastic, CONFIG_MESHTASTIC_LOG_LEVEL);

struct meshtastic_airtime_state {
	struct k_mutex lock;
	uint32_t channel_utilization[MESHTASTIC_CHANNEL_UTILIZATION_PERIODS];
	uint32_t utilization_tx[MESHTASTIC_MINUTES_IN_HOUR];
	uint32_t sec_since_boot;
	uint8_t last_util_period;
	uint8_t last_util_period_tx;
	bool first_tick;
};

static struct meshtastic_airtime_state airtime;

static void airtime_timer_fn(struct k_timer *timer);

K_TIMER_DEFINE(airtime_timer, airtime_timer_fn, NULL);

static uint8_t period_util_minute(void)
{
	return (uint8_t)((airtime.sec_since_boot / 10U) % MESHTASTIC_CHANNEL_UTILIZATION_PERIODS);
}

static uint8_t period_util_hour(void)
{
	return (uint8_t)((airtime.sec_since_boot / 60U) % MESHTASTIC_MINUTES_IN_HOUR);
}

static void channel_util_add(uint32_t ms)
{
	uint8_t idx = period_util_minute();

	airtime.channel_utilization[idx] += ms;
}

void meshtastic_airtime_log(enum meshtastic_airtime_type type, uint32_t ms)
{
	k_mutex_lock(&airtime.lock, K_FOREVER);

	switch (type) {
	case MESHTASTIC_AIRTIME_TX:
		LOG_DBG("Packet TX: %u ms", ms);
		airtime.utilization_tx[period_util_hour()] += ms;
		channel_util_add(ms);
		break;
	case MESHTASTIC_AIRTIME_RX:
		LOG_DBG("Packet RX: %u ms", ms);
		channel_util_add(ms);
		break;
	case MESHTASTIC_AIRTIME_RX_ALL:
		LOG_DBG("Packet RX (noise?): %u ms", ms);
		channel_util_add(ms);
		break;
	default:
		break;
	}

	k_mutex_unlock(&airtime.lock);
}

uint32_t meshtastic_airtime_packet_ms(uint32_t wire_len)
{
	if (mt.lora_dev == NULL || !device_is_ready(mt.lora_dev)) {
		return 0U;
	}

	return lora_airtime(mt.lora_dev, wire_len);
}

float meshtastic_airtime_channel_util_percent(void)
{
	uint32_t sum = 0U;
	float percent;

	k_mutex_lock(&airtime.lock, K_FOREVER);
	for (size_t i = 0; i < MESHTASTIC_CHANNEL_UTILIZATION_PERIODS; i++) {
		sum += airtime.channel_utilization[i];
	}
	k_mutex_unlock(&airtime.lock);

	percent = ((float)sum / (float)(MESHTASTIC_CHANNEL_UTILIZATION_PERIODS * 10U * 1000U)) *
		  100.0f;

	return percent;
}

float meshtastic_airtime_tx_util_percent(void)
{
	uint32_t sum = 0U;
	float percent;

	k_mutex_lock(&airtime.lock, K_FOREVER);
	for (size_t i = 0; i < MESHTASTIC_MINUTES_IN_HOUR; i++) {
		sum += airtime.utilization_tx[i];
	}
	k_mutex_unlock(&airtime.lock);

	percent = ((float)sum / (float)MESHTASTIC_MS_IN_HOUR) * 100.0f;

	return percent;
}

static void meshtastic_airtime_tick(void)
{
	uint8_t util_period;
	uint8_t util_period_tx;

	k_mutex_lock(&airtime.lock, K_FOREVER);

	airtime.sec_since_boot++;
	util_period = period_util_minute();
	util_period_tx = period_util_hour();

	if (airtime.first_tick) {
		airtime.first_tick = false;
		airtime.last_util_period = util_period;
		airtime.last_util_period_tx = util_period_tx;
	} else {
		if (airtime.last_util_period != util_period) {
			airtime.last_util_period = util_period;
			airtime.channel_utilization[util_period] = 0U;
		}

		if (airtime.last_util_period_tx != util_period_tx) {
			airtime.last_util_period_tx = util_period_tx;
			airtime.utilization_tx[util_period_tx] = 0U;
		}
	}

	k_mutex_unlock(&airtime.lock);
}

static void airtime_timer_fn(struct k_timer *timer)
{
	ARG_UNUSED(timer);

	meshtastic_airtime_tick();
}

int meshtastic_airtime_init(void)
{
	memset(&airtime, 0, sizeof(airtime));
	airtime.first_tick = true;
	k_mutex_init(&airtime.lock);
	k_timer_start(&airtime_timer, K_SECONDS(1), K_SECONDS(1));

	return 0;
}
