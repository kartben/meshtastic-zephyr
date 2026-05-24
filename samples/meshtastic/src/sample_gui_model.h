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
	uint32_t node_id;
	uint32_t last_sender;
	int16_t last_rssi;
	int8_t last_snr;
	bool has_text_message;
	bool dirty;
	char last_channel[16];
	char last_text[MESHTASTIC_MAX_TEXT_LEN + 1];
};

void meshtastic_sample_gui_model_init(struct meshtastic_sample_gui_model *model, uint32_t node_id);
void meshtastic_sample_gui_model_set_text_message(struct meshtastic_sample_gui_model *model,
						  const struct meshtastic_packet *packet,
						  const char *channel_name);

#endif /* MESHTASTIC_SAMPLE_GUI_MODEL_H_ */
