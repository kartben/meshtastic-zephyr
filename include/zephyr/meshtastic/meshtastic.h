/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

/**
 * @file
 * @brief Meshtastic mesh radio stack public API.
 *
 * Implements the on-air packet format used by the Meshtastic project
 * (https://meshtastic.org) on top of Zephyr's raw LoRa driver API.
 * The implementation is wire-compatible with official Meshtastic firmware:
 * same 16-byte packet header, same AES-CTR encryption scheme, same
 * nanopb-encoded @c Data protobuf payload, and same flood-routing algorithm.
 *
 * Feature modules such as Bluetooth, shell, GNSS position, and device
 * metrics all use the same packet-level API.
 */

#ifndef ZEPHYR_INCLUDE_MESHTASTIC_MESHTASTIC_H_
#define ZEPHYR_INCLUDE_MESHTASTIC_MESHTASTIC_H_

/**
 * @defgroup meshtastic Meshtastic
 * @ingroup connectivity
 * @{
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Node ID that addresses all nodes simultaneously (broadcast). */
#define MESHTASTIC_NODE_BROADCAST 0xFFFFFFFFU

/**
 * @brief Maximum UTF-8 text payload for a single text message.
 *
 * Limited by the maximum LoRa packet size (255 bytes) minus the 16-byte
 * wire header and the 5-byte protobuf field overhead for the @c Data message.
 */
#define MESHTASTIC_MAX_TEXT_LEN 233U

/** Maximum raw Data.payload bytes supported in a single LoRa packet. */
#define MESHTASTIC_MAX_PAYLOAD_LEN 233U

/** Channel name for the default Meshtastic "LongFast" channel. */
#define MESHTASTIC_CHANNEL_LONGFAST "LongFast"

/** @ref meshtastic_packet.channel_index when the RX channel is unknown. */
#define MESHTASTIC_CHANNEL_INDEX_INVALID 0xFFU

/** LoRa carrier frequency for LongFast in the US region (Hz). */
#define MESHTASTIC_FREQ_US 906875000U

/** LoRa carrier frequency for LongFast in the EU region (Hz). */
#define MESHTASTIC_FREQ_EU 869525000U

/**
 * @brief Well-known 16-byte AES-128 PSK for the default LongFast channel.
 *
 * This is the publicly documented default key shared by all Meshtastic
 * devices on the default channel.  It corresponds to the base64 string
 * @c "1PG7OiApB1nwvP+rz05pAQ==" published in the official Meshtastic
 * protobuf repository.
 */
extern const uint8_t meshtastic_default_psk[16];

/**
 * @brief Meshtastic application port numbers.
 *
 * Wire-compatible subset of the official @c PortNum protobuf enum.
 * Only the values required for a messaging-focused implementation are
 * listed; additional values may be passed to meshtastic_send_data() as
 * plain @c uint8_t constants.
 */
enum meshtastic_portnum {
	/** Unknown / unset. */
	MESHTASTIC_PORT_UNKNOWN = 0,
	/** Plain UTF-8 text message. */
	MESHTASTIC_PORT_TEXT_MESSAGE = 1,
	/** Remote GPIO / hardware control. */
	MESHTASTIC_PORT_REMOTE_HARDWARE = 2,
	/** GPS position report. */
	MESHTASTIC_PORT_POSITION = 3,
	/** Node information (name, hardware model, …). */
	MESHTASTIC_PORT_NODEINFO = 4,
	/** Internal mesh routing. */
	MESHTASTIC_PORT_ROUTING = 5,
	/** Node status string. */
	MESHTASTIC_PORT_NODE_STATUS = 36,
	/** Sensor telemetry. */
	MESHTASTIC_PORT_TELEMETRY = 67,
	/** Private / application-defined. */
	MESHTASTIC_PORT_PRIVATE = 256,
};

/** Meshtastic stack event type. */
enum meshtastic_event_type {
	/** A packet addressed to this node or broadcast was decoded. */
	MESHTASTIC_EVENT_PACKET_RECEIVED,
	/**
	 * A text message (port 1) was received and passed module deduplication.
	 *
	 * @p event.packet is set; payload is UTF-8 and not NUL-terminated.
	 * Requires @kconfig{CONFIG_MESHTASTIC_MESSAGE}.
	 */
	MESHTASTIC_EVENT_TEXT_MESSAGE,
	/** A local packet was transmitted successfully. */
	MESHTASTIC_EVENT_TX_DONE,
	/** A local packet transmit failed. */
	MESHTASTIC_EVENT_TX_FAILED,
	/** A Bluetooth central connected to the Meshtastic service. */
	MESHTASTIC_EVENT_BLE_CONNECTED,
	/** The Bluetooth central disconnected. */
	MESHTASTIC_EVENT_BLE_DISCONNECTED,
	/** A GNSS fix was accepted for position reporting. */
	MESHTASTIC_EVENT_GNSS_FIX,
	/** Device metrics collection failed partially or completely. */
	MESHTASTIC_EVENT_METRICS_ERROR,
};

/**
 * @brief Decoded Meshtastic packet.
 *
 * Payload pointers passed to callbacks are valid only for the duration of the
 * callback. Callers of meshtastic_send_packet() must keep @p payload valid
 * until the function returns.
 */
struct meshtastic_packet {
	/** Source node ID. Filled automatically for local sends when zero. */
	uint32_t from;
	/** Destination node ID, or @ref MESHTASTIC_NODE_BROADCAST. */
	uint32_t to;
	/** Packet ID. Filled automatically for local sends when zero. */
	uint32_t id;
	/** Application port number. */
	uint32_t portnum;
	/** Raw application payload. */
	const uint8_t *payload;
	/** Number of bytes in @p payload. */
	size_t payload_len;
	/** Decoded Data.dest field, when present in the encrypted payload. */
	uint32_t data_dest;
	/** Decoded Data.source field, when present in the encrypted payload. */
	uint32_t data_source;
	/** Decoded Data.request_id field, used by response/routing packets. */
	uint32_t request_id;
	/** Decoded Data.reply_id field, used by response packets. */
	uint32_t reply_id;
	/** Remaining hop limit. */
	uint8_t hop_limit;
	/** Initial hop limit. */
	uint8_t hop_start;
	/** On-air channel hash from the wire header. */
	uint8_t channel;
	/**
	 * Decoded channel table index (0..7), or
	 * @ref MESHTASTIC_CHANNEL_INDEX_INVALID when unknown.
	 */
	uint8_t channel_index;
	/** Last byte of requested next-hop node. */
	uint8_t next_hop;
	/** Last byte of relay node. */
	uint8_t relay_node;
	/** Packet requests an acknowledgement. */
	bool want_ack;
	/** Packet was marked as having passed via MQTT. */
	bool via_mqtt;
	/** Application payload asks peers to respond in kind. */
	bool want_response;
	/** Receive RSSI in dBm, when known. */
	int16_t rssi;
	/** Receive SNR value reported by the LoRa driver, when known. */
	int8_t snr;
};

/** Runtime Meshtastic stack counters and state. */
struct meshtastic_status {
	bool initialized;
	bool ble_connected;
	uint32_t node_id;
	uint32_t tx_packets;
	uint32_t tx_failures;
	uint32_t rx_packets;
	uint32_t relayed_packets;
	uint32_t duplicate_packets;
	uint32_t decode_failures;
	uint32_t rx_dropped;
	uint32_t rx_rearm_failures;
	uint32_t last_rx_from;
	int16_t last_rssi;
	int8_t last_snr;
};

/** Event payload passed to meshtastic_event_cb_t. */
struct meshtastic_event {
	enum meshtastic_event_type type;
	int err;
	const struct meshtastic_packet *packet;
};

/**
 * @brief Configuration passed to meshtastic_init().
 *
 * All pointer fields must remain valid for the lifetime of the stack.
 */
struct meshtastic_config {
	/**
	 * Handle of the LoRa transceiver device.  Must be ready before
	 * meshtastic_init() is called.
	 */
	const struct device *lora_dev;

	/**
	 * Local node ID.  When @kconfig{CONFIG_MESHTASTIC_NODE_ID_CUSTOM} is
	 * enabled, set this to a unique value or zero to use
	 * @kconfig{CONFIG_MESHTASTIC_NODE_ID_DEFAULT}.  Otherwise pass zero and
	 * the stack derives the ID from hardware per
	 * @kconfig{CONFIG_MESHTASTIC_NODE_ID_SOURCE}.  Two nodes with the same
	 * ID on the same mesh will cause routing anomalies.
	 */
	uint32_t node_id;

	/**
	 * Pointer to the AES channel key.  16 bytes for AES-128, 32 bytes
	 * for AES-256.  Use @ref meshtastic_default_psk for the standard
	 * LongFast channel.
	 */
	const uint8_t *psk;

	/** Number of bytes in @p psk (must be 16 or 32). */
	size_t psk_len;

	/**
	 * Channel name used with @p psk to compute the one-byte channel hash
	 * (xor of name and key, per official Meshtastic firmware) embedded in
	 * every packet header.  Use @ref MESHTASTIC_CHANNEL_LONGFAST for the
	 * default channel.
	 */
	const char *channel_name;

	/**
	 * LoRa carrier frequency in Hz.  Use @ref MESHTASTIC_FREQ_US or
	 * @ref MESHTASTIC_FREQ_EU for the default LongFast channel.
	 */
	uint32_t frequency;

	/**
	 * Hop limit written into outgoing packet headers (1–7).
	 * 0 → use @kconfig{CONFIG_MESHTASTIC_DEFAULT_HOP_LIMIT}.
	 */
	uint8_t hop_limit;

	/**
	 * TX power in dBm.
	 * 0 → use @kconfig{CONFIG_MESHTASTIC_TX_POWER}.
	 */
	int8_t tx_power;

	/**
	 * Optional human-readable long node name used by local services such as
	 * the Bluetooth PhoneAPI configuration handshake.
	 */
	const char *long_name;

	/**
	 * Optional short node name. If NULL, a short name is derived from the
	 * node ID where needed.
	 */
	const char *short_name;
};

/**
 * @brief Callback invoked for each received Meshtastic packet.
 *
 * Called from the Meshtastic processing thread after a packet has been
 * successfully decrypted and decoded.  Packets addressed to this node
 * or to the broadcast address trigger the callback; pure relay packets
 * (addressed to another node) do not.
 *
 * For text messages (port 1), prefer @ref MESHTASTIC_EVENT_TEXT_MESSAGE via
 * meshtastic_set_event_cb() when @kconfig{CONFIG_MESHTASTIC_MESSAGE} is
 * enabled.
 *
 * @param from        Source node ID.
 * @param to          Destination node ID (@ref MESHTASTIC_NODE_BROADCAST
 *                    for broadcast packets).
 * @param portnum     Application port number (see @ref meshtastic_portnum).
 * @param payload     Decrypted, decoded payload bytes.
 * @param payload_len Number of bytes in @p payload.
 * @param rssi        RSSI in dBm.
 * @param snr         SNR in dB (0.25 dB units as returned by the driver).
 */
typedef void (*meshtastic_recv_cb_t)(uint32_t from, uint32_t to, uint32_t portnum,
				     const uint8_t *payload, size_t payload_len, int16_t rssi,
				     int8_t snr);

/**
 * @brief Callback invoked for stack events.
 *
 * Called synchronously from the context where the event occurs. Packet
 * pointers are borrowed and must not be retained after the callback returns.
 */
typedef void (*meshtastic_event_cb_t)(const struct meshtastic_event *event, void *user_data);

/**
 * @brief Initialise the Meshtastic stack.
 *
 * Configures the LoRa modem for the LongFast channel parameters (SF 11,
 * BW 250 kHz, CR 4/5, 16-symbol preamble) at the frequency given in
 * @p cfg, initialises PSA crypto, starts continuous receive and the
 * background packet-processing thread.  Must be called once before any
 * other @c meshtastic_*() function.
 *
 * @param cfg  Non-NULL pointer to a filled-in configuration structure that
 *             must remain valid for the lifetime of the stack.
 *
 * @retval 0        Success.
 * @retval -EINVAL  @p cfg is NULL or contains invalid parameters.
 * @retval -ENODEV  The LoRa device is not ready.
 * @retval -EIO     LoRa modem configuration or PSA crypto initialisation
 *                  failed.
 */
int meshtastic_init(const struct meshtastic_config *cfg);

/**
 * @brief Send a UTF-8 text message.
 *
 * Encodes @p text as a @c TEXT_MESSAGE_APP (port 1) Meshtastic packet,
 * encrypts it, and transmits it.  Blocks until the radio has finished
 * transmitting.
 *
 * @param dest  Destination node ID, or @ref MESHTASTIC_NODE_BROADCAST.
 * @param text  NUL-terminated UTF-8 string.  Maximum length is
 *              @ref MESHTASTIC_MAX_TEXT_LEN bytes (excluding the NUL).
 *
 * @retval 0        Success.
 * @retval -EINVAL  @p text is NULL or longer than @ref MESHTASTIC_MAX_TEXT_LEN.
 * @retval -ENOMEM  Protobuf encoding failed.
 * @retval -EIO     Crypto or radio transmission failed.
 */
int meshtastic_send_text(uint32_t dest, const char *text);

/**
 * @brief Send a raw Meshtastic data payload.
 *
 * Places @p payload in the @c Data.payload protobuf field, encrypts the
 * message, and transmits it.
 *
 * @param dest        Destination node ID, or @ref MESHTASTIC_NODE_BROADCAST.
 * @param portnum     Application port number (see @ref meshtastic_portnum).
 * @param payload     Raw payload bytes.
 * @param payload_len Number of bytes in @p payload (max
 *                    @ref MESHTASTIC_MAX_PAYLOAD_LEN).
 * @param wait        @c K_FOREVER to block until transmission completes;
 *                    @c K_NO_WAIT to queue the frame only (-ENOMSG if the TX
 *                    queue is full).
 *
 * @retval 0        Success (queued, or transmitted when @p wait blocks).
 * @retval -EINVAL  Invalid arguments or @p payload_len exceeds the limit.
 * @retval -ENOMEM  Protobuf encoding failed.
 * @retval -EIO     Crypto or radio transmission failed.
 * @retval -ENOMSG  TX queue full (@p wait is @c K_NO_WAIT).
 */
int meshtastic_send_data(uint32_t dest, uint32_t portnum, const uint8_t *payload,
			 size_t payload_len, k_timeout_t wait);

/**
 * @brief Send a decoded Meshtastic packet.
 *
 * The stack fills @p packet->from and @p packet->id when they are zero,
 * encodes @p packet->payload as a Data payload, encrypts it, and transmits
 * the corresponding LoRa wire packet.
 *
 * @param packet Packet description.
 * @param wait   @c K_FOREVER to block until transmission completes;
 *               @c K_NO_WAIT to queue the frame only (-ENOMSG if the TX
 *               queue is full).
 *
 * @retval 0        Success (queued, or transmitted when @p wait blocks).
 * @retval -EINVAL  Invalid arguments or payload too large.
 * @retval -ENOMEM  Protobuf encoding failed.
 * @retval -EIO     Crypto or radio transmission failed.
 * @retval -ENOMSG  TX queue full (@p wait is @c K_NO_WAIT).
 */
int meshtastic_send_packet(const struct meshtastic_packet *packet, k_timeout_t wait);

/**
 * @brief Register (or deregister) a packet-receive callback.
 *
 * Only packets addressed to this node or broadcast are delivered.
 * Pass NULL to deregister the current callback.
 *
 * @param cb  Callback function pointer, or NULL.
 */
void meshtastic_set_recv_cb(meshtastic_recv_cb_t cb);

/**
 * @brief Register (or deregister) a stack event callback.
 *
 * Pass NULL to deregister the current callback.
 */
void meshtastic_set_event_cb(meshtastic_event_cb_t cb, void *user_data);

/**
 * @brief Copy current stack status counters.
 *
 * @param status Destination for status data.
 *
 * @retval 0        Success.
 * @retval -EINVAL  @p status is NULL.
 */
int meshtastic_get_status(struct meshtastic_status *status);

/**
 * @brief Return the local node ID.
 *
 * @return The @c node_id value supplied in @ref meshtastic_config.
 */
uint32_t meshtastic_get_node_id(void);

/**
 * @brief Return the configured name for a channel table slot.
 *
 * @param index Channel index (0..7).
 *
 * @return Channel name string, or an empty string when @p index is invalid.
 */
const char *meshtastic_get_channel_name(uint8_t index);

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* ZEPHYR_INCLUDE_MESHTASTIC_MESHTASTIC_H_ */
