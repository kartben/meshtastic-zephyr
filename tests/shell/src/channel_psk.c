/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

/**
 * @file
 * @brief How "channel show" describes each kind of pre-shared key.
 */

#include <zephyr/ztest.h>

#include "shell_fixture.h"

#define KEY_128 "000102030405060708090a0b0c0d0e0f"
#define KEY_256 KEY_128 KEY_128

ZTEST_SUITE(shell_channel_psk, NULL, shell_test_setup, shell_test_before, NULL, NULL);

ZTEST(shell_channel_psk, test_a_primary_without_a_key_is_cleartext)
{
	zassert_ok(shell_run("meshtastic channel set 0 psk none"));

	zassert_ok(shell_run("meshtastic channel show 0"));
	shell_expect("psk: cleartext\r\n");
}

ZTEST(shell_channel_psk, test_a_secondary_without_a_key_inherits_the_primary)
{
	zassert_ok(shell_run("meshtastic channel set 1 role secondary name Private psk none"));

	zassert_ok(shell_run("meshtastic channel show 1"));
	shell_expect("psk: inherit primary\r\n");
}

ZTEST(shell_channel_psk, test_the_default_key_is_shorthand_one)
{
	zassert_ok(shell_run("meshtastic channel set 0 psk default"));

	zassert_ok(shell_run("meshtastic channel show 0"));
	shell_expect("psk: shorthand 1\r\n");
}

ZTEST(shell_channel_psk, test_a_numbered_key_is_shorthand)
{
	zassert_ok(shell_run("meshtastic channel set 0 psk 7"));

	zassert_ok(shell_run("meshtastic channel show 0"));
	shell_expect("psk: shorthand 7\r\n");
}

ZTEST(shell_channel_psk, test_a_128_bit_key_is_printed_in_full)
{
	zassert_ok(shell_run("meshtastic channel set 0 psk hex " KEY_128));

	zassert_ok(shell_run("meshtastic channel show 0"));
	shell_expect("psk: 16-byte key\r\n");
	shell_expect("psk hex: " KEY_128 "\r\n");
}

ZTEST(shell_channel_psk, test_a_256_bit_key_is_printed_in_full)
{
	zassert_ok(shell_run("meshtastic channel set 0 psk hex " KEY_256));

	zassert_ok(shell_run("meshtastic channel show 0"));
	shell_expect("psk: 32-byte key\r\n");
	shell_expect("psk hex: " KEY_256 "\r\n");
}

ZTEST(shell_channel_psk, test_a_disabled_channel_reports_no_usable_key)
{
	zassert_ok(shell_run("meshtastic channel set 1 role secondary name Private psk default"));
	zassert_ok(shell_run("meshtastic channel disable 1"));

	zassert_ok(shell_run("meshtastic channel show 1"));
	shell_expect("psk: n/a\r\n");
}
