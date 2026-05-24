/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

#ifndef MESHTASTIC_SAMPLE_GUI_H_
#define MESHTASTIC_SAMPLE_GUI_H_

#include <zephyr/kernel.h>

struct meshtastic_event;

int meshtastic_sample_gui_init(void);
int meshtastic_sample_gui_process(k_timeout_t timeout);
void meshtastic_sample_gui_handle_event(const struct meshtastic_event *event);

#endif /* MESHTASTIC_SAMPLE_GUI_H_ */
