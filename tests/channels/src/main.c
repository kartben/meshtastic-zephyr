/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <zephyr/meshtastic/meshtastic.h>

#include "meshtastic_channels.h"
#include "meshtastic_core.h"

#include "mock_lora.h"

#define TEST_NODE_ID 0x12345678U
#define PEER_NODE_ID 0x87654321U

/* The hash every Meshtastic device puts on air for the default LongFast channel. */
#define LONGFAST_HASH 0x08U

static struct meshtastic_config cfg = {
	.node_id = TEST_NODE_ID,
	.psk = meshtastic_default_psk,
	.psk_len = sizeof(meshtastic_default_psk),
	.channel_name = MESHTASTIC_CHANNEL_LONGFAST,
	.frequency = MESHTASTIC_FREQ_EU,
};

static uint8_t xor_hash(const uint8_t *data, size_t len)
{
	uint8_t code = 0U;

	for (size_t i = 0; i < len; i++) {
		code ^= data[i];
	}

	return code;
}

static void set_slot(uint8_t index, meshtastic_Channel_Role role, const char *name,
		     const uint8_t *psk, size_t psk_len)
{
	meshtastic_Channel ch = meshtastic_Channel_init_zero;

	ch.role = role;
	ch.has_settings = true;
	if (name != NULL) {
		strncpy(ch.settings.name, name, sizeof(ch.settings.name) - 1U);
	}
	if (psk_len > 0U) {
		memcpy(ch.settings.psk.bytes, psk, psk_len);
	}
	ch.settings.psk.size = (pb_size_t)psk_len;

	zassert_ok(meshtastic_channels_set_slot(index, &ch), "set_slot(%u) failed", index);
}

static void *setup(void)
{
	cfg.lora_dev = mock_lora_device();
	zassert_ok(meshtastic_init(&cfg), "meshtastic_init failed");

	return NULL;
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);

	zassert_ok(meshtastic_channels_init_from_config(&cfg), "channel reset failed");
	meshtastic_set_device_role(meshtastic_Config_DeviceConfig_Role_CLIENT);
	meshtastic_set_rebroadcast_mode(meshtastic_Config_DeviceConfig_RebroadcastMode_ALL);
}

ZTEST_SUITE(channels, NULL, setup, before, NULL, NULL);

ZTEST(channels, test_defaults_match_the_longfast_primary_channel)
{
	const meshtastic_Channel *primary = meshtastic_channels_get(0U);

	zassert_equal(meshtastic_channels_count(), MESHTASTIC_MAX_CHANNELS);
	zassert_equal(meshtastic_channels_primary_index(), 0U);
	zassert_equal(primary->role, meshtastic_Channel_Role_PRIMARY);
	zassert_str_equal(meshtastic_channels_primary_name(), MESHTASTIC_CHANNEL_LONGFAST);
	zassert_mem_equal(primary->settings.psk.bytes, meshtastic_default_psk,
			  sizeof(meshtastic_default_psk));
	zassert_true(primary->settings.uplink_enabled);
	zassert_true(primary->settings.downlink_enabled);
	zassert_equal(primary->settings.module_settings.position_precision, 13);

	for (uint8_t i = 1U; i < MESHTASTIC_MAX_CHANNELS; i++) {
		zassert_equal(meshtastic_channels_get(i)->role, meshtastic_Channel_Role_DISABLED,
			      "slot %u should be disabled", i);
	}
}

ZTEST(channels, test_primary_hash_is_the_name_and_key_xor)
{
	const uint8_t expected = xor_hash((const uint8_t *)MESHTASTIC_CHANNEL_LONGFAST,
					  strlen(MESHTASTIC_CHANNEL_LONGFAST)) ^
				 xor_hash(meshtastic_default_psk, sizeof(meshtastic_default_psk));

	zassert_equal(expected, LONGFAST_HASH, "LongFast hash drifted from the on-air value");
	zassert_equal(meshtastic_channels_primary_hash(), LONGFAST_HASH);
	zassert_equal(meshtastic_channels_get_hash(0U), LONGFAST_HASH);
	zassert_true(meshtastic_channels_decrypt_for_hash(0U, LONGFAST_HASH));
	zassert_false(meshtastic_channels_decrypt_for_hash(0U, LONGFAST_HASH + 1U));
	zassert_equal(mt.ch_hash, LONGFAST_HASH, "transmit context hash out of sync");
}

ZTEST(channels, test_disabled_slot_has_no_key_and_no_hash)
{
	struct meshtastic_channel_key key;

	zassert_equal(meshtastic_channels_get_key(1U, &key), -ENOENT);
	zassert_equal(meshtastic_channels_get_hash(1U), 0U);
}

ZTEST(channels, test_short_psk_index_selects_a_default_key_variant)
{
	const uint8_t psk_index[] = {3U};
	struct meshtastic_channel_key key;
	uint8_t expected[sizeof(meshtastic_default_psk)];

	set_slot(1U, meshtastic_Channel_Role_SECONDARY, "shortpsk", psk_index, sizeof(psk_index));

	memcpy(expected, meshtastic_default_psk, sizeof(expected));
	expected[sizeof(expected) - 1U] += 2U;

	zassert_ok(meshtastic_channels_get_key(1U, &key));
	zassert_equal(key.len, sizeof(expected));
	zassert_mem_equal(key.bytes, expected, sizeof(expected));
}

ZTEST(channels, test_short_psk_index_one_is_the_default_key)
{
	const uint8_t psk_index[] = {1U};
	struct meshtastic_channel_key key;

	set_slot(1U, meshtastic_Channel_Role_SECONDARY, "defaultpsk", psk_index, sizeof(psk_index));

	zassert_ok(meshtastic_channels_get_key(1U, &key));
	zassert_equal(key.len, sizeof(meshtastic_default_psk));
	zassert_mem_equal(key.bytes, meshtastic_default_psk, sizeof(meshtastic_default_psk));
}

ZTEST(channels, test_short_psk_zero_disables_encryption)
{
	const uint8_t psk_index[] = {0U};
	struct meshtastic_channel_key key;

	set_slot(1U, meshtastic_Channel_Role_SECONDARY, "cleartext", psk_index, sizeof(psk_index));

	zassert_ok(meshtastic_channels_get_key(1U, &key));
	zassert_equal(key.len, 0U);
	zassert_equal(meshtastic_channels_get_hash(1U),
		      xor_hash((const uint8_t *)"cleartext", strlen("cleartext")));
}

ZTEST(channels, test_secondary_channel_without_psk_inherits_the_primary_key)
{
	struct meshtastic_channel_key key;

	set_slot(1U, meshtastic_Channel_Role_SECONDARY, "inherit", NULL, 0U);

	zassert_ok(meshtastic_channels_get_key(1U, &key));
	zassert_equal(key.len, sizeof(meshtastic_default_psk));
	zassert_mem_equal(key.bytes, meshtastic_default_psk, sizeof(meshtastic_default_psk));
}

ZTEST(channels, test_primary_channel_without_psk_is_cleartext)
{
	struct meshtastic_channel_key key;

	set_slot(0U, meshtastic_Channel_Role_PRIMARY, "open", NULL, 0U);

	zassert_ok(meshtastic_channels_primary_key(&key));
	zassert_equal(key.len, 0U);
	zassert_equal(meshtastic_channels_primary_hash(),
		      xor_hash((const uint8_t *)"open", strlen("open")));
}

ZTEST(channels, test_an_undersized_key_is_padded_to_aes_128)
{
	const uint8_t short_key[] = {1U, 2U, 3U, 4U};
	struct meshtastic_channel_key key;

	set_slot(1U, meshtastic_Channel_Role_SECONDARY, "shortkey", short_key, sizeof(short_key));

	zassert_ok(meshtastic_channels_get_key(1U, &key));
	zassert_equal(key.len, 16U);
	zassert_mem_equal(key.bytes, short_key, sizeof(short_key));
	for (size_t i = sizeof(short_key); i < key.len; i++) {
		zassert_equal(key.bytes[i], 0U, "byte %zu should be zero padding", i);
	}

	zassert_equal(meshtastic_channels_get_hash(1U),
		      xor_hash((const uint8_t *)"shortkey", strlen("shortkey")) ^
			      xor_hash(short_key, sizeof(short_key)),
		      "padding bytes must not change the hash");
}

ZTEST(channels, test_a_key_between_the_two_aes_sizes_is_padded_to_aes_256)
{
	uint8_t key_20[20];
	struct meshtastic_channel_key key;

	memset(key_20, 0xABU, sizeof(key_20));
	set_slot(1U, meshtastic_Channel_Role_SECONDARY, "longkey", key_20, sizeof(key_20));

	zassert_ok(meshtastic_channels_get_key(1U, &key));
	zassert_equal(key.len, 32U);
	zassert_mem_equal(key.bytes, key_20, sizeof(key_20));
	zassert_equal(key.bytes[sizeof(key_20)], 0U);
}

ZTEST(channels, test_legacy_default_channel_name_is_normalised)
{
	set_slot(1U, meshtastic_Channel_Role_SECONDARY, "Default", meshtastic_default_psk,
		 sizeof(meshtastic_default_psk));

	zassert_str_equal(meshtastic_channels_get(1U)->settings.name, "");
	zassert_str_equal(meshtastic_channels_get_name(1U), MESHTASTIC_CHANNEL_LONGFAST);
}

ZTEST(channels, test_promoting_a_slot_demotes_the_previous_primary)
{
	set_slot(2U, meshtastic_Channel_Role_PRIMARY, "newprimary", meshtastic_default_psk,
		 sizeof(meshtastic_default_psk));

	zassert_equal(meshtastic_channels_primary_index(), 2U);
	zassert_equal(meshtastic_channels_get(0U)->role, meshtastic_Channel_Role_SECONDARY);
	zassert_equal(meshtastic_channels_get(2U)->index, 2);
	zassert_str_equal(meshtastic_channels_primary_name(), "newprimary");
	zassert_equal(mt.ch_hash, meshtastic_channels_get_hash(2U));
}

ZTEST(channels, test_out_of_range_slots_are_rejected)
{
	meshtastic_Channel ch = meshtastic_Channel_init_zero;
	struct meshtastic_channel_key key;

	zassert_is_null(meshtastic_channels_get(MESHTASTIC_MAX_CHANNELS));
	zassert_str_equal(meshtastic_channels_get_name(MESHTASTIC_MAX_CHANNELS), "");
	zassert_equal(meshtastic_channels_get_hash(MESHTASTIC_MAX_CHANNELS), 0U);
	zassert_false(meshtastic_channels_decrypt_for_hash(MESHTASTIC_MAX_CHANNELS, 0U));
	zassert_equal(meshtastic_channels_get_key(MESHTASTIC_MAX_CHANNELS, &key), -EINVAL);
	zassert_equal(meshtastic_channels_get_key(0U, NULL), -EINVAL);
	zassert_equal(meshtastic_channels_set_slot(MESHTASTIC_MAX_CHANNELS, &ch), -EINVAL);
	zassert_equal(meshtastic_channels_set_slot(0U, NULL), -EINVAL);
	zassert_false(meshtastic_channels_uplink_enabled(MESHTASTIC_MAX_CHANNELS));
	zassert_false(meshtastic_channels_downlink_enabled(MESHTASTIC_MAX_CHANNELS));
}

ZTEST(channels, test_send_index_prefers_an_enabled_requested_slot)
{
	set_slot(3U, meshtastic_Channel_Role_SECONDARY, "secondary", meshtastic_default_psk,
		 sizeof(meshtastic_default_psk));

	zassert_equal(meshtastic_channels_resolve_send_index(PEER_NODE_ID, 3U, 0U), 3U);
	/* Slot 4 is still disabled, so the request cannot be honoured. */
	zassert_equal(meshtastic_channels_resolve_send_index(PEER_NODE_ID, 4U, 0U), 0U);
}

ZTEST(channels, test_send_index_falls_back_to_the_slot_matching_the_wire_hash)
{
	set_slot(5U, meshtastic_Channel_Role_SECONDARY, "byhash", meshtastic_default_psk,
		 sizeof(meshtastic_default_psk));

	zassert_equal(meshtastic_channels_resolve_send_index(PEER_NODE_ID,
							     MESHTASTIC_CHANNEL_INDEX_INVALID,
							     meshtastic_channels_get_hash(5U)),
		      5U);
}

ZTEST(channels, test_send_index_falls_back_to_the_primary_channel)
{
	zassert_equal(meshtastic_channels_resolve_send_index(MESHTASTIC_NODE_BROADCAST,
							     MESHTASTIC_CHANNEL_INDEX_INVALID, 0U),
		      meshtastic_channels_primary_index());
	/* An unknown hash matches no slot and also lands on the primary channel. */
	zassert_equal(meshtastic_channels_resolve_send_index(
			      MESHTASTIC_NODE_BROADCAST, MESHTASTIC_CHANNEL_INDEX_INVALID, 0xA5U),
		      meshtastic_channels_primary_index());
}

ZTEST(channels, test_uplink_and_downlink_follow_channel_settings)
{
	meshtastic_Channel ch = *meshtastic_channels_get(0U);

	zassert_true(meshtastic_channels_uplink_enabled(0U));
	zassert_true(meshtastic_channels_downlink_enabled(0U));
	zassert_false(meshtastic_channels_uplink_enabled(1U), "disabled slot cannot uplink");
	zassert_false(meshtastic_channels_downlink_enabled(1U), "disabled slot cannot downlink");

	ch.settings.uplink_enabled = false;
	zassert_ok(meshtastic_channels_set_slot(0U, &ch));
	zassert_false(meshtastic_channels_uplink_enabled(0U));
	zassert_true(meshtastic_channels_downlink_enabled(0U));
}

ZTEST(channels, test_mqtt_channel_name_matches_enabled_slots_only)
{
	zassert_true(meshtastic_channels_matches_mqtt_name(MESHTASTIC_CHANNEL_LONGFAST));
	zassert_false(meshtastic_channels_matches_mqtt_name("Private"));
	zassert_false(meshtastic_channels_matches_mqtt_name(NULL));

	set_slot(1U, meshtastic_Channel_Role_SECONDARY, "Private", meshtastic_default_psk,
		 sizeof(meshtastic_default_psk));
	zassert_true(meshtastic_channels_matches_mqtt_name("Private"));
}

ZTEST(channels, test_client_mute_and_rebroadcast_none_stop_relaying)
{
	zassert_true(meshtastic_is_rebroadcaster());

	meshtastic_set_device_role(meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE);
	zassert_false(meshtastic_is_rebroadcaster());

	meshtastic_set_device_role(meshtastic_Config_DeviceConfig_Role_ROUTER);
	zassert_equal(meshtastic_device_role(), meshtastic_Config_DeviceConfig_Role_ROUTER);
	zassert_true(meshtastic_is_rebroadcaster());

	meshtastic_set_rebroadcast_mode(meshtastic_Config_DeviceConfig_RebroadcastMode_NONE);
	zassert_equal(meshtastic_rebroadcast_mode(),
		      meshtastic_Config_DeviceConfig_RebroadcastMode_NONE);
	zassert_false(meshtastic_is_rebroadcaster());
}

ZTEST(channels, test_known_only_mode_requires_a_known_sender)
{
	zassert_true(meshtastic_decode_known_only(PEER_NODE_ID), "ALL mode accepts any sender");

	meshtastic_set_rebroadcast_mode(meshtastic_Config_DeviceConfig_RebroadcastMode_KNOWN_ONLY);
	zassert_false(meshtastic_decode_known_only(PEER_NODE_ID));
	zassert_true(meshtastic_decode_known_only(TEST_NODE_ID),
		     "the local node is always in the NodeDB");
}
