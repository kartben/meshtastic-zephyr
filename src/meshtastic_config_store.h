/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

#ifndef ZEPHYR_SUBSYS_MESHTASTIC_CONFIG_STORE_H_
#define ZEPHYR_SUBSYS_MESHTASTIC_CONFIG_STORE_H_

#include <stddef.h>
#include <stdint.h>

#include "meshtastic/channel.pb.h"
#include "meshtastic/config.pb.h"
#include "meshtastic/module_config.pb.h"

#include <zephyr/meshtastic/meshtastic.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MESHTASTIC_STORE_VALUE_MAX 256U

int meshtastic_config_store_seed(const struct meshtastic_config *cfg);
int meshtastic_config_store_apply_core(void);

const char *meshtastic_config_store_long_name(void);
const char *meshtastic_config_store_short_name(void);

int meshtastic_config_store_get_channel(uint8_t index, meshtastic_Channel *channel);
int meshtastic_config_store_set_channel(uint8_t index, const meshtastic_Channel *channel);

int meshtastic_config_store_get_config(pb_size_t tag, meshtastic_Config *config);
int meshtastic_config_store_set_config(const meshtastic_Config *config);

int meshtastic_config_store_get_module(pb_size_t tag, meshtastic_ModuleConfig *module);
int meshtastic_config_store_set_module(const meshtastic_ModuleConfig *module);

int meshtastic_config_store_get_device_ui(meshtastic_DeviceUIConfig *device_ui);

int meshtastic_config_store_set_device_role(meshtastic_Config_DeviceConfig_Role role);
int meshtastic_config_store_set_rebroadcast_mode(
	meshtastic_Config_DeviceConfig_RebroadcastMode mode);

int meshtastic_config_store_setting_get(const char *key, void *buf, size_t buf_len);
int meshtastic_config_store_setting_set(const char *key, const void *buf, size_t len);
int meshtastic_config_store_export(int (*export_func)(const char *name, const void *val,
						      size_t val_len));

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_SUBSYS_MESHTASTIC_CONFIG_STORE_H_ */
