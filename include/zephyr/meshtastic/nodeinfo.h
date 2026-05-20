/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

/**
 * @file
 * @brief Meshtastic NodeInfo public API.
 */

#ifndef ZEPHYR_INCLUDE_MESHTASTIC_NODEINFO_H_
#define ZEPHYR_INCLUDE_MESHTASTIC_NODEINFO_H_

#include <zephyr/meshtastic/meshtastic.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Announce this node to the mesh.
 *
 * Encodes the node's @c User identity (id, long/short name, MAC address
 * derived from the node ID, hardware model and role) as a @c NODEINFO_APP
 * (port 4) Meshtastic packet and transmits it. Other nodes use this to
 * populate their node database; without it peers only see a bare node
 * number with no name, MAC address or hardware model.
 *
 * When @kconfig{CONFIG_MESHTASTIC_NODEINFO_AUTO_SEND} is enabled the
 * subsystem already broadcasts this periodically; calling this directly is
 * only needed for an explicit, immediate announcement.
 *
 * @param dest Destination node ID, or @ref MESHTASTIC_NODE_BROADCAST.
 *
 * @retval 0        Success.
 * @retval -ENOMEM  Protobuf encoding failed.
 * @retval -EIO     Crypto or radio transmission failed.
 * @retval -ENOTSUP NodeInfo support is not compiled in.
 */
int meshtastic_send_node_info(uint32_t dest);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_MESHTASTIC_NODEINFO_H_ */
