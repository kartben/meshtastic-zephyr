/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

/**
 * @file
 * @brief Shared stack bring-up for the phone API suites.
 */

#ifndef MESHTASTIC_TESTS_PHONEAPI_FIXTURE_H_
#define MESHTASTIC_TESTS_PHONEAPI_FIXTURE_H_

#include "mock_lora.h"

#define TEST_NODE_ID 0x12345678U
#define PEER_NODE_ID 0x87654321U

/** Bring the stack up once on the mock radio; usable as a ztest suite setup. */
void *phoneapi_suite_setup(void);

#endif /* MESHTASTIC_TESTS_PHONEAPI_FIXTURE_H_ */
