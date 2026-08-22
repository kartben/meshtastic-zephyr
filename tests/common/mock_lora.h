/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

/**
 * @file
 * @brief In-process LoRa transceiver used by the Meshtastic test suites.
 *
 * Records every frame handed to @c lora_send() and lets a test push wire
 * frames back up through the driver's asynchronous receive callback.
 */

#ifndef MESHTASTIC_TESTS_MOCK_LORA_H_
#define MESHTASTIC_TESTS_MOCK_LORA_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/lora.h>
#include <zephyr/kernel.h>

/** Handle of the mock transceiver, ready to pass as @c meshtastic_config.lora_dev. */
const struct device *mock_lora_device(void);

/** Drop the recorded transmits and counters and restore a successful send result. */
void mock_lora_reset(void);

/** Value returned by subsequent @c lora_send() calls. */
void mock_lora_set_send_result(int result);

/** Number of frames handed to the radio since the last reset. */
uint32_t mock_lora_send_count(void);

/** Number of @c lora_config() calls since the last reset. */
uint32_t mock_lora_config_count(void);

/**
 * @brief Copy the most recently transmitted wire frame.
 *
 * @param out     Destination buffer, or NULL to only query the length.
 * @param out_len Capacity of @p out.
 * @return Length of the recorded frame.
 */
uint32_t mock_lora_last_tx(uint8_t *out, size_t out_len);

/** Modem configuration from the most recent @c lora_config() call. */
void mock_lora_last_config(struct lora_modem_config *out);

/** Block until @p expected frames have been transmitted; false on timeout. */
bool mock_lora_wait_for_send_count(uint32_t expected, k_timeout_t timeout);

/** Deliver @p wire to the stack as if the radio had received it. */
void mock_lora_inject_rx(const uint8_t *wire, uint32_t wire_len, int16_t rssi, int8_t snr);

#endif /* MESHTASTIC_TESTS_MOCK_LORA_H_ */
