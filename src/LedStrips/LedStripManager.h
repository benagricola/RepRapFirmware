/*
 * LedStripManager.h
 *
 *  Created on: 30 Apr 2023
 *      Author: David
 */

#ifndef SRC_LEDSTRIPS_LEDSTRIPMANAGER_H_
#define SRC_LEDSTRIPS_LEDSTRIPMANAGER_H_

#include <ObjectModel/ObjectModel.h>

#if SUPPORT_LED_STRIPS

#if SUPPORT_REMOTE_COMMANDS
class CanMessageGeneric;
#endif

class LedStripBase;

class LedStripManager INHERIT_OBJECT_MODEL
{
public:
	LedStripManager() noexcept;

	GCodeResult CreateStrip(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);
	GCodeResult HandleM150(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);
	bool MustStopMovement(GCodeBuffer& gb) noexcept;				// Test whether this strip requires motion to be stopped before sending a command

#if SUPPORT_REMOTE_COMMANDS
	GCodeResult HandleM950Led(const CanMessageGeneric &msg, const StringRef& reply, uint8_t& extra) noexcept;
	GCodeResult HandleLedSetColours(const CanMessageGeneric &msg, const StringRef& reply) noexcept;
#endif

protected:
	DECLARE_OBJECT_MODEL_WITH_ARRAYS

private:
	size_t GetNumLedStrips() const noexcept;

	ReadWriteLock ledLock;
	LedStripBase *strips[MaxLedStrips];
};

#endif

#endif /* SRC_LEDSTRIPS_LEDSTRIPMANAGER_H_ */
