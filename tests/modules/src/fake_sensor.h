/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

/**
 * @file
 * @brief Test-only sensor backing every environment metrics alias.
 */

#ifndef MESHTASTIC_TESTS_FAKE_SENSOR_H_
#define MESHTASTIC_TESTS_FAKE_SENSOR_H_

#include <stdbool.h>

/** Readings the fake sensor reports, in Zephyr's units. */
struct fake_sensor_values {
	/** Ambient temperature, degrees Celsius. */
	double temperature;
	/** Relative humidity, percent. */
	double humidity;
	/** Barometric pressure, kPa. */
	double pressure;
	/** Gas resistance, ohms. */
	double gas_resistance;
	/** Illuminance, lux. */
	double light;
};

/** Replace the readings the sensor reports. */
void fake_sensor_set_values(const struct fake_sensor_values *values);

/** Make every sample fetch fail, as an absent or broken sensor would. */
void fake_sensor_set_failing(bool failing);

#endif /* MESHTASTIC_TESTS_FAKE_SENSOR_H_ */
