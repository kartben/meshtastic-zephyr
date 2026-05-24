/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

#include "meshtastic_stats.h"

static bool meshtastic_stats_registered;

STATS_NAME_START(meshtastic_stats)
	STATS_NAME(meshtastic_stats, tx_packets)
	STATS_NAME(meshtastic_stats, tx_failures)
	STATS_NAME(meshtastic_stats, rx_packets)
	STATS_NAME(meshtastic_stats, relayed_packets)
	STATS_NAME(meshtastic_stats, duplicate_packets)
	STATS_NAME(meshtastic_stats, decode_failures)
	STATS_NAME(meshtastic_stats, rx_dropped)
	STATS_NAME(meshtastic_stats, rx_rearm_failures)
	STATS_NAME(meshtastic_stats, last_rx_from)
	STATS_NAME(meshtastic_stats, last_rssi)
	STATS_NAME(meshtastic_stats, last_snr)
STATS_NAME_END(meshtastic_stats);

STATS_SECT_DECL(meshtastic_stats) meshtastic_stats;

int meshtastic_stats_init(void)
{
#if defined(CONFIG_MESHTASTIC_STATS)
	if (!meshtastic_stats_registered) {
		int ret = STATS_INIT_AND_REG(meshtastic_stats, STATS_SIZE_32, "meshtastic");

		if (ret < 0) {
			return ret;
		}

		meshtastic_stats_registered = true;
	} else {
		stats_reset(&meshtastic_stats.s_hdr);
	}
#endif

	return 0;
}
