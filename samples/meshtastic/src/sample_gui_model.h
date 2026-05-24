/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

#ifndef MESHTASTIC_SAMPLE_GUI_MODEL_H_
#define MESHTASTIC_SAMPLE_GUI_MODEL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/meshtastic/meshtastic.h>

struct meshtastic_sample_gui_model {
	struct meshtastic_status status;
	uint32_t last_message_sender;
	int16_t last_message_rssi;
	int8_t last_message_snr;
	bool has_text_message;
	bool has_last_event;
	bool dirty;
	char last_event[48];
	char last_channel[16];
	char last_text[MESHTASTIC_MAX_TEXT_LEN + 1];
};

void meshtastic_sample_gui_model_init(struct meshtastic_sample_gui_model *model, uint32_t node_id);
int meshtastic_sample_gui_model_refresh_status(struct meshtastic_sample_gui_model *model);
void meshtastic_sample_gui_model_set_event(struct meshtastic_sample_gui_model *model,
					   const struct meshtastic_event *event);
void meshtastic_sample_gui_model_set_text_message(struct meshtastic_sample_gui_model *model,
						  const struct meshtastic_packet *packet,
						  const char *channel_name);

#endif /* MESHTASTIC_SAMPLE_GUI_MODEL_H_ */
