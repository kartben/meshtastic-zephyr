/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

#include "sample_gui.h"

int meshtastic_sample_gui_init(void)
{
	return 0;
}

int meshtastic_sample_gui_process(k_timeout_t timeout)
{
	k_sleep(timeout);

	return 0;
}

void meshtastic_sample_gui_handle_event(const struct meshtastic_event *event)
{
	ARG_UNUSED(event);
}
