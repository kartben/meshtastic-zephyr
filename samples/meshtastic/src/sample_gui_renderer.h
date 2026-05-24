/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

#ifndef MESHTASTIC_SAMPLE_GUI_RENDERER_H_
#define MESHTASTIC_SAMPLE_GUI_RENDERER_H_

#include "sample_gui_model.h"

int meshtastic_sample_gui_renderer_init(void);
int meshtastic_sample_gui_renderer_process(struct meshtastic_sample_gui_model *model);

#endif /* MESHTASTIC_SAMPLE_GUI_RENDERER_H_ */
