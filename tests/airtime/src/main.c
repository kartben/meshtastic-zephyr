#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "meshtastic/telemetry.pb.h"
#include "meshtastic_airtime.h"

int meshtastic_collect_device_metrics(meshtastic_DeviceMetrics *metrics);

#define CHANNEL_WINDOW_MS (MESHTASTIC_CHANNEL_UTILIZATION_PERIODS * 10U * 1000U)

static float channel_percent(uint32_t ms)
{
	return ((float)ms / (float)CHANNEL_WINDOW_MS) * 100.0f;
}

static float tx_percent(uint32_t ms)
{
	return ((float)ms / (float)MESHTASTIC_MS_IN_HOUR) * 100.0f;
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);

	zassert_ok(meshtastic_airtime_init(), "meshtastic_airtime_init failed");
}

ZTEST(airtime_metrics, test_utilization_starts_at_zero)
{
	zassert_equal(meshtastic_airtime_channel_util_percent(), 0.0f);
	zassert_equal(meshtastic_airtime_tx_util_percent(), 0.0f);
}

ZTEST(airtime_metrics, test_tx_updates_channel_and_tx_util)
{
	meshtastic_airtime_log(MESHTASTIC_AIRTIME_TX, 100U);

	zassert_within(meshtastic_airtime_channel_util_percent(), channel_percent(100U), 0.001f);
	zassert_within(meshtastic_airtime_tx_util_percent(), tx_percent(100U), 0.001f);
}

ZTEST(airtime_metrics, test_rx_updates_channel_util_only)
{
	meshtastic_airtime_log(MESHTASTIC_AIRTIME_TX, 50U);
	meshtastic_airtime_log(MESHTASTIC_AIRTIME_RX, 150U);
	meshtastic_airtime_log(MESHTASTIC_AIRTIME_RX_ALL, 25U);

	zassert_within(meshtastic_airtime_channel_util_percent(), channel_percent(225U), 0.001f);
	zassert_within(meshtastic_airtime_tx_util_percent(), tx_percent(50U), 0.001f);
}

ZTEST(airtime_metrics, test_utilization_survives_timer_tick)
{
	meshtastic_airtime_log(MESHTASTIC_AIRTIME_TX, 80U);

	k_sleep(K_MSEC(1200));

	zassert_within(meshtastic_airtime_channel_util_percent(), channel_percent(80U), 0.001f);
	zassert_within(meshtastic_airtime_tx_util_percent(), tx_percent(80U), 0.001f);

	meshtastic_airtime_log(MESHTASTIC_AIRTIME_RX, 40U);
	zassert_within(meshtastic_airtime_channel_util_percent(), channel_percent(120U), 0.001f);
	zassert_within(meshtastic_airtime_tx_util_percent(), tx_percent(80U), 0.001f);
}

ZTEST(airtime_metrics, test_packet_ms_is_zero_without_radio)
{
	zassert_equal(meshtastic_airtime_packet_ms(48U), 0U);
}

ZTEST(airtime_metrics, test_device_metrics_include_airtime)
{
	meshtastic_DeviceMetrics metrics;
	int ret;

	meshtastic_airtime_log(MESHTASTIC_AIRTIME_TX, 100U);
	meshtastic_airtime_log(MESHTASTIC_AIRTIME_RX, 200U);

	ret = meshtastic_collect_device_metrics(&metrics);
	zassert_ok(ret, "meshtastic_collect_device_metrics failed: %d", ret);

	zassert_true(metrics.has_uptime_seconds);
	zassert_true(metrics.has_channel_utilization);
	zassert_true(metrics.has_air_util_tx);
	zassert_within(metrics.channel_utilization, channel_percent(300U), 0.001f);
	zassert_within(metrics.air_util_tx, tx_percent(100U), 0.001f);
}

ZTEST_SUITE(airtime_metrics, NULL, NULL, before, NULL, NULL);
