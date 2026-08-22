/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/lora.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/__assert.h>

#include "mock_lora.h"

#define MOCK_LORA_FRAME_MAX 255U

static struct {
	struct k_mutex lock;
	struct lora_modem_config config;
	lora_recv_cb rx_cb;
	void *rx_user_data;
	int send_result;
	uint32_t send_count;
	uint32_t config_count;
	uint8_t last_tx[MOCK_LORA_FRAME_MAX];
	uint32_t last_tx_len;
} mock;

static int mock_lora_init(const struct device *dev)
{
	ARG_UNUSED(dev);

	k_mutex_init(&mock.lock);
	mock.send_result = 0;

	return 0;
}

static int mock_lora_config(const struct device *dev, const struct lora_modem_config *config)
{
	ARG_UNUSED(dev);

	k_mutex_lock(&mock.lock, K_FOREVER);
	mock.config = *config;
	mock.config_count++;
	k_mutex_unlock(&mock.lock);

	return 0;
}

static uint32_t mock_lora_airtime(const struct device *dev, uint32_t data_len)
{
	ARG_UNUSED(dev);

	return data_len;
}

static int mock_lora_send(const struct device *dev, uint8_t *data, uint32_t data_len)
{
	int ret;

	ARG_UNUSED(dev);

	k_mutex_lock(&mock.lock, K_FOREVER);
	__ASSERT(data_len <= sizeof(mock.last_tx), "unexpected tx len %u", data_len);
	memcpy(mock.last_tx, data, data_len);
	mock.last_tx_len = data_len;
	mock.send_count++;
	ret = mock.send_result;
	k_mutex_unlock(&mock.lock);

	return ret;
}

static int mock_lora_send_async(const struct device *dev, uint8_t *data, uint32_t data_len,
				struct k_poll_signal *async)
{
	int ret = mock_lora_send(dev, data, data_len);

	if (async != NULL) {
		k_poll_signal_raise(async, ret);
	}

	return ret;
}

static int mock_lora_recv(const struct device *dev, uint8_t *data, uint8_t size,
			  k_timeout_t timeout, int16_t *rssi, int8_t *snr)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(data);
	ARG_UNUSED(size);
	ARG_UNUSED(timeout);
	ARG_UNUSED(rssi);
	ARG_UNUSED(snr);

	return -ENOTSUP;
}

static int mock_lora_recv_async(const struct device *dev, lora_recv_cb cb, void *user_data)
{
	ARG_UNUSED(dev);

	k_mutex_lock(&mock.lock, K_FOREVER);
	mock.rx_cb = cb;
	mock.rx_user_data = user_data;
	k_mutex_unlock(&mock.lock);

	return 0;
}

static DEVICE_API(lora, mock_lora_api) = {
	.config = mock_lora_config,
	.airtime = mock_lora_airtime,
	.send = mock_lora_send,
	.send_async = mock_lora_send_async,
	.recv = mock_lora_recv,
	.recv_async = mock_lora_recv_async,
};

DEVICE_DEFINE(mock_lora, "mock_lora", mock_lora_init, NULL, NULL, NULL, POST_KERNEL,
	      CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &mock_lora_api);

const struct device *mock_lora_device(void)
{
	return DEVICE_GET(mock_lora);
}

void mock_lora_reset(void)
{
	k_mutex_lock(&mock.lock, K_FOREVER);
	memset(&mock.config, 0, sizeof(mock.config));
	mock.send_result = 0;
	mock.send_count = 0U;
	mock.config_count = 0U;
	mock.last_tx_len = 0U;
	memset(mock.last_tx, 0, sizeof(mock.last_tx));
	k_mutex_unlock(&mock.lock);
}

void mock_lora_set_send_result(int result)
{
	k_mutex_lock(&mock.lock, K_FOREVER);
	mock.send_result = result;
	k_mutex_unlock(&mock.lock);
}

uint32_t mock_lora_send_count(void)
{
	uint32_t count;

	k_mutex_lock(&mock.lock, K_FOREVER);
	count = mock.send_count;
	k_mutex_unlock(&mock.lock);

	return count;
}

uint32_t mock_lora_config_count(void)
{
	uint32_t count;

	k_mutex_lock(&mock.lock, K_FOREVER);
	count = mock.config_count;
	k_mutex_unlock(&mock.lock);

	return count;
}

uint32_t mock_lora_last_tx(uint8_t *out, size_t out_len)
{
	uint32_t len;

	k_mutex_lock(&mock.lock, K_FOREVER);
	len = mock.last_tx_len;
	if (out != NULL) {
		__ASSERT(len <= out_len, "tx buffer too small (%u > %zu)", len, out_len);
		memcpy(out, mock.last_tx, len);
	}
	k_mutex_unlock(&mock.lock);

	return len;
}

void mock_lora_last_config(struct lora_modem_config *out)
{
	k_mutex_lock(&mock.lock, K_FOREVER);
	*out = mock.config;
	k_mutex_unlock(&mock.lock);
}

bool mock_lora_wait_for_send_count(uint32_t expected, k_timeout_t timeout)
{
	const int64_t deadline = k_uptime_get() + k_ticks_to_ms_floor64(timeout.ticks);

	do {
		if (mock_lora_send_count() >= expected) {
			return true;
		}
		k_sleep(K_MSEC(10));
	} while (k_uptime_get() <= deadline);

	return false;
}

void mock_lora_inject_rx(const uint8_t *wire, uint32_t wire_len, int16_t rssi, int8_t snr)
{
	uint8_t frame[MOCK_LORA_FRAME_MAX];
	lora_recv_cb cb;
	void *user_data;

	__ASSERT(wire_len <= sizeof(frame), "unexpected rx len %u", wire_len);

	k_mutex_lock(&mock.lock, K_FOREVER);
	cb = mock.rx_cb;
	user_data = mock.rx_user_data;
	k_mutex_unlock(&mock.lock);

	__ASSERT(cb != NULL, "radio receive callback not armed");

	/* The driver API hands the stack a mutable buffer; keep the caller's intact. */
	memcpy(frame, wire, wire_len);
	cb(mock_lora_device(), frame, (uint16_t)wire_len, rssi, snr, user_data);
}
