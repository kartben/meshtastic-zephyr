/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

/**
 * @file
 * @brief Shared bring-up and helpers for the port module suites.
 */

#ifndef MESHTASTIC_TESTS_MODULES_FIXTURE_H_
#define MESHTASTIC_TESTS_MODULES_FIXTURE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/meshtastic/meshtastic.h>

#include "mock_lora.h"

#define TEST_NODE_ID 0x12345678U

/** Bring the stack up once on the mock radio; usable as a ztest suite setup. */
void *modules_suite_setup(void);

/** Forget the recorded transmits and events. */
void modules_reset(void);

/**
 * @brief Introduce @p node to the stack by delivering its NodeInfo.
 *
 * Peers the stack has never heard of are probed for their identity, which
 * would otherwise show up as an extra transmit in unrelated tests.
 */
void modules_introduce_peer(uint32_t node);

/** Number of stack events seen since the last reset. */
uint32_t modules_event_count(void);

/** Copy the most recent stack event, returning false when none was raised. */
bool modules_last_event(struct meshtastic_event *event, struct meshtastic_packet *packet,
			uint8_t *payload, size_t payload_len);

/**
 * @brief Wait for one transmit and decode it back into a packet.
 *
 * Fails the test if the radio was not handed exactly @p expected frames.
 */
void modules_decode_tx(uint32_t expected, struct meshtastic_packet *packet, uint8_t *payload,
		       size_t payload_len);

#endif /* MESHTASTIC_TESTS_MODULES_FIXTURE_H_ */
