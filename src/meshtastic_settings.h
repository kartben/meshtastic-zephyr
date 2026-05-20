/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

#ifndef ZEPHYR_SUBSYS_MESHTASTIC_SETTINGS_H_
#define ZEPHYR_SUBSYS_MESHTASTIC_SETTINGS_H_

#ifdef __cplusplus
extern "C" {
#endif

int meshtastic_settings_init(void);
void meshtastic_settings_schedule_save(void);
int meshtastic_settings_flush(void);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_SUBSYS_MESHTASTIC_SETTINGS_H_ */
