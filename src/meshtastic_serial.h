/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

#ifndef ZEPHYR_SUBSYS_MESHTASTIC_SERIAL_H_
#define ZEPHYR_SUBSYS_MESHTASTIC_SERIAL_H_

#include <stddef.h>
#include <stdint.h>

#include "meshtastic_phoneapi.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Serial PhoneAPI StreamAPI framing helpers. Exposed for the subsystem unit
 * tests. Subsystem init is invoked from meshtastic.c when
 * CONFIG_MESHTASTIC_SERIAL is enabled.
 */
size_t meshtastic_serial_encode_frame(const uint8_t *payload, size_t payload_len, uint8_t *out,
				      size_t out_len);
int meshtastic_serial_decode_byte(uint8_t byte, uint8_t *payload, size_t payload_len,
				  size_t *frame_len);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_SUBSYS_MESHTASTIC_SERIAL_H_ */
