/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

#include "sample_gui_renderer.h"

int meshtastic_sample_gui_renderer_init(void)
{
	return 0;
}

int meshtastic_sample_gui_renderer_process(struct meshtastic_sample_gui_model *model)
{
	if (model != NULL) {
		model->dirty = false;
	}

	return 0;
}
