/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

#ifndef ZEPHYR_SUBSYS_MESHTASTIC_MQTT_H_
#define ZEPHYR_SUBSYS_MESHTASTIC_MQTT_H_

#include "meshtastic_core.h"

#ifdef __cplusplus
extern "C" {
#endif

int meshtastic_mqtt_init(void);
void meshtastic_mqtt_on_tx(const struct meshtastic_packet *packet, const uint8_t *wire,
			   size_t wire_len);
void meshtastic_mqtt_on_rx(const struct meshtastic_packet *packet, const uint8_t *wire,
			   size_t wire_len);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_SUBSYS_MESHTASTIC_MQTT_H_ */
