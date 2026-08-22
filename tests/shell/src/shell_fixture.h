/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

/**
 * @file
 * @brief Shared setup for the shell suites.
 */

#ifndef MESHTASTIC_TESTS_SHELL_FIXTURE_H_
#define MESHTASTIC_TESTS_SHELL_FIXTURE_H_

#include <stddef.h>

#include <zephyr/meshtastic/meshtastic.h>

#include "meshtastic_core.h"

#define SHELL_TEST_NODE_ID 0x12345678U
#define SHELL_TEST_PEER_ID 0x87654321U

#define SHELL_TEST_LONG_NAME  "Zephyr Test Node"
#define SHELL_TEST_SHORT_NAME "ZTN"

/** The configuration the stack was brought up with. */
extern struct meshtastic_config shell_test_cfg;

/** Bring the stack and the dummy shell backend up. */
void *shell_test_setup(void);

/** Put configuration, radio and captured output back to a known state. */
void shell_test_before(void *fixture);

/** Run one command and return the shell's exit code. */
int shell_run(const char *cmd);

/** Fail unless @p needle shows up in what the last command printed. */
void shell_expect(const char *needle);

/** Wait for the frame a deferred command sends, and decode it. */
void shell_deferred_tx(struct meshtastic_packet *packet, uint8_t *payload, size_t payload_len);

#endif /* MESHTASTIC_TESTS_SHELL_FIXTURE_H_ */
