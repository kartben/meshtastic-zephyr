/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

#include <errno.h>
#include <string.h>

#include "meshtastic_channels.h"
#include "meshtastic_core.h"

#include <zephyr/meshtastic/nodedb.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(meshtastic, CONFIG_MESHTASTIC_LOG_LEVEL);

static meshtastic_Channel channel_slots[MESHTASTIC_MAX_CHANNELS];
static uint8_t channel_hashes[MESHTASTIC_MAX_CHANNELS];
static uint8_t primary_index;
static meshtastic_Config_DeviceConfig_Role device_role = meshtastic_Config_DeviceConfig_Role_CLIENT;
static meshtastic_Config_DeviceConfig_RebroadcastMode rebroadcast_mode =
	meshtastic_Config_DeviceConfig_RebroadcastMode_ALL;

static uint8_t xor_hash_bytes(const uint8_t *data, size_t len)
{
	uint8_t code = 0U;

	for (size_t i = 0; i < len; i++) {
		code ^= data[i];
	}

	return code;
}

static int channel_key_invalid(const struct meshtastic_channel_key *key)
{
	return (key == NULL || key->len == 0U);
}

static void expand_short_psk(uint8_t psk_index, struct meshtastic_channel_key *key)
{
	memcpy(key->bytes, meshtastic_default_psk, sizeof(meshtastic_default_psk));
	key->len = sizeof(meshtastic_default_psk);

	if (psk_index > 1U) {
		key->bytes[sizeof(meshtastic_default_psk) - 1U] += (psk_index - 1U);
	}
}

static int channel_get_key(uint8_t index, struct meshtastic_channel_key *key)
{
	const meshtastic_Channel *ch;
	const meshtastic_ChannelSettings *settings;

	if (key == NULL || index >= MESHTASTIC_MAX_CHANNELS) {
		return -EINVAL;
	}

	ch = &channel_slots[index];
	if (!ch->has_settings || ch->role == meshtastic_Channel_Role_DISABLED) {
		return -ENOENT;
	}

	settings = &ch->settings;
	memset(key->bytes, 0, sizeof(key->bytes));
	memcpy(key->bytes, settings->psk.bytes, settings->psk.size);
	key->len = settings->psk.size;

	if (key->len == 0U) {
		if (ch->role == meshtastic_Channel_Role_SECONDARY) {
			return channel_get_key(primary_index, key);
		}

		/* PRIMARY with empty PSK: cleartext (length stays 0). */
		return 0;
	}

	if (key->len == 1U) {
		const uint8_t psk_index = key->bytes[0];

		if (psk_index == 0U) {
			key->len = 0U;
			return 0;
		}

		expand_short_psk(psk_index, key);
	}

	if (key->len != 16U && key->len != 32U) {
		return -EINVAL;
	}

	return 0;
}

static uint8_t channel_generate_hash(uint8_t index)
{
	struct meshtastic_channel_key key;
	const char *name;
	uint8_t h = 0U;

	if (channel_get_key(index, &key) < 0) {
		return 0U;
	}

	name = meshtastic_channels_get_name(index);
	h = xor_hash_bytes((const uint8_t *)name, strlen(name));
	if (!channel_key_invalid(&key)) {
		h ^= xor_hash_bytes(key.bytes, key.len);
	}

	return h;
}

static void channel_fixup(uint8_t index)
{
	meshtastic_Channel *ch = &channel_slots[index];

	ch->index = index;
	if (!ch->has_settings) {
		ch->role = meshtastic_Channel_Role_DISABLED;
		memset(&ch->settings, 0, sizeof(ch->settings));
		ch->has_settings = true;
	}

	if (strcmp(ch->settings.name, "Default") == 0) {
		ch->settings.name[0] = '\0';
	}

	channel_hashes[index] = channel_generate_hash(index);
}

static void demote_other_primary(uint8_t index)
{
	for (uint8_t i = 0; i < MESHTASTIC_MAX_CHANNELS; i++) {
		if (i == index) {
			continue;
		}

		if (channel_slots[i].role == meshtastic_Channel_Role_PRIMARY) {
			channel_slots[i].role = meshtastic_Channel_Role_SECONDARY;
			channel_fixup(i);
		}
	}
}

int meshtastic_channels_init_defaults(void)
{
	meshtastic_Channel *ch;

	memset(channel_slots, 0, sizeof(channel_slots));
	memset(channel_hashes, 0, sizeof(channel_hashes));
	primary_index = 0U;

	ch = &channel_slots[0];
	*ch = (meshtastic_Channel)meshtastic_Channel_init_zero;
	ch->role = meshtastic_Channel_Role_PRIMARY;
	ch->has_settings = true;
	strncpy(ch->settings.name, MESHTASTIC_CHANNEL_LONGFAST, sizeof(ch->settings.name) - 1U);
	memcpy(ch->settings.psk.bytes, meshtastic_default_psk, sizeof(meshtastic_default_psk));
	ch->settings.psk.size = sizeof(meshtastic_default_psk);
	ch->settings.uplink_enabled = true;
	ch->settings.downlink_enabled = true;
	ch->settings.has_module_settings = true;
	ch->settings.module_settings.position_precision = 13;

	for (uint8_t i = 1; i < MESHTASTIC_MAX_CHANNELS; i++) {
		channel_slots[i] = (meshtastic_Channel)meshtastic_Channel_init_zero;
		channel_slots[i].role = meshtastic_Channel_Role_DISABLED;
		channel_slots[i].has_settings = true;
		channel_fixup(i);
	}

	channel_fixup(0);

	return 0;
}

int meshtastic_channels_init_from_config(const struct meshtastic_config *cfg)
{
	meshtastic_Channel *ch;
	int ret;

	ret = meshtastic_channels_init_defaults();
	if (ret < 0 || cfg == NULL) {
		return ret;
	}

	ch = &channel_slots[0];
	if (cfg->channel_name != NULL) {
		strncpy(ch->settings.name, cfg->channel_name, sizeof(ch->settings.name) - 1U);
	}
	memcpy(ch->settings.psk.bytes, cfg->psk, cfg->psk_len);
	ch->settings.psk.size = (pb_size_t)cfg->psk_len;
	channel_fixup(0);

	mt.ch_hash = meshtastic_channels_primary_hash();
	mt.psk_len = cfg->psk_len;
	memcpy(mt.psk, cfg->psk, cfg->psk_len);
	mt.channel_name = meshtastic_channels_primary_name();

	return 0;
}

uint8_t meshtastic_channels_count(void)
{
	return MESHTASTIC_MAX_CHANNELS;
}

uint8_t meshtastic_channels_primary_index(void)
{
	return primary_index;
}

const meshtastic_Channel *meshtastic_channels_get(uint8_t index)
{
	if (index >= MESHTASTIC_MAX_CHANNELS) {
		return NULL;
	}

	return &channel_slots[index];
}

int meshtastic_channels_set_slot(uint8_t index, const meshtastic_Channel *channel)
{
	if (index >= MESHTASTIC_MAX_CHANNELS || channel == NULL) {
		return -EINVAL;
	}

	channel_slots[index] = *channel;
	channel_slots[index].index = index;

	if (channel_slots[index].role == meshtastic_Channel_Role_PRIMARY) {
		primary_index = index;
		demote_other_primary(index);
	}

	channel_fixup(index);

	if (index == primary_index) {
		struct meshtastic_channel_key key;

		if (meshtastic_channels_primary_key(&key) == 0) {
			mt.psk_len = key.len;
			memcpy(mt.psk, key.bytes, key.len);
		}
		mt.ch_hash = meshtastic_channels_primary_hash();
		mt.channel_name = meshtastic_channels_primary_name();
	}

	return 0;
}

int meshtastic_channels_get_key(uint8_t index, struct meshtastic_channel_key *key)
{
	return channel_get_key(index, key);
}

uint8_t meshtastic_channels_get_hash(uint8_t index)
{
	if (index >= MESHTASTIC_MAX_CHANNELS) {
		return 0U;
	}

	return channel_hashes[index];
}

bool meshtastic_channels_decrypt_for_hash(uint8_t index, uint8_t wire_hash)
{
	if (index >= MESHTASTIC_MAX_CHANNELS) {
		return false;
	}

	return channel_hashes[index] == wire_hash;
}

const char *meshtastic_channels_get_name(uint8_t index)
{
	const meshtastic_Channel *ch;

	if (index >= MESHTASTIC_MAX_CHANNELS) {
		return "";
	}

	ch = &channel_slots[index];
	if (!ch->has_settings || ch->settings.name[0] == '\0') {
		return MESHTASTIC_CHANNEL_LONGFAST;
	}

	return ch->settings.name;
}

uint8_t meshtastic_channels_resolve_send_index(uint32_t dest, uint8_t channel_index,
					       uint8_t wire_hash)
{
	if (channel_index < MESHTASTIC_MAX_CHANNELS &&
	    channel_slots[channel_index].role != meshtastic_Channel_Role_DISABLED) {
		return channel_index;
	}

	if (wire_hash != 0U) {
		for (uint8_t i = 0; i < MESHTASTIC_MAX_CHANNELS; i++) {
			if (meshtastic_channels_decrypt_for_hash(i, wire_hash)) {
				return i;
			}
		}
	}

#if defined(CONFIG_MESHTASTIC_NODEDB)
	if (dest != 0U && dest != MESHTASTIC_NODE_BROADCAST) {
		struct meshtastic_nodedb_node node;
		uint8_t peer_ch;

		if (meshtastic_nodedb_get(dest, &node) == 0 && node.channel != 0U) {
			peer_ch = (uint8_t)node.channel;
			if (peer_ch < MESHTASTIC_MAX_CHANNELS &&
			    channel_slots[peer_ch].role != meshtastic_Channel_Role_DISABLED) {
				return peer_ch;
			}
		}
	}
#endif

	return primary_index;
}

uint8_t meshtastic_channels_primary_hash(void)
{
	return channel_hashes[primary_index];
}

const char *meshtastic_channels_primary_name(void)
{
	return meshtastic_channels_get_name(primary_index);
}

int meshtastic_channels_primary_key(struct meshtastic_channel_key *key)
{
	return channel_get_key(primary_index, key);
}

bool meshtastic_channels_uplink_enabled(uint8_t index)
{
	const meshtastic_Channel *ch = meshtastic_channels_get(index);

	if (ch == NULL || ch->role == meshtastic_Channel_Role_DISABLED || !ch->has_settings) {
		return false;
	}

	return ch->settings.uplink_enabled;
}

bool meshtastic_channels_downlink_enabled(uint8_t index)
{
	const meshtastic_Channel *ch = meshtastic_channels_get(index);

	if (ch == NULL || ch->role == meshtastic_Channel_Role_DISABLED || !ch->has_settings) {
		return false;
	}

	return ch->settings.downlink_enabled;
}

bool meshtastic_channels_matches_mqtt_name(const char *channel_id)
{
	if (channel_id == NULL) {
		return false;
	}

	for (uint8_t i = 0; i < MESHTASTIC_MAX_CHANNELS; i++) {
		if (channel_slots[i].role == meshtastic_Channel_Role_DISABLED) {
			continue;
		}

		if (strcmp(channel_id, meshtastic_channels_get_name(i)) == 0) {
			return true;
		}
	}

	return false;
}

meshtastic_Config_DeviceConfig_Role meshtastic_device_role(void)
{
	return device_role;
}

meshtastic_Config_DeviceConfig_RebroadcastMode meshtastic_rebroadcast_mode(void)
{
	return rebroadcast_mode;
}

void meshtastic_set_device_role(meshtastic_Config_DeviceConfig_Role role)
{
	device_role = role;
}

void meshtastic_set_rebroadcast_mode(meshtastic_Config_DeviceConfig_RebroadcastMode mode)
{
	rebroadcast_mode = mode;
}

bool meshtastic_is_rebroadcaster(void)
{
	return (device_role != meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE) &&
	       (rebroadcast_mode != meshtastic_Config_DeviceConfig_RebroadcastMode_NONE);
}

bool meshtastic_decode_known_only(uint32_t from)
{
	if (rebroadcast_mode != meshtastic_Config_DeviceConfig_RebroadcastMode_KNOWN_ONLY) {
		return true;
	}

#if defined(CONFIG_MESHTASTIC_NODEDB)
	{
		struct meshtastic_nodedb_node node;

		return meshtastic_nodedb_get(from, &node) == 0;
	}
#else
	ARG_UNUSED(from);

	return false;
#endif
}
