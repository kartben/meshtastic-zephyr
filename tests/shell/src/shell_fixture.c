/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_dummy.h>
#include <zephyr/ztest.h>

#include "meshtastic_channels.h"
#include "meshtastic_config_store.h"
#include "meshtastic_packet.h"

#include "mock_lora.h"
#include "shell_fixture.h"

struct meshtastic_config shell_test_cfg = {
	.node_id = SHELL_TEST_NODE_ID,
	.psk = meshtastic_default_psk,
	.psk_len = sizeof(meshtastic_default_psk),
	.channel_name = MESHTASTIC_CHANNEL_LONGFAST,
	.frequency = MESHTASTIC_FREQ_EU,
	.long_name = SHELL_TEST_LONG_NAME,
	.short_name = SHELL_TEST_SHORT_NAME,
};

static const struct shell *sh;
static char captured[4096];
static size_t captured_len;

/*
 * Reading the dummy backend drains it, so keep our own copy: a command's
 * output can arrive in several chunks once the shell thread flushes.
 */
static bool output_contains(const char *needle)
{
	size_t len;
	const char *chunk = shell_backend_dummy_get_output(sh, &len);

	if (len > 0U && (captured_len + len) < sizeof(captured)) {
		memcpy(&captured[captured_len], chunk, len);
		captured_len += len;
		captured[captured_len] = '\0';
	}

	return strstr(captured, needle) != NULL;
}

void shell_expect(const char *needle)
{
	WAIT_FOR(output_contains(needle), 500000, k_msleep(1));
	zassert_true(output_contains(needle), "expected \"%s\" in:\n%s", needle, captured);
}

/* Run a command and return the shell's exit code. */
int shell_run(const char *cmd)
{
	/* Let whatever the previous command printed land before dropping it. */
	k_msleep(20);
	shell_backend_dummy_clear_output(sh);
	captured_len = 0U;
	captured[0] = '\0';

	return shell_execute_cmd(sh, cmd);
}

/* Deferred commands are handed to a worker thread; wait for the frame it sends. */
void shell_deferred_tx(struct meshtastic_packet *packet, uint8_t *payload,
		       size_t payload_len)
{
	uint8_t wire[MESHTASTIC_PKT_MAX];
	uint32_t wire_len;

	zassert_true(mock_lora_wait_for_send_count(1U, K_MSEC(1000)),
		     "the radio did not transmit in time");
	wire_len = mock_lora_last_tx(wire, sizeof(wire));
	zassert_ok(meshtastic_decode_wire_packet(wire, (int)wire_len, 0, 0, packet, payload,
						 payload_len),
		   "could not decode the transmitted frame");
}

void *shell_test_setup(void)
{
	if (sh != NULL) {
		return NULL;
	}

	shell_test_cfg.lora_dev = mock_lora_device();
	zassert_ok(meshtastic_init(&shell_test_cfg), "meshtastic_init failed");

	sh = shell_backend_dummy_get_ptr();
	zassert_not_null(sh, "dummy shell backend missing");
	/* The dummy backend needs one processing pass before it accepts commands. */
	shell_process(sh);

	return NULL;
}

void shell_test_before(void *fixture)
{
	ARG_UNUSED(fixture);

	zassert_ok(meshtastic_channels_init_from_config(&shell_test_cfg));
	zassert_ok(meshtastic_config_store_seed(&shell_test_cfg));
	meshtastic_set_device_role(meshtastic_Config_DeviceConfig_Role_CLIENT);
	meshtastic_set_rebroadcast_mode(meshtastic_Config_DeviceConfig_RebroadcastMode_ALL);
	mock_lora_reset();
	shell_backend_dummy_clear_output(sh);
	captured_len = 0U;
	captured[0] = '\0';
}
