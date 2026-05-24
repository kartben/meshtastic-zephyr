/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

#ifndef ZEPHYR_SUBSYS_MESHTASTIC_STATS_H_
#define ZEPHYR_SUBSYS_MESHTASTIC_STATS_H_

#include <zephyr/stats/stats.h>

STATS_SECT_START(meshtastic_stats)
	STATS_SECT_ENTRY(tx_packets)
	STATS_SECT_ENTRY(tx_failures)
	STATS_SECT_ENTRY(rx_packets)
	STATS_SECT_ENTRY(relayed_packets)
	STATS_SECT_ENTRY(duplicate_packets)
	STATS_SECT_ENTRY(decode_failures)
	STATS_SECT_ENTRY(rx_dropped)
	STATS_SECT_ENTRY(rx_rearm_failures)
	STATS_SECT_ENTRY(last_rx_from)
	STATS_SECT_ENTRY(last_rssi)
	STATS_SECT_ENTRY(last_snr)
STATS_SECT_END;

extern STATS_SECT_DECL(meshtastic_stats) meshtastic_stats;

int meshtastic_stats_init(void);

#endif /* ZEPHYR_SUBSYS_MESHTASTIC_STATS_H_ */
