/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/sys/printk.h>

#include "sample_gui_model.h"

void meshtastic_sample_gui_model_init(struct meshtastic_sample_gui_model *model, uint32_t node_id)
{
	if (model == NULL) {
		return;
	}

	(void)memset(model, 0, sizeof(*model));
	model->status.node_id = node_id;
	(void)snprintk(model->last_event, sizeof(model->last_event), "Waiting for mesh activity");
	model->has_last_event = true;
	model->dirty = true;
}

int meshtastic_sample_gui_model_refresh_status(struct meshtastic_sample_gui_model *model)
{
	struct meshtastic_status status;
	int ret;

	if (model == NULL) {
		return -EINVAL;
	}

	ret = meshtastic_get_status(&status);
	if (ret < 0) {
		return ret;
	}

	if (memcmp(&model->status, &status, sizeof(status)) != 0) {
		model->status = status;
		model->dirty = true;
	}

	return 0;
}

void meshtastic_sample_gui_model_set_event(struct meshtastic_sample_gui_model *model,
					   const struct meshtastic_event *event)
{
	if (model == NULL || event == NULL) {
		return;
	}

	switch (event->type) {
	case MESHTASTIC_EVENT_TEXT_MESSAGE:
		(void)snprintk(model->last_event, sizeof(model->last_event),
			       "Text message received");
		break;
	case MESHTASTIC_EVENT_TX_DONE:
		if (event->packet != NULL) {
			(void)snprintk(model->last_event, sizeof(model->last_event),
				       "TX complete to 0x%08x", event->packet->to);
		} else {
			(void)snprintk(model->last_event, sizeof(model->last_event), "TX complete");
		}
		break;
	case MESHTASTIC_EVENT_TX_FAILED:
		(void)snprintk(model->last_event, sizeof(model->last_event), "TX failed (%d)",
			       event->err);
		break;
	case MESHTASTIC_EVENT_BLE_CONNECTED:
		(void)snprintk(model->last_event, sizeof(model->last_event), "BLE connected");
		break;
	case MESHTASTIC_EVENT_BLE_DISCONNECTED:
		(void)snprintk(model->last_event, sizeof(model->last_event), "BLE disconnected");
		break;
	case MESHTASTIC_EVENT_GNSS_FIX:
		(void)snprintk(model->last_event, sizeof(model->last_event), "GNSS fix updated");
		break;
	case MESHTASTIC_EVENT_METRICS_ERROR:
		(void)snprintk(model->last_event, sizeof(model->last_event), "Metrics error (%d)",
			       event->err);
		break;
	case MESHTASTIC_EVENT_PACKET_RECEIVED:
	default:
		return;
	}

	model->has_last_event = true;
	model->dirty = true;
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

	model->last_message_sender = packet->from;
	model->last_message_rssi = packet->rssi;
	model->last_message_snr = packet->snr;
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
