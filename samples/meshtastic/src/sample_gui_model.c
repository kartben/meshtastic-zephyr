/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

#include <string.h>

#include "sample_gui_model.h"

void meshtastic_sample_gui_model_init(struct meshtastic_sample_gui_model *model, uint32_t node_id)
{
	if (model == NULL) {
		return;
	}

	(void)memset(model, 0, sizeof(*model));
	model->node_id = node_id;
}

void meshtastic_sample_gui_model_set_text_message(struct meshtastic_sample_gui_model *model,
						  const struct meshtastic_packet *packet,
						  const char *channel_name)
{
	size_t channel_len;
	size_t payload_len;

	if (model == NULL || packet == NULL || packet->payload == NULL || channel_name == NULL) {
		return;
	}

	model->last_sender = packet->from;
	model->last_rssi = packet->rssi;
	model->last_snr = packet->snr;
	model->has_text_message = true;
	model->dirty = true;

	channel_len = strlen(channel_name);
	if (channel_len >= sizeof(model->last_channel)) {
		channel_len = sizeof(model->last_channel) - 1U;
	}

	(void)memcpy(model->last_channel, channel_name, channel_len);
	model->last_channel[channel_len] = '\0';

	payload_len = packet->payload_len;
	if (payload_len > MESHTASTIC_MAX_TEXT_LEN) {
		payload_len = MESHTASTIC_MAX_TEXT_LEN;
	}

	(void)memcpy(model->last_text, packet->payload, payload_len);
	model->last_text[payload_len] = '\0';
}
