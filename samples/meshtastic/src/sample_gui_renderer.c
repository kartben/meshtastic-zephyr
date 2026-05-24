/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

#include <errno.h>

#include <zephyr/logging/log.h>

#if defined(CONFIG_MESHTASTIC_SAMPLE_GUI_LVGL)
#include <lvgl.h>
#include <zephyr/devicetree.h>

#if defined(CONFIG_DISPLAY) && DT_HAS_CHOSEN(zephyr_display)
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#endif
#endif

#include "sample_gui_renderer.h"

LOG_MODULE_REGISTER(meshtastic_sample_gui, LOG_LEVEL_INF);

enum meshtastic_sample_gui_backend {
	MESHTASTIC_SAMPLE_GUI_BACKEND_LOG,
	MESHTASTIC_SAMPLE_GUI_BACKEND_LVGL,
};

struct meshtastic_sample_gui_view {
	enum meshtastic_sample_gui_backend backend;
#if defined(CONFIG_MESHTASTIC_SAMPLE_GUI_LVGL)
	lv_obj_t *screen;
	lv_obj_t *title_label;
	lv_obj_t *status_label;
	lv_obj_t *event_label;
	lv_obj_t *message_label;
#endif
};

static struct meshtastic_sample_gui_view gui_view = {
	.backend = MESHTASTIC_SAMPLE_GUI_BACKEND_LOG,
};

#if defined(CONFIG_MESHTASTIC_SAMPLE_GUI_LVGL)
#if defined(LV_VERSION_MAJOR) && (LV_VERSION_MAJOR >= 9)
#define SAMPLE_GUI_LV_SCREEN_ACTIVE() lv_screen_active()
#else
#define SAMPLE_GUI_LV_SCREEN_ACTIVE() lv_scr_act()
#endif

static void sample_gui_renderer_update_lvgl(struct meshtastic_sample_gui_model *model)
{
	lv_label_set_text_fmt(gui_view.title_label, "Meshtastic sample\nNode 0x%08x",
			      model->status.node_id);
	lv_label_set_text_fmt(gui_view.status_label,
			      "Status\nBLE: %s\nTX: %u ok / %u failed\nRX: %u decoded / %u dup\n"
			      "Relayed: %u\nLast RX: 0x%08x RSSI %d SNR %d",
			      model->status.ble_connected ? "connected" : "idle",
			      model->status.tx_packets, model->status.tx_failures,
			      model->status.rx_packets, model->status.duplicate_packets,
			      model->status.relayed_packets, model->status.last_rx_from,
			      model->status.last_rssi, model->status.last_snr);

	if (model->has_last_event) {
		lv_label_set_text_fmt(gui_view.event_label, "Last event\n%s", model->last_event);
	} else {
		lv_label_set_text(gui_view.event_label, "Last event\nNone yet");
	}

	if (model->has_text_message) {
		lv_label_set_text_fmt(gui_view.message_label,
				      "Last message\nFrom: 0x%08x on %s\nRSSI %d SNR %d\n%s",
				      model->last_message_sender, model->last_channel,
				      model->last_message_rssi, model->last_message_snr,
				      model->last_text);
	} else {
		lv_label_set_text(gui_view.message_label, "Last message\nNo text messages yet");
	}
}

static int sample_gui_renderer_init_lvgl(void)
{
#if defined(CONFIG_DISPLAY) && DT_HAS_CHOSEN(zephyr_display)
	const struct device *display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

	if (!device_is_ready(display_dev)) {
		return -ENODEV;
	}
#endif

	gui_view.screen = SAMPLE_GUI_LV_SCREEN_ACTIVE();
	if (gui_view.screen == NULL) {
		return -ENODEV;
	}

	lv_obj_clean(gui_view.screen);

	gui_view.title_label = lv_label_create(gui_view.screen);
	gui_view.status_label = lv_label_create(gui_view.screen);
	gui_view.event_label = lv_label_create(gui_view.screen);
	gui_view.message_label = lv_label_create(gui_view.screen);

	if (gui_view.title_label == NULL || gui_view.status_label == NULL ||
	    gui_view.event_label == NULL || gui_view.message_label == NULL) {
		return -ENOMEM;
	}

	lv_obj_set_width(gui_view.title_label, lv_pct(96));
	lv_obj_set_width(gui_view.status_label, lv_pct(96));
	lv_obj_set_width(gui_view.event_label, lv_pct(96));
	lv_obj_set_width(gui_view.message_label, lv_pct(96));

	lv_label_set_long_mode(gui_view.status_label, LV_LABEL_LONG_WRAP);
	lv_label_set_long_mode(gui_view.event_label, LV_LABEL_LONG_WRAP);
	lv_label_set_long_mode(gui_view.message_label, LV_LABEL_LONG_WRAP);

	lv_obj_align(gui_view.title_label, LV_ALIGN_TOP_LEFT, 6, 6);
	lv_obj_align_to(gui_view.status_label, gui_view.title_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 12);
	lv_obj_align_to(gui_view.event_label, gui_view.status_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 12);
	lv_obj_align_to(gui_view.message_label, gui_view.event_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0,
			12);

#if defined(CONFIG_DISPLAY) && DT_HAS_CHOSEN(zephyr_display)
	(void)display_blanking_off(display_dev);
#endif

	lv_timer_handler();

	return 0;
}
#endif

static void sample_gui_renderer_update_log(struct meshtastic_sample_gui_model *model)
{
	LOG_INF("UI status: node=0x%08x ble=%s tx=%u/%u rx=%u dup=%u relayed=%u last=0x%08x %d/%d",
		model->status.node_id, model->status.ble_connected ? "connected" : "idle",
		model->status.tx_packets, model->status.tx_failures, model->status.rx_packets,
		model->status.duplicate_packets, model->status.relayed_packets,
		model->status.last_rx_from, model->status.last_rssi, model->status.last_snr);

	if (model->has_last_event) {
		LOG_INF("UI event: %s", model->last_event);
	}

	if (model->has_text_message) {
		LOG_INF("UI message from 0x%08x on %s (%d/%d): %s", model->last_message_sender,
			model->last_channel, model->last_message_rssi, model->last_message_snr,
			model->last_text);
	}
}

int meshtastic_sample_gui_renderer_init(void)
{
#if defined(CONFIG_MESHTASTIC_SAMPLE_GUI_LVGL)
	int ret;

	ret = sample_gui_renderer_init_lvgl();
	if (ret == 0) {
		gui_view.backend = MESHTASTIC_SAMPLE_GUI_BACKEND_LVGL;
		LOG_INF("Sample GUI renderer backend: LVGL");
		return 0;
	}

	LOG_WRN("LVGL renderer unavailable (%d), using log fallback", ret);
#endif

	gui_view.backend = MESHTASTIC_SAMPLE_GUI_BACKEND_LOG;
	LOG_INF("Sample GUI renderer backend: log");

	return 0;
}

int meshtastic_sample_gui_renderer_process(struct meshtastic_sample_gui_model *model)
{
	if (model == NULL) {
		return -EINVAL;
	}

#if defined(CONFIG_MESHTASTIC_SAMPLE_GUI_LVGL)
	if (gui_view.backend == MESHTASTIC_SAMPLE_GUI_BACKEND_LVGL) {
		if (model->dirty) {
			sample_gui_renderer_update_lvgl(model);
			model->dirty = false;
		}

		lv_timer_handler();
		return 0;
	}
#endif

	if (model->dirty) {
		sample_gui_renderer_update_log(model);
		model->dirty = false;
	}

	return 0;
}
