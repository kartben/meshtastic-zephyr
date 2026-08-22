/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

/**
 * @file
 * @brief The hooks modules register to react to freshly loaded settings.
 *
 * The hooks below are this test's own. They are linked into the iterable
 * section in symbol-name order, so a_ok runs before c_last.
 */

#include <errno.h>

#include <zephyr/ztest.h>

#include "meshtastic_core.h"

#include "settings_fixture.h"

static int first_result;
static uint32_t first_runs;
static uint32_t last_runs;

static int apply_first(void)
{
	first_runs++;

	return first_result;
}

static int apply_last(void)
{
	last_runs++;

	return 0;
}

MESHTASTIC_SETTINGS_APPLY_DEFINE(a_ok, apply_first);
MESHTASTIC_SETTINGS_APPLY_DEFINE(b_no_callback, NULL);
MESHTASTIC_SETTINGS_APPLY_DEFINE(c_last, apply_last);

static void before(void *fixture)
{
	ARG_UNUSED(fixture);

	first_result = 0;
	first_runs = 0U;
	last_runs = 0U;
}

ZTEST_SUITE(settings_apply, NULL, settings_test_setup, before, NULL, NULL);

ZTEST(settings_apply, test_every_hook_with_a_callback_runs)
{
	zassert_ok(meshtastic_settings_apply_all());

	zassert_equal(first_runs, 1U);
	zassert_equal(last_runs, 1U, "a hook with no callback must not stop the walk");
}

ZTEST(settings_apply, test_a_failing_hook_stops_the_walk)
{
	first_result = -EIO;

	zassert_equal(meshtastic_settings_apply_all(), -EIO);

	zassert_equal(first_runs, 1U);
	zassert_equal(last_runs, 0U, "hooks after a failure must not run");
}
