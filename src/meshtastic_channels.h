/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

/**
 * @file meshtastic_channels.h
 * @brief Internal Meshtastic channel table and device policy helpers.
 *
 * Maintains the fixed-size channel slot array (up to eight entries, per the Meshtastic @c Channel
 * protobuf schema), derives per-channel wire hashes used for AES-CTR packet crypto, and exposes
 * convenience accessors for the primary channel. Also holds runtime copies of @c DeviceConfig role
 * and rebroadcast mode used when deciding whether to relay foreign packets.
 *
 * Channel slots are initialized from Kconfig defaults or
 * @ref meshtastic_config at boot; callers may update individual slots with
 * @ref meshtastic_channels_set_slot.
 */

#ifndef ZEPHYR_SUBSYS_MESHTASTIC_CHANNELS_H_
#define ZEPHYR_SUBSYS_MESHTASTIC_CHANNELS_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "meshtastic/channel.pb.h"
#include "meshtastic/config.pb.h"

#include <zephyr/meshtastic/meshtastic.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Maximum number of channel slots in the local table.
 *
 * Fixed by the Meshtastic protobuf schema; indices are always in
 * @c [0, MESHTASTIC_MAX_CHANNELS).
 */
#define MESHTASTIC_MAX_CHANNELS 8U

/**
 * @brief Normalized pre-shared key material for a channel slot.
 *
 * Produced by @ref meshtastic_channels_get_key and
 * @ref meshtastic_channels_primary_key after expanding single-byte “short PSK” indices and
 * inheriting the primary key for secondary channels with an empty PSK. A length of zero means
 * cleartext (no AES) for that slot.
 */
struct meshtastic_channel_key {
	/** Expanded AES-128 (16 bytes) or AES-256 (32 bytes) key bytes. */
	uint8_t bytes[32];
	/** Effective key length after short-PSK expansion; @c 0 for cleartext. */
	size_t len;
};

/**
 * @brief Initialize all channel slots to stack defaults.
 *
 * Slot 0 becomes the primary LongFast channel with
 * @ref meshtastic_default_psk; remaining slots are disabled placeholders with hashes computed via
 * @ref meshtastic_channels_get_hash rules.
 *
 * @return 0 on success (always succeeds today).
 */
int meshtastic_channels_init_defaults(void);

/**
 * @brief Initialize channels from application Kconfig / @ref meshtastic_config.
 *
 * Calls @ref meshtastic_channels_init_defaults first, then overwrites the primary channel name and
 * PSK from @p cfg when non-NULL. Also refreshes the global transmit context (@c mt) primary hash,
 * PSK, and name.
 *
 * @param cfg Build-time configuration, or @c NULL to keep defaults only.
 * @return 0 on success, or a negative errno from the defaults path.
 */
int meshtastic_channels_init_from_config(const struct meshtastic_config *cfg);

/**
 * @brief Return the fixed channel table capacity.
 *
 * @return Always @ref MESHTASTIC_MAX_CHANNELS.
 */
uint8_t meshtastic_channels_count(void);

/**
 * @brief Index of the slot currently designated PRIMARY.
 *
 * @return Channel index in @c [0, MESHTASTIC_MAX_CHANNELS).
 */
uint8_t meshtastic_channels_primary_index(void);

/**
 * @brief Read-only view of a channel slot.
 *
 * @param index Slot index.
 * @return Pointer to the internal @c meshtastic_Channel, or @c NULL if
 *         @p index is out of range.
 */
const meshtastic_Channel *meshtastic_channels_get(uint8_t index);

/**
 * @brief Replace a channel slot and refresh derived state.
 *
 * Copies @p channel into the slot, assigns @c index, demotes any other PRIMARY if the new slot is
 * PRIMARY, recomputes the wire hash, and updates @c mt when the primary slot changes.
 *
 * @param index Slot to update.
 * @param channel Protobuf channel descriptor (must not be @c NULL).
 * @return 0 on success, or @c -EINVAL for invalid arguments.
 */
int meshtastic_channels_set_slot(uint8_t index, const meshtastic_Channel *channel);

/**
 * @brief Obtain the normalized AES key for a channel slot.
 *
 * Expands 1-byte short PSK indices, falls back to the primary key for SECONDARY slots with empty
 * PSK, and leaves @p key->len at 0 for cleartext PRIMARY channels.
 *
 * @param index Slot index.
 * @param key Output key buffer (must not be @c NULL).
 * @return 0 on success, @c -EINVAL for bad arguments or invalid key size,
 *         @c -ENOENT if the slot is disabled or missing settings.
 */
int meshtastic_channels_get_key(uint8_t index, struct meshtastic_channel_key *key);

/**
 * @brief Return the cached wire hash for a channel slot.
 *
 * The hash is XOR of the channel name bytes and key bytes; it appears in the LoRa packet header and
 * selects the decryption key on receive.
 *
 * @param index Slot index.
 * @return Cached hash, or @c 0 if @p index is invalid.
 */
uint8_t meshtastic_channels_get_hash(uint8_t index);

/**
 * @brief Test whether a slot's cached hash matches an on-air header hash.
 *
 * Used after RX to confirm which local slot can decrypt a packet.
 *
 * @param index Slot to test.
 * @param wire_hash Hash byte from the received packet header.
 * @return @c true if the slot hash equals @p wire_hash and @p index is valid.
 */
bool meshtastic_channels_decrypt_for_hash(uint8_t index, uint8_t wire_hash);

/**
 * @brief Human-readable channel name for a slot.
 *
 * Empty or unset names map to @ref MESHTASTIC_CHANNEL_LONGFAST for slot 0 semantics. The returned
 * pointer is stable until the slot is modified.
 *
 * @param index Slot index.
 * @return Channel name string, or @c "" if @p index is out of range.
 */
const char *meshtastic_channels_get_name(uint8_t index);

/**
 * @brief Pick the channel index to use when sending a packet.
 *
 * Resolution order:
 * 1. Use @p channel_index if that slot is enabled.
 * 2. Else, if @p wire_hash is non-zero, find the first slot matching the hash.
 * 3. Else, if NodeDB is enabled and @p dest is a known unicast peer, use the peer's stored channel
 *    when valid.
 * 4. Else fall back to the primary channel index.
 *
 * @param dest Destination node ID (used for NodeDB lookup).
 * @param channel_index Requested index from the caller (may be invalid).
 * @param wire_hash Header hash from an incoming packet when replying, or 0.
 * @return Resolved slot index for TX crypto and @c Data.channel field.
 */
uint8_t meshtastic_channels_resolve_send_index(uint32_t dest, uint8_t channel_index,
					       uint8_t wire_hash);

/**
 * @brief Wire hash of the primary channel slot.
 */
uint8_t meshtastic_channels_primary_hash(void);

/**
 * @brief Display name of the primary channel slot.
 *
 * @return Same rules as @ref meshtastic_channels_get_name for @c primary_index.
 */
const char *meshtastic_channels_primary_name(void);

/**
 * @brief Normalized AES key for the primary channel slot.
 *
 * @param key Output key buffer (must not be @c NULL).
 * @return 0 on success, or negative errno from @ref meshtastic_channels_get_key.
 */
int meshtastic_channels_primary_key(struct meshtastic_channel_key *key);

/**
 * @brief Whether MQTT/uplink forwarding is enabled for a slot.
 *
 * @param index Slot index.
 * @return @c true when the slot is active and @c settings.uplink_enabled is set.
 */
bool meshtastic_channels_uplink_enabled(uint8_t index);

/**
 * @brief Whether MQTT/downlink injection is enabled for a slot.
 *
 * @param index Slot index.
 * @return @c true when the slot is active and @c settings.downlink_enabled is set.
 */
bool meshtastic_channels_downlink_enabled(uint8_t index);

/**
 * @brief Test whether a MQTT topic channel id matches any local slot name.
 *
 * Compares @p channel_id against @ref meshtastic_channels_get_name for each non-disabled slot
 * (case-sensitive @c strcmp).
 *
 * @param channel_id Channel name from an MQTT envelope.
 * @return @c true if any enabled local channel uses the same name.
 */
bool meshtastic_channels_matches_mqtt_name(const char *channel_id);

/**
 * @brief Current device role (CLIENT, ROUTER, …).
 */
meshtastic_Config_DeviceConfig_Role meshtastic_device_role(void);

/**
 * @brief Current rebroadcast mode filter.
 */
meshtastic_Config_DeviceConfig_RebroadcastMode meshtastic_rebroadcast_mode(void);

/**
 * @brief Override the runtime device role.
 *
 * @param role New @c DeviceConfig.role value.
 */
void meshtastic_set_device_role(meshtastic_Config_DeviceConfig_Role role);

/**
 * @brief Override the runtime rebroadcast mode.
 *
 * @param mode New @c DeviceConfig.rebroadcast_mode value.
 */
void meshtastic_set_rebroadcast_mode(meshtastic_Config_DeviceConfig_RebroadcastMode mode);

/**
 * @brief Whether this node should relay packets originated by others.
 *
 * Rebroadcasting is suppressed for @c CLIENT_MUTE role or when rebroadcast mode
 * is @c NONE.
 */
bool meshtastic_is_rebroadcaster(void);

/**
 * @brief Whether a packet from @p from may be decoded/processed under policy.
 *
 * When rebroadcast mode is not @c KNOWN_ONLY, always returns @c true. In
 * @c KNOWN_ONLY mode, returns @c true only if NodeDB contains @p from (when NodeDB is compiled in);
 * otherwise returns @c false.
 *
 * @param from Source node ID from the packet header.
 * @return @c true if the sender is allowed under the current rebroadcast policy.
 */
bool meshtastic_decode_known_only(uint32_t from);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_SUBSYS_MESHTASTIC_CHANNELS_H_ */
