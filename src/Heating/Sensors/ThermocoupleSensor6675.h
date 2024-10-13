/*
 * ThermocoupleSensor6675.h
 *
 *  Created on: 14 Jan 2021
 *      Author: GA (based on ThermocoupleSensor31855.h
 */

#ifndef SRC_HEATING_THERMOCOUPLESENSOR6675_H_
#define SRC_HEATING_THERMOCOUPLESENSOR6675_H_

#include "SpiTemperatureSensor.h"

class ThermocoupleSensor6675 : public SpiTemperatureSensor
{
public:
	ThermocoupleSensor6675(unsigned int sensorNum) noexcept;
	GCodeResult Configure(GCodeBuffer& gb, const StringRef& reply, bool& changed) THROWS(GCodeException) override;

#if SUPPORT_REMOTE_COMMANDS
	GCodeResult Configure(const CanMessageGenericParser& parser, const StringRef& reply) noexcept override; // configure the sensor from M308 parameters
#endif
	void Poll() noexcept override;
	const char *GetShortSensorType() const noexcept override { return TypeName; }

	static constexpr const char *TypeName = "thermocouplemax6675";
};

#endif /* SRC_HEATING_THERMOCOUPLESENSOR6675_H_ */
