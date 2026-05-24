/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

#include <zephyr/meshtastic/meshtastic.h>

#include "sample_gui.h"
#include "sample_gui_model.h"
#include "sample_gui_renderer.h"

static struct meshtastic_sample_gui_model gui_model;

static const char *sample_gui_channel_name(const struct meshtastic_packet *packet)
{
	if (packet->channel_index != MESHTASTIC_CHANNEL_INDEX_INVALID) {
		return meshtastic_get_channel_name(packet->channel_index);
	}

	return "unknown";
}

int meshtastic_sample_gui_init(void)
{
	int ret;

	meshtastic_sample_gui_model_init(&gui_model, meshtastic_get_node_id());

	ret = meshtastic_sample_gui_model_refresh_status(&gui_model);
	if (ret < 0) {
		return ret;
	}

	ret = meshtastic_sample_gui_renderer_init();
	if (ret < 0) {
		return ret;
	}

	return meshtastic_sample_gui_renderer_process(&gui_model);
}

int meshtastic_sample_gui_process(k_timeout_t timeout)
{
	k_sleep(timeout);

	return 0;
}

void meshtastic_sample_gui_handle_event(const struct meshtastic_event *event)
{
	const struct meshtastic_packet *packet;

	if (event == NULL) {
		return;
	}

	meshtastic_sample_gui_model_set_event(&gui_model, event);

	switch (event->type) {
	case MESHTASTIC_EVENT_TEXT_MESSAGE:
		packet = event->packet;
		if (packet == NULL || packet->payload == NULL) {
			break;
		}

		meshtastic_sample_gui_model_set_text_message(&gui_model, packet,
							     sample_gui_channel_name(packet));
		break;
	default:
		break;
	}

	(void)meshtastic_sample_gui_model_refresh_status(&gui_model);
	(void)meshtastic_sample_gui_renderer_process(&gui_model);
}
