/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

#define DT_DRV_COMPAT vnd_fake_sensor

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>

#include "fake_sensor.h"

static struct fake_sensor_values values;
static bool failing;

void fake_sensor_set_values(const struct fake_sensor_values *new_values)
{
	values = *new_values;
}

void fake_sensor_set_failing(bool fail)
{
	failing = fail;
}

static int fake_sensor_sample_fetch(const struct device *dev, enum sensor_channel chan)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(chan);

	return failing ? -EIO : 0;
}

static int fake_sensor_channel_get(const struct device *dev, enum sensor_channel chan,
				   struct sensor_value *val)
{
	double reading;

	ARG_UNUSED(dev);

	switch (chan) {
	case SENSOR_CHAN_AMBIENT_TEMP:
		reading = values.temperature;
		break;
	case SENSOR_CHAN_HUMIDITY:
		reading = values.humidity;
		break;
	case SENSOR_CHAN_PRESS:
		reading = values.pressure;
		break;
	case SENSOR_CHAN_GAS_RES:
		reading = values.gas_resistance;
		break;
	case SENSOR_CHAN_LIGHT:
		reading = values.light;
		break;
	default:
		return -ENOTSUP;
	}

	return sensor_value_from_double(val, reading);
}

static DEVICE_API(sensor, fake_sensor_api) = {
	.sample_fetch = fake_sensor_sample_fetch,
	.channel_get = fake_sensor_channel_get,
};

static int fake_sensor_init(const struct device *dev)
{
	ARG_UNUSED(dev);

	return 0;
}

DEVICE_DT_INST_DEFINE(0, fake_sensor_init, NULL, NULL, NULL, POST_KERNEL,
		      CONFIG_SENSOR_INIT_PRIORITY, &fake_sensor_api);
