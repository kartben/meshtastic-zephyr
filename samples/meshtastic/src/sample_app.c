/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zephyr/meshtastic/meshtastic.h>

#include "sample_app.h"
#include "sample_gui.h"

LOG_MODULE_REGISTER(meshtastic_sample, LOG_LEVEL_INF);

static const struct device *lora_dev = DEVICE_DT_GET(DT_ALIAS(lora0));

static const char *packet_channel_name(const struct meshtastic_packet *packet)
{
	if (packet->channel_index != MESHTASTIC_CHANNEL_INDEX_INVALID) {
		return meshtastic_get_channel_name(packet->channel_index);
	}

	return "unknown";
}

static void on_event(const struct meshtastic_event *event, void *user_data)
{
	const struct meshtastic_packet *packet;

	ARG_UNUSED(user_data);

	if (event == NULL) {
		return;
	}

	meshtastic_sample_gui_handle_event(event);

	switch (event->type) {
	case MESHTASTIC_EVENT_TEXT_MESSAGE:
		packet = event->packet;
		if (packet == NULL || packet->payload == NULL) {
			break;
		}

		LOG_INF("MSG from 0x%08x on \"%s\": %.*s  (RSSI %d dBm, SNR %d)", packet->from,
			packet_channel_name(packet), (int)packet->payload_len,
			(const char *)packet->payload, (int)packet->rssi, (int)packet->snr);
		break;
	default:
		break;
	}
}

int meshtastic_sample_run(void)
{
	struct meshtastic_config cfg = {
		.lora_dev = lora_dev,
		.node_id = 0,
		.psk = meshtastic_default_psk,
		.psk_len = sizeof(meshtastic_default_psk),
		.channel_name = MESHTASTIC_CHANNEL_LONGFAST,
#if defined(CONFIG_MESHTASTIC_TEST_LORA_SEND_PARAMS)
		.frequency = 865100000U,
		.tx_power = CONFIG_MESHTASTIC_TX_POWER,
#else
		.frequency = MESHTASTIC_FREQ_EU,
#endif
	};
	int ret;

	if (!device_is_ready(lora_dev)) {
		LOG_ERR("LoRa device not ready");
		return -ENODEV;
	}

	ret = meshtastic_init(&cfg);
	if (ret < 0) {
		LOG_ERR("meshtastic_init failed (%d)", ret);
		return ret;
	}

	ret = meshtastic_sample_gui_init();
	if (ret < 0) {
		LOG_ERR("sample GUI init failed (%d)", ret);
		return ret;
	}

	meshtastic_set_event_cb(on_event, NULL);

	LOG_INF("Meshtastic sample started, node ID 0x%08x",
		meshtastic_get_node_id());

	while (true) {
		ret = meshtastic_send_text(MESHTASTIC_NODE_BROADCAST,
					   "Hello from " CONFIG_BOARD_TARGET "!");
		if (ret < 0) {
			LOG_ERR("meshtastic_send_text failed (%d)", ret);
		} else {
			LOG_INF("Broadcast sent");
		}

		ret = meshtastic_sample_gui_process(K_SECONDS(300));
		if (ret < 0) {
			LOG_ERR("sample GUI process failed (%d)", ret);
			return ret;
		}
	}

	return 0;
}
