/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

/**
 * @file
 * @brief Shared setup for the settings suites.
 */

#ifndef MESHTASTIC_TESTS_SETTINGS_FIXTURE_H_
#define MESHTASTIC_TESTS_SETTINGS_FIXTURE_H_

#include <zephyr/meshtastic/meshtastic.h>

#define SETTINGS_TEST_NODE_ID 0x12345678U

/** The configuration the stack was brought up with. */
extern struct meshtastic_config settings_test_cfg;

/** Bring the stack up once, storage included. */
void *settings_test_setup(void);

/**
 * @brief Drop everything the store holds and start again from the defaults.
 *
 * Anything that survives this and a settings_load_subtree() came back from
 * storage rather than from memory.
 */
void settings_test_forget_ram(void);

/** Reload the Meshtastic subtree from storage. */
void settings_test_reload(void);

#endif /* MESHTASTIC_TESTS_SETTINGS_FIXTURE_H_ */
