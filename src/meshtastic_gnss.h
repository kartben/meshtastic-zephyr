/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

#ifndef ZEPHYR_SUBSYS_MESHTASTIC_GNSS_H_
#define ZEPHYR_SUBSYS_MESHTASTIC_GNSS_H_

#include "meshtastic_core.h"

#ifdef __cplusplus
extern "C" {
#endif

int meshtastic_gnss_init(void);
int meshtastic_gnss_get_last_position(meshtastic_Position *position);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_SUBSYS_MESHTASTIC_GNSS_H_ */
