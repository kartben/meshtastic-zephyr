/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

/**
 * @file
 * @brief Meshtastic GNSS position public API.
 */

#ifndef ZEPHYR_INCLUDE_MESHTASTIC_GNSS_H_
#define ZEPHYR_INCLUDE_MESHTASTIC_GNSS_H_

#include <zephyr/meshtastic/meshtastic.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Send the latest GNSS position if one is available.
 *
 * @param dest Destination node ID, or @ref MESHTASTIC_NODE_BROADCAST.
 *
 * @retval 0        Success.
 * @retval -ENODATA No GNSS fix is available yet.
 * @retval -ENOMEM  Protobuf encoding failed.
 * @retval -EIO     Crypto or radio transmission failed.
 * @retval -ENOTSUP GNSS support is not compiled in.
 */
int meshtastic_send_position(uint32_t dest);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_MESHTASTIC_GNSS_H_ */
