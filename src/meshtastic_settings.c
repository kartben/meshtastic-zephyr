/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

#include "meshtastic_config_store.h"
#include "meshtastic_settings.h"

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(meshtastic, CONFIG_MESHTASTIC_LOG_LEVEL);

#define MESHTASTIC_SETTINGS_SUBTREE "meshtastic"

static void save_work_handler(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(save_work, save_work_handler);
static int (*active_export_func)(const char *name, const void *val, size_t val_len);

static int settings_get_cb(const char *key, char *val, int val_len_max)
{
	return meshtastic_config_store_setting_get(key, val, (size_t)val_len_max);
}

static int settings_set_cb(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	uint8_t buf[MESHTASTIC_STORE_VALUE_MAX];
	ssize_t read;
	int ret;

	if (len > sizeof(buf)) {
		LOG_WRN("Ignoring oversized Meshtastic setting '%s' (%zu bytes)", key, len);
		return 0;
	}

	read = read_cb(cb_arg, buf, len);
	if (read < 0) {
		LOG_WRN("Reading Meshtastic setting '%s' failed (%d)", key, (int)read);
		return 0;
	}

	if ((size_t)read != len) {
		LOG_WRN("Ignoring short Meshtastic setting '%s' (%d/%zu bytes)", key, (int)read,
			len);
		return 0;
	}

	ret = meshtastic_config_store_setting_set(key, buf, len);
	if (ret < 0) {
		LOG_WRN("Ignoring invalid Meshtastic setting '%s' (%d)", key, ret);
	}

	return 0;
}

static int settings_export_prefixed(const char *name, const void *val, size_t val_len)
{
	char full_name[SETTINGS_MAX_NAME_LEN + SETTINGS_EXTRA_LEN + 1];
	int ret;

	ret = snprintk(full_name, sizeof(full_name), MESHTASTIC_SETTINGS_SUBTREE "/%s", name);
	if (ret < 0 || ret >= sizeof(full_name)) {
		return -EINVAL;
	}

	return active_export_func(full_name, val, val_len);
}

static int settings_export_cb(int (*export_func)(const char *name, const void *val, size_t val_len))
{
	int ret;

	active_export_func = export_func;
	ret = meshtastic_config_store_export(settings_export_prefixed);
	active_export_func = NULL;

	return ret;
}

SETTINGS_STATIC_HANDLER_DEFINE(meshtastic, MESHTASTIC_SETTINGS_SUBTREE, settings_get_cb,
			       settings_set_cb, NULL, settings_export_cb);

static void save_work_handler(struct k_work *work)
{
	int ret;

	ARG_UNUSED(work);

	ret = settings_save_subtree(MESHTASTIC_SETTINGS_SUBTREE);
	if (ret < 0) {
		LOG_WRN("Meshtastic settings save failed (%d)", ret);
	}
}

int meshtastic_settings_init(void)
{
	int ret;

	ret = settings_subsys_init();
	if (ret < 0) {
		LOG_ERR("settings_subsys_init failed (%d)", ret);
		return ret;
	}

	ret = settings_load_subtree(MESHTASTIC_SETTINGS_SUBTREE);
	if (ret < 0) {
		LOG_ERR("Meshtastic settings load failed (%d)", ret);
		return ret;
	}

	return 0;
}

void meshtastic_settings_schedule_save(void)
{
	(void)k_work_reschedule(&save_work, K_MSEC(CONFIG_MESHTASTIC_SETTINGS_SAVE_DELAY_MS));
}

int meshtastic_settings_flush(void)
{
	(void)k_work_cancel_delayable(&save_work);

	return settings_save_subtree(MESHTASTIC_SETTINGS_SUBTREE);
}
