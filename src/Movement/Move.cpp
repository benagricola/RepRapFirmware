/*
 * Move.cpp
 *
 *  Created on: 7 Dec 2014
 *      Author: David

 A note on bed levelling:

 As at version 1.21 we support two types of bed compensation:
 1. The old 3, 4 and 5-point compensation using a RandomProbePointSet. We will probably discontinue this soon.
 2. Mesh bed levelling

 There is an interaction between using G30 to home Z or set a precise Z=0 height just before a print, and bed compensation.
 Consider the following sequence:
 1. Home Z, using either G30 or an endstop.
 2. Run G29 to generate a height map. If the Z=0 point has drifted off, the height map may have a Z offset.
 3. Use G30 to get an accurate Z=0 point. We want to keep the shape of the height map, but get rid of the offset.
 4. Run G29 to generate a height map. This should generate a height map with on offset at the point we just probed.
 5. Cancel bed compensation. The height at the point we just probed should be zero.

 So as well as maintaining a height map, we maintain a Z offset from it. The procedure is:
 1. Whenever bed compensation is not being used, the Z offset should be zero.
 2. Whenever we run G29 to probe the bed, we have a choice:
 (a) accept that the map may have a height offset; and set the Z offset to zero. This is what we do currently.
 (b) normalise the height map to zero, adjust the Z=0 origin, and set the Z offset to zero.
 3. When we run G30 to reset the Z=0 height, and we have a height map loaded, we adjust the Z offset to be the negative of the
    height map indication of that point.
 4. If we now cancel the height map, we also clear the Z offset, and the height at the point we probed remains correct.
 5. If we now run G29 to probe again, the height map should have near zero offset at the point we probed, if there has been no drift.

 Before we introduced the Z offset, at step 4 we would have a potentially large Z error as if the G30 hadn't been run,
 and at step 5 the new height map would have an offset again.

 */

#include "Move.h"
#include "MoveDebugFlags.h"
#include "StepTimer.h"
#include <Platform/Platform.h>
#include <GCodes/GCodes.h>
#include <GCodes/GCodeBuffer/GCodeBuffer.h>
#include <Tools/Tool.h>
#include <Endstops/ZProbe.h>
#include <Platform/TaskPriorities.h>
#include <AppNotifyIndices.h>

#if SUPPORT_IOBITS
# include <Platform/PortControl.h>
#endif

#if SUPPORT_CAN_EXPANSION
# include <CAN/CanMotion.h>
# include <CAN/CanInterface.h>
#endif

Task<Move::MoveTaskStackWords> Move::moveTask;

// Object model table and functions
// Note: if using GCC version 7.3.1 20180622 and lambda functions are used in this table, you must compile this file with option -std=gnu++17.
// Otherwise the table will be allocated in RAM instead of flash, which wastes too much RAM.

// Macro to build a standard lambda function that includes the necessary type conversions
#define OBJECT_MODEL_FUNC(...)					OBJECT_MODEL_FUNC_BODY(Move, __VA_ARGS__)
#define OBJECT_MODEL_FUNC_IF(_condition, ...)	OBJECT_MODEL_FUNC_IF_BODY(Move, _condition, __VA_ARGS__)

constexpr ObjectModelArrayTableEntry Move::objectModelArrayTable[] =
{
	// 0. Axes
	{
		nullptr,					// no lock needed
		[] (const ObjectModel *self, const ObjectExplorationContext& context) noexcept -> size_t
				{
					const size_t numAxes = reprap.GetGCodes().GetTotalAxes();
					// The array gets too large to send when we want all fields and there are a lot of axes, so restrict the number of axes returned to 9
					return (context.TruncateLongArrays()) ? min<size_t>(numAxes, 9) : numAxes;
				},
		[] (const ObjectModel *self, ObjectExplorationContext& context) noexcept -> ExpressionValue { return ExpressionValue(&reprap.GetPlatform(), 3); }
	},
	// 1. Extruders
	{
		nullptr,					// no lock needed
		[] (const ObjectModel *self, const ObjectExplorationContext&) noexcept -> size_t { return reprap.GetGCodes().GetNumExtruders(); },
		[] (const ObjectModel *self, ObjectExplorationContext& context) noexcept -> ExpressionValue { return ExpressionValue(&reprap.GetPlatform(), 4); }
	},
	// 2. Motion system queues
	{
		nullptr,					// no lock needed
		[] (const ObjectModel *self, const ObjectExplorationContext&) noexcept -> size_t { return ARRAY_SIZE(rings); },
		[] (const ObjectModel *self, ObjectExplorationContext& context) noexcept -> ExpressionValue { return ExpressionValue(&((const Move*)self)->rings[context.GetLastIndex()]); }
	},

#if SUPPORT_COORDINATE_ROTATION
	// 3. Rotation centre coordinates
	{
		nullptr,					// no lock needed
		[] (const ObjectModel *self, const ObjectExplorationContext&) noexcept -> size_t { return 2; },
		[] (const ObjectModel *self, ObjectExplorationContext& context) noexcept -> ExpressionValue { return ExpressionValue(reprap.GetGCodes().GetRotationCentre(context.GetLastIndex())); }
	},
#endif

#if SUPPORT_KEEPOUT_ZONES
	// 4. Keepout zone list
	{
		nullptr,					// no lock needed
		[] (const ObjectModel *self, const ObjectExplorationContext&) noexcept -> size_t { return reprap.GetGCodes().GetNumKeepoutZones(); },
		[] (const ObjectModel *self, ObjectExplorationContext& context) noexcept -> ExpressionValue
				{ return (reprap.GetGCodes().IsKeepoutZoneDefined(context.GetLastIndex())) ? ExpressionValue(reprap.GetGCodes().GetKeepoutZone(context.GetLastIndex())) : ExpressionValue(nullptr); }
	},
#endif
};

DEFINE_GET_OBJECT_MODEL_ARRAY_TABLE(Move)

constexpr ObjectModelTableEntry Move::objectModelTable[] =
{
	// Within each group, these entries must be in alphabetical order
	// 0. Move members
	{ "axes",					OBJECT_MODEL_FUNC_ARRAY(0), 																	ObjectModelEntryFlags::live },
	{ "backlashFactor",			OBJECT_MODEL_FUNC_NOSELF((int32_t)reprap.GetPlatform().GetBacklashCorrectionDistanceFactor()),	ObjectModelEntryFlags::none },
	{ "calibration",			OBJECT_MODEL_FUNC(self, 3),																		ObjectModelEntryFlags::none },
	{ "compensation",			OBJECT_MODEL_FUNC(self, 6),																		ObjectModelEntryFlags::none },
	{ "currentMove",			OBJECT_MODEL_FUNC(self, 2),																		ObjectModelEntryFlags::live },
	{ "extruders",				OBJECT_MODEL_FUNC_ARRAY(1),																		ObjectModelEntryFlags::live },
	{ "idle",					OBJECT_MODEL_FUNC(self, 1),																		ObjectModelEntryFlags::none },
#if SUPPORT_KEEPOUT_ZONES
	{ "keepout",				OBJECT_MODEL_FUNC_ARRAY(4),																		ObjectModelEntryFlags::none },
#endif
	{ "kinematics",				OBJECT_MODEL_FUNC(self->kinematics),															ObjectModelEntryFlags::none },
	{ "limitAxes",				OBJECT_MODEL_FUNC_NOSELF(reprap.GetGCodes().LimitAxes()),										ObjectModelEntryFlags::none },
	{ "noMovesBeforeHoming",	OBJECT_MODEL_FUNC_NOSELF(reprap.GetGCodes().NoMovesBeforeHoming()),								ObjectModelEntryFlags::none },
	{ "printingAcceleration",	OBJECT_MODEL_FUNC_NOSELF(InverseConvertAcceleration(reprap.GetGCodes().GetPrimaryMaxPrintingAcceleration()), 1),	ObjectModelEntryFlags::none },
	{ "queue",					OBJECT_MODEL_FUNC_ARRAY(2),																		ObjectModelEntryFlags::none },
#if SUPPORT_COORDINATE_ROTATION
	{ "rotation",				OBJECT_MODEL_FUNC(self, 9),																		ObjectModelEntryFlags::none },
#endif
	{ "shaping",				OBJECT_MODEL_FUNC(&self->axisShaper, 0),														ObjectModelEntryFlags::none },
	{ "speedFactor",			OBJECT_MODEL_FUNC_NOSELF(reprap.GetGCodes().GetPrimarySpeedFactor(), 2),						ObjectModelEntryFlags::none },
	{ "travelAcceleration",		OBJECT_MODEL_FUNC_NOSELF(InverseConvertAcceleration(reprap.GetGCodes().GetPrimaryMaxTravelAcceleration()), 1),		ObjectModelEntryFlags::none },
	{ "virtualEPos",			OBJECT_MODEL_FUNC_NOSELF(reprap.GetGCodes().GetCurrentMovementState(context).latestVirtualExtruderPosition, 5),		ObjectModelEntryFlags::live },
	{ "workplaceNumber",		OBJECT_MODEL_FUNC_NOSELF((int32_t)reprap.GetGCodes().GetPrimaryWorkplaceCoordinateSystemNumber() - 1),				ObjectModelEntryFlags::none },

	// 1. Move.Idle members
	{ "factor",					OBJECT_MODEL_FUNC_NOSELF(reprap.GetPlatform().GetIdleCurrentFactor(), 1),						ObjectModelEntryFlags::none },
	{ "timeout",				OBJECT_MODEL_FUNC(0.001f * (float)self->idleTimeout, 1),										ObjectModelEntryFlags::none },

	// 2. move.currentMove members
	{ "acceleration",			OBJECT_MODEL_FUNC(self->GetAccelerationMmPerSecSquared(), 1),									ObjectModelEntryFlags::live },
	{ "deceleration",			OBJECT_MODEL_FUNC(self->GetDecelerationMmPerSecSquared(), 1),									ObjectModelEntryFlags::live },
	{ "extrusionRate",			OBJECT_MODEL_FUNC(self->GetTotalExtrusionRate(), 2),											ObjectModelEntryFlags::live },
# if SUPPORT_LASER
	{ "laserPwm",				OBJECT_MODEL_FUNC_IF_NOSELF(reprap.GetGCodes().GetMachineType() == MachineType::laser,
															reprap.GetPlatform().GetLaserPwm(), 2),								ObjectModelEntryFlags::live },
# endif
	{ "requestedSpeed",			OBJECT_MODEL_FUNC(self->GetRequestedSpeedMmPerSec(), 1),										ObjectModelEntryFlags::live },
	{ "topSpeed",				OBJECT_MODEL_FUNC(self->GetTopSpeedMmPerSec(), 1),												ObjectModelEntryFlags::live },

	// 3. move.calibration members
	{ "final",					OBJECT_MODEL_FUNC(self, 5),																		ObjectModelEntryFlags::none },
	{ "initial",				OBJECT_MODEL_FUNC(self, 4),																		ObjectModelEntryFlags::none },
	{ "numFactors",				OBJECT_MODEL_FUNC((int32_t)self->numCalibratedFactors),											ObjectModelEntryFlags::none },

	// 4. move.calibration.initialDeviation members
	{ "deviation",				OBJECT_MODEL_FUNC(self->initialCalibrationDeviation.GetDeviationFromMean(), 3),					ObjectModelEntryFlags::none },
	{ "mean",					OBJECT_MODEL_FUNC(self->initialCalibrationDeviation.GetMean(), 3),								ObjectModelEntryFlags::none },

	// 5. move.calibration.finalDeviation members
	{ "deviation",				OBJECT_MODEL_FUNC(self->latestCalibrationDeviation.GetDeviationFromMean(), 3),					ObjectModelEntryFlags::none },
	{ "mean",					OBJECT_MODEL_FUNC(self->latestCalibrationDeviation.GetMean(), 3),								ObjectModelEntryFlags::none },

	// 6. move.compensation members
	{ "fadeHeight",				OBJECT_MODEL_FUNC((self->useTaper) ? self->taperHeight : std::numeric_limits<float>::quiet_NaN(), 1),	ObjectModelEntryFlags::none },
#if HAS_MASS_STORAGE || HAS_SBC_INTERFACE
	{ "file",					OBJECT_MODEL_FUNC_IF(self->usingMesh, self->heightMap.GetFileName()),							ObjectModelEntryFlags::none },
#endif
	{ "liveGrid",				OBJECT_MODEL_FUNC_IF(self->usingMesh, (const GridDefinition *)&self->GetGrid()),				ObjectModelEntryFlags::none },
	{ "meshDeviation",			OBJECT_MODEL_FUNC_IF(self->usingMesh, self, 7),													ObjectModelEntryFlags::none },
	{ "probeGrid",				OBJECT_MODEL_FUNC_NOSELF((const GridDefinition *)&reprap.GetGCodes().GetDefaultGrid()),			ObjectModelEntryFlags::none },
	{ "skew",					OBJECT_MODEL_FUNC(self, 8),																		ObjectModelEntryFlags::none },
	{ "type",					OBJECT_MODEL_FUNC(self->GetCompensationTypeString()),											ObjectModelEntryFlags::none },

	// 7. move.compensation.meshDeviation members
	{ "deviation",				OBJECT_MODEL_FUNC(self->latestMeshDeviation.GetDeviationFromMean(), 3),							ObjectModelEntryFlags::none },
	{ "mean",					OBJECT_MODEL_FUNC(self->latestMeshDeviation.GetMean(), 3),										ObjectModelEntryFlags::none },

	// 8. move.compensation.skew members
	{ "compensateXY",			OBJECT_MODEL_FUNC(self->compensateXY),															ObjectModelEntryFlags::none },
	{ "tanXY",					OBJECT_MODEL_FUNC(self->tanXY(), 4),															ObjectModelEntryFlags::none },
	{ "tanXZ",					OBJECT_MODEL_FUNC(self->tanXZ(), 4),															ObjectModelEntryFlags::none },
	{ "tanYZ",					OBJECT_MODEL_FUNC(self->tanYZ(), 4),															ObjectModelEntryFlags::none },

#if SUPPORT_COORDINATE_ROTATION
	// 9. move.rotation members
	{ "angle",					OBJECT_MODEL_FUNC_NOSELF(reprap.GetGCodes().GetRotationAngle()),								ObjectModelEntryFlags::none },
	{ "centre",					OBJECT_MODEL_FUNC_ARRAY(3),																		ObjectModelEntryFlags::none },
#endif
};

constexpr uint8_t Move::objectModelTableDescriptor[] =
{
	9 + SUPPORT_COORDINATE_ROTATION,
	17 + SUPPORT_COORDINATE_ROTATION + SUPPORT_KEEPOUT_ZONES,
	2,
	5 + SUPPORT_LASER,
	3,
	2,
	2,
	6 + (HAS_MASS_STORAGE || HAS_SBC_INTERFACE),
	2,
	4,
#if SUPPORT_COORDINATE_ROTATION
	2
#endif
};

DEFINE_GET_OBJECT_MODEL_TABLE(Move)

// The Move task starts executing here
[[noreturn]] static void MoveStart(void *param) noexcept
{
	static_cast<Move *>(param)->MoveLoop();
}

Move::Move() noexcept
	:
#if SUPPORT_ASYNC_MOVES
	  heightController(nullptr),
#endif
	  jerkPolicy(0),
	  numCalibratedFactors(0)
{
	// Kinematics must be set up here because GCodes::Init asks the kinematics for the assumed initial position
	kinematics = Kinematics::Create(KinematicsType::cartesian);		// default to Cartesian
	rings[0].Init1(InitialDdaRingLength);
#if SUPPORT_ASYNC_MOVES
	rings[1].Init1(AuxDdaRingLength);
#endif
}

void Move::Init() noexcept
{
	rings[0].Init2();

#if SUPPORT_ASYNC_MOVES
	rings[1].Init2();
	auxMoveAvailable = false;
	auxMoveLocked = false;
#endif

	// Clear the transforms
	SetIdentityTransform();
	compensateXY = true;
	tangents[0] = tangents[1] = tangents[2] = 0.0;

	usingMesh = useTaper = false;
	zShift = 0.0;

	idleTimeout = DefaultIdleTimeout;
	moveState = MoveState::idle;
	whenLastMoveAdded = whenIdleTimerStarted = millis();

	simulationMode = SimulationMode::off;
	longestGcodeWaitInterval = 0;
	stepErrors = numHiccups = 0;
	lastReportedMovementDelay = 0;
	bedLevellingMoveAvailable = false;
	activeDMs = nullptr;
	for (volatile int32_t& acc : movementAccumulators)
	{
		acc = 0;
	}
	for (int32_t& pos : motorPositionsAfterScheduledMoves)
	{
		pos = 0;
	}
	for (uint16_t& ms : microstepping)
	{
		ms = 16 | 0x8000;
	}
	for (size_t drv = 0; drv < MaxAxesPlusExtruders + NumDirectDrivers; ++drv)
	{
		dms[drv].Init(drv);
		if (drv < MaxAxesPlusExtruders)
		{
			const float stepsPerMm = (drv >= MaxAxes) ? DefaultEDriveStepsPerUnit
										: (drv == Z_AXIS) ? DefaultZDriveStepsPerUnit
											: DefaultAxisDriveStepsPerUnit;
			SetDriveStepsPerMm(drv, stepsPerMm, 0);				//TODO what about steps/mm for the direct driver numbers?
		}
	}

	moveTask.Create(MoveStart, "Move", this, TaskPriority::MovePriority);
}

void Move::Exit() noexcept
{
	StepTimer::DisableTimerInterrupt();
	timer.CancelCallback();
	rings[0].Exit();
#if SUPPORT_ASYNC_MOVES
	rings[1].Exit();
#endif
#if SUPPORT_LASER || SUPPORT_IOBITS
	delete laserTask;
	laserTask = nullptr;
#endif
	moveTask.TerminateAndUnlink();
}

// Set the microstepping for local drivers, returning true if successful. All drivers for the same axis must use the same microstepping.
// Caller must deal with remote drivers.
bool Move::SetMicrostepping(size_t axisOrExtruder, int microsteps, bool interp, const StringRef& reply) noexcept
{
	//TODO check that it is a valid microstep setting
	microstepping[axisOrExtruder] = (interp) ? microsteps | 0x8000 : microsteps;
	reprap.MoveUpdated();
	return reprap.GetPlatform().SetDriversMicrostepping(axisOrExtruder, microsteps, interp, reply);
}

// Get the microstepping for an axis or extruder
unsigned int Move::GetMicrostepping(size_t axisOrExtruder, bool& interpolation) const noexcept
{
	interpolation = (microstepping[axisOrExtruder] & 0x8000) != 0;
	return microstepping[axisOrExtruder] & 0x7FFF;
}

// Set the drive steps per mm. Called when processing M92.
void Move::SetDriveStepsPerMm(size_t axisOrExtruder, float value, uint32_t requestedMicrostepping) noexcept
{
	if (requestedMicrostepping != 0)
	{
		const uint32_t currentMicrostepping = microstepping[axisOrExtruder] & 0x7FFF;
		if (currentMicrostepping != requestedMicrostepping)
		{
			value = value * (float)currentMicrostepping / (float)requestedMicrostepping;
		}
	}

	value = max<float>(value, 1.0);							// don't allow zero or negative
	driveStepsPerMm[axisOrExtruder] = value;
	reprap.MoveUpdated();
}

[[noreturn]] void Move::MoveLoop() noexcept
{

	timer.SetCallback(Move::TimerCallback, CallbackParameter(this));
	for (;;)
	{
		if (reprap.IsStopped())
		{
			// Emergency stop has been commanded, so terminate this task to prevent new moves being prepared and executed
			moveTask.TerminateAndUnlink();
		}

		bool moveRead = false;

		// See if we can add another move to ring 0
		const bool canAddRing0Move = rings[0].CanAddMove();
		if (canAddRing0Move)
		{
			// OK to add another move. First check if a special move is available.
			if (bedLevellingMoveAvailable)
			{
				moveRead = true;
				if (simulationMode < SimulationMode::partial)
				{
					if (rings[0].AddSpecialMove(reprap.GetPlatform().MaxFeedrate(Z_AXIS), specialMoveCoords))
					{
						const uint32_t now = millis();
						const uint32_t timeWaiting = now - whenLastMoveAdded;
						if (timeWaiting > longestGcodeWaitInterval)
						{
							longestGcodeWaitInterval = timeWaiting;
						}
						whenLastMoveAdded = now;
						moveState = MoveState::collecting;
					}
				}
				bedLevellingMoveAvailable = false;
			}
			else
			{
				// If there's a G Code move available, add it to the DDA ring for processing.
				RawMove nextMove;
				if (reprap.GetGCodes().ReadMove(0, nextMove))				// if we have a new move
				{
					moveRead = true;
					if (simulationMode < SimulationMode::partial)			// in simulation mode partial, we don't process incoming moves beyond this point
					{
						if (nextMove.moveType == 0)
						{
							AxisAndBedTransform(nextMove.coords, nextMove.movementTool,
#if SUPPORT_SCANNING_PROBES
													!nextMove.scanningProbeMove
#else
													true
#endif
													);
						}

						if (rings[0].AddStandardMove(nextMove, !IsRawMotorMove(nextMove.moveType)))
						{
							const uint32_t now = millis();
							const uint32_t timeWaiting = now - whenLastMoveAdded;
							if (timeWaiting > longestGcodeWaitInterval)
							{
								longestGcodeWaitInterval = timeWaiting;
							}
							whenLastMoveAdded = now;
							moveState = MoveState::collecting;
						}
					}
				}
			}
		}

		// Let ring 0 process moves. Better to have a few moves in the queue so that we can do lookahead, hence the test on idleCount and idleTime.
		uint32_t nextPrepareDelay = rings[0].Spin(simulationMode, !canAddRing0Move, millis() - whenLastMoveAdded >= rings[0].GetGracePeriod());

#if SUPPORT_ASYNC_MOVES
		const bool canAddRing1Move = rings[1].CanAddMove();
		if (canAddRing1Move)
		{
			if (auxMoveAvailable)
			{
				moveRead = true;
				if (rings[1].AddAsyncMove(auxMove))
				{
					const uint32_t now = millis();
					const uint32_t timeWaiting = now - whenLastMoveAdded;
					if (timeWaiting > longestGcodeWaitInterval)
					{
						longestGcodeWaitInterval = timeWaiting;
					}
					whenLastMoveAdded = now;
					moveState = MoveState::collecting;
				}
				auxMoveAvailable = false;
			}
			else
			{
				// If there's a G Code move available, add it to the DDA ring for processing.
				RawMove nextMove;
				if (reprap.GetGCodes().ReadMove(1, nextMove))				// if we have a new move
				{
					moveRead = true;
					if (simulationMode < SimulationMode::partial)			// in simulation mode partial, we don't process incoming moves beyond this point
					{
						if (nextMove.moveType == 0)
						{
							AxisAndBedTransform(nextMove.coords, nextMove.movementTool, true);
						}

						if (rings[1].AddStandardMove(nextMove, !IsRawMotorMove(nextMove.moveType)))
						{
							const uint32_t now = millis();
							const uint32_t timeWaiting = now - whenLastMoveAdded;
							if (timeWaiting > longestGcodeWaitInterval)
							{
								longestGcodeWaitInterval = timeWaiting;
							}
							whenLastMoveAdded = now;
							moveState = MoveState::collecting;
						}
					}
				}
			}
		}

		const uint32_t auxPrepareDelay = rings[1].Spin(simulationMode, !canAddRing1Move,  millis() - whenLastMoveAdded >= rings[1].GetGracePeriod());
		if (auxPrepareDelay < nextPrepareDelay)
		{
			nextPrepareDelay = auxPrepareDelay;
		}
#endif

		if (simulationMode == SimulationMode::debug && reprap.GetDebugFlags(Module::Move).IsBitSet(MoveDebugFlags::SimulateSteppingDrivers))
		{
			while (activeDMs != nullptr)
			{
				SimulateSteppingDrivers(reprap.GetPlatform());
			}
		}

		// Reduce motor current to standby if the rings have been idle for long enough
		if (   rings[0].IsIdle()
#if SUPPORT_ASYNC_MOVES
			&& rings[1].IsIdle()
#endif
		   )
		{
			if (   moveState == MoveState::executing
				&& reprap.GetGCodes().GetPauseState() == PauseState::notPaused	// for now we don't go into idle hold when we are paused (is this sensible?)
			   )
			{
				whenIdleTimerStarted = millis();				// record when we first noticed that the machine was idle
				moveState = MoveState::timing;
			}
			else if (moveState == MoveState::timing && millis() - whenIdleTimerStarted >= idleTimeout)
			{
				reprap.GetPlatform().SetDriversIdle();			// put all drives in idle hold
				moveState = MoveState::idle;
			}
		}
		else
		{
			moveState = MoveState::executing;
		}

		// We need to be woken when one of the following is true:
		// 1. If moves are being executed and there are unprepared moves in the queue, when it is time to prepare more moves.
		// 2. If the queue was full and all moves in it were prepared, when we have completed one or more moves.
		// 3. In order to implement idle timeout, we must wake up regularly anyway, say every half second
		if (!moveRead && nextPrepareDelay != 0)
		{
			TaskBase::TakeIndexed(NotifyIndices::Move, min<uint32_t>(nextPrepareDelay, 500));
		}
	}
}

// This is called from GCodes to tell the Move task that a move is available
void Move::MoveAvailable() noexcept
{
	if (moveTask.IsRunning())
	{
		moveTask.Give(NotifyIndices::Move);
	}
}

// Tell the lookahead ring we are waiting for it to empty and return true if it is
bool Move::WaitingForAllMovesFinished(MovementSystemNumber msNumber) noexcept
{
	return rings[msNumber].SetWaitingToEmpty();
}

// Return the number of actually probed probe points
unsigned int Move::GetNumProbedProbePoints() const noexcept
{
	return probePoints.NumberOfProbePoints();
}

// Try to push some babystepping through the lookahead queue, returning the amount pushed
// This is called by the Main task, so we need to lock out the Move task while doing this
float Move::PushBabyStepping(MovementSystemNumber msNumber,size_t axis, float amount) noexcept
{
	TaskCriticalSectionLocker lock;						// lock out the Move task

	return rings[msNumber].PushBabyStepping(axis, amount);
}

// Change the kinematics to the specified type if it isn't already
// If it is already correct leave its parameters alone.
// This violates our rule on no dynamic memory allocation after the initialisation phase,
// however this function is normally called only when M665, M667 and M669 commands in config.g are processed.
bool Move::SetKinematics(KinematicsType k) noexcept
{
	if (kinematics->GetKinematicsType() != k)
	{
		Kinematics * const nk = Kinematics::Create(k);
		if (nk == nullptr)
		{
			return false;
		}
		delete kinematics;
		kinematics = nk;
		reprap.MoveUpdated();
	}
	return true;
}

// Return true if this is a raw motor move
bool Move::IsRawMotorMove(uint8_t moveType) const noexcept
{
	return moveType == 2 || ((moveType == 1 || moveType == 3) && kinematics->GetHomingMode() != HomingMode::homeCartesianAxes);
}

// Return true if the specified point is accessible to the Z probe
bool Move::IsAccessibleProbePoint(float axesCoords[MaxAxes], AxesBitmap axes) const noexcept
{
	return kinematics->IsReachable(axesCoords, axes);
}

// Pause the print as soon as we can, returning true if we are able to skip any moves and updating ms.pauseRestorePoint to the first move we skipped.
bool Move::PausePrint(MovementState& ms) noexcept
{
	return rings[ms.GetMsNumber()].PauseMoves(ms);
}

#if HAS_VOLTAGE_MONITOR || HAS_STALL_DETECT

// Pause the print immediately, returning true if we were able to skip or abort any moves and setting up to the move we aborted
bool Move::LowPowerOrStallPause(unsigned int queueNumber, RestorePoint& rp) noexcept
{
	return rings[queueNumber].LowPowerOrStallPause(rp);
}

// Stop generating steps
void Move::CancelStepping() noexcept
{
	StepTimer::DisableTimerInterrupt();
}

#endif

void Move::Diagnostics(MessageType mtype) noexcept
{
	// Get the type of bed compensation in use
#if 0	// debug only
	String<StringLength256> scratchString;
#else
	String<StringLength100> scratchString;
#endif
	scratchString.copy(GetCompensationTypeString());

	const uint32_t currentMovementDelay = StepTimer::GetMovementDelay();
	const float delayToReport = (currentMovementDelay - lastReportedMovementDelay) * (1000.0/(float)StepTimer::GetTickRate());
	lastReportedMovementDelay = currentMovementDelay;

	Platform& p = reprap.GetPlatform();
	p.MessageF(mtype,
				"=== Move ===\nSegments created %u, maxWait %" PRIu32 "ms, bed compensation in use: %s, height map offset %.3f, hiccups %u, hiccup time %.2fms, stepErrors %u, max steps late %" PRIi32
#if 1	//debug
				", ebfmin %.2f, ebfmax %.2f"
#endif
				"\n",
						MoveSegment::NumCreated(), longestGcodeWaitInterval, scratchString.c_str(), (double)zShift, numHiccups, (double)delayToReport, stepErrors, DriveMovement::GetAndClearMaxStepsLate()
#if 1
						, (double)minExtrusionPending, (double)maxExtrusionPending
#endif
		);
	longestGcodeWaitInterval = 0;
	stepErrors = numHiccups = 0;
#if 1	//debug
	minExtrusionPending = maxExtrusionPending = 0.0;
#endif

#if DDA_DEBUG_STEP_COUNT
	scratchString.copy("Steps requested/done:");
	for (size_t driver = 0; driver < NumDirectDrivers; ++driver)
	{
		scratchString.catf(" %" PRIu32 "/%" PRIu32, stepsRequested[driver], stepsDone[driver]);
		stepsRequested[driver] = stepsDone[driver] = 0;
	}
	scratchString.cat('\n');
	p.Message(mtype, scratchString.c_str());
#endif

#if DDA_LOG_PROBE_CHANGES
	// Temporary code to print Z probe trigger positions
	p.Message(mtype, "Probe change coordinates:");
	char ch = ' ';
	for (size_t i = 0; i < numLoggedProbePositions; ++i)
	{
		float xyzPos[XYZ_AXES];
		MotorStepsToCartesian(loggedProbePositions + (XYZ_AXES * i), XYZ_AXES, XYZ_AXES, xyzPos);
		p.MessageF(mtype, "%c%.2f,%.2f", ch, xyzPos[X_AXIS], xyzPos[Y_AXIS]);
		ch = ',';
	}
	p.Message(mtype, "\n");
#endif

	// DEBUG
#if 0
	extern uint32_t maxDelay;
	extern uint32_t maxDelayIncrease;
	p.MessageF(mtype, "Max delay %" PRIu32 ", increase %" PRIu32 "\n", maxDelay, maxDelayIncrease);
	maxDelay = maxDelayIncrease = 0;
#endif

	scratchString.Clear();
	StepTimer::Diagnostics(scratchString.GetRef());
	p.MessageF(mtype, "%s\n", scratchString.c_str());
	axisShaper.Diagnostics(mtype);

	for (size_t i = 0; i < ARRAY_SIZE(rings); ++i)
	{
		rings[i].Diagnostics(mtype, i);
	}
}

// Clear the movement pending value for an extruder
void Move::ClearExtruderMovementPending(size_t extruder) noexcept
{
	dms[ExtruderToLogicalDrive(extruder)].ClearMovementPending();
}

// Return when we started doing normal moves after the most recent extruder-only move, in millisecond ticks
uint32_t Move::ExtruderPrintingSince(size_t logicalDrive) const noexcept
{
	return dms[logicalDrive].extruderPrintingSince;
}

// Set the current position to be this
void Move::SetNewPosition(const float positionNow[MaxAxesPlusExtruders], MovementSystemNumber msNumber, bool doBedCompensation) noexcept
{
	float newPos[MaxAxesPlusExtruders];
	memcpyf(newPos, positionNow, ARRAY_SIZE(newPos));			// copy to local storage because Transform modifies it
	AxisAndBedTransform(newPos, reprap.GetGCodes().GetMovementState(msNumber).currentTool, doBedCompensation);
	SetRawPosition(newPos, msNumber);
}

// Convert distance to steps for a particular drive
int32_t Move::MotorMovementToSteps(size_t drive, float coord) const noexcept
{
	return lrintf(coord * driveStepsPerMm[drive]);
}

// Convert motor coordinates to machine coordinates. Used after homing and after individual motor moves.
// This is computationally expensive on a delta or SCARA machine, so only call it when necessary, and never from the step ISR.
void Move::MotorStepsToCartesian(const int32_t motorPos[], size_t numVisibleAxes, size_t numTotalAxes, float machinePos[]) const noexcept
{
	kinematics->MotorStepsToCartesian(motorPos, driveStepsPerMm, numVisibleAxes, numTotalAxes, machinePos);
	if (reprap.GetDebugFlags(Module::Move).IsBitSet(MoveDebugFlags::PrintTransforms))
	{
		debugPrintf("Forward transformed %" PRIi32 " %" PRIi32 " %" PRIi32 " to %.2f %.2f %.2f\n",
			motorPos[0], motorPos[1], motorPos[2], (double)machinePos[0], (double)machinePos[1], (double)machinePos[2]);
	}
}

// Convert Cartesian coordinates to motor steps, axes only, returning true if successful.
// Used to perform movement and G92 commands.
// This may be called from an ISR, e.g. via Kinematics::OnHomingSwitchTriggered, DDA::SetPositions and Move::EndPointToMachine
// If isCoordinated is false then multi-mode kinematics such as SCARA are allowed to switch mode if necessary to make the specified machine position reachable
bool Move::CartesianToMotorSteps(const float machinePos[MaxAxes], int32_t motorPos[MaxAxes], bool isCoordinated) const noexcept
{
	const bool b = kinematics->CartesianToMotorSteps(machinePos, driveStepsPerMm,
														reprap.GetGCodes().GetVisibleAxes(), reprap.GetGCodes().GetTotalAxes(), motorPos, isCoordinated);
	if (reprap.GetDebugFlags(Module::Move).IsBitSet(MoveDebugFlags::PrintTransforms))
	{
		if (!b)
		{
			debugPrintf("Unable to transform");
			for (size_t i = 0; i < reprap.GetGCodes().GetVisibleAxes(); ++i)
			{
				debugPrintf(" %.2f", (double)machinePos[i]);
			}
			debugPrintf("\n");
		}
		else
		{
			debugPrintf("Transformed");
			for (size_t i = 0; i < reprap.GetGCodes().GetVisibleAxes(); ++i)
			{
				debugPrintf(" %.2f", (double)machinePos[i]);
			}
			debugPrintf(" to");
			for (size_t i = 0; i < reprap.GetGCodes().GetTotalAxes(); ++i)
			{
				debugPrintf(" %" PRIi32, motorPos[i]);
			}
			debugPrintf("\n");
		}
	}
	return b;
}

void Move::AxisAndBedTransform(float xyzPoint[MaxAxes], const Tool *tool, bool useBedCompensation) const noexcept
{
	AxisTransform(xyzPoint, tool);
	if (useBedCompensation)
	{
		BedTransform(xyzPoint, tool);
	}
}

void Move::InverseAxisAndBedTransform(float xyzPoint[MaxAxes], const Tool *tool) const noexcept
{
	InverseBedTransform(xyzPoint, tool);
	InverseAxisTransform(xyzPoint, tool);
}

// Do the Axis transform BEFORE the bed transform
void Move::AxisTransform(float xyzPoint[MaxAxes], const Tool *tool) const noexcept
{
	// Identify the lowest Y axis
	const size_t numVisibleAxes = reprap.GetGCodes().GetVisibleAxes();
	const AxesBitmap yAxes = Tool::GetYAxes(tool);
	const size_t lowestYAxis = yAxes.LowestSetBit();
	if (lowestYAxis < numVisibleAxes)
	{
		// Found a Y axis. Use this one when correcting the X coordinate.
		const AxesBitmap xAxes = Tool::GetXAxes(tool);
		const size_t lowestXAxis = xAxes.LowestSetBit();
		for (size_t axis = 0; axis < numVisibleAxes; ++axis)
		{
			if (xAxes.IsBitSet(axis))
			{
				xyzPoint[axis] += (compensateXY ? tanXY() * xyzPoint[lowestYAxis] : 0.0) + tanXZ() * xyzPoint[Z_AXIS];
			}
			if (yAxes.IsBitSet(axis))
			{
				xyzPoint[axis] += (compensateXY ? 0.0 : tanXY() * xyzPoint[lowestXAxis]) + tanYZ() * xyzPoint[Z_AXIS];
			}
		}
	}
}

// Invert the Axis transform AFTER the bed transform
void Move::InverseAxisTransform(float xyzPoint[MaxAxes], const Tool *tool) const noexcept
{
	// Identify the lowest Y axis
	const size_t numVisibleAxes = reprap.GetGCodes().GetVisibleAxes();
	const AxesBitmap yAxes = Tool::GetYAxes(tool);
	const size_t lowestYAxis = yAxes.LowestSetBit();
	if (lowestYAxis < numVisibleAxes)
	{
		// Found a Y axis. Use this one when correcting the X coordinate.
		const AxesBitmap xAxes = Tool::GetXAxes(tool);
		const size_t lowestXAxis = xAxes.LowestSetBit();
		for (size_t axis = 0; axis < numVisibleAxes; ++axis)
		{
			if (yAxes.IsBitSet(axis))
			{
				xyzPoint[axis] -= ((compensateXY ? 0.0 : tanXY() * xyzPoint[lowestXAxis]) + tanYZ() * xyzPoint[Z_AXIS]);
			}
			if (xAxes.IsBitSet(axis))
			{
				xyzPoint[axis] -= ((compensateXY ? tanXY() * xyzPoint[lowestYAxis] : 0.0) + tanXZ() * xyzPoint[Z_AXIS]);
			}
		}
	}
}

// Compute the height correction needed at a point, ignoring taper
float Move::ComputeHeightCorrection(float xyzPoint[MaxAxes], const Tool *tool) const noexcept
{
	float zCorrection = 0.0;
	unsigned int numCorrections = 0;
	const GridDefinition& grid = GetGrid();
	const AxesBitmap axis1Axes = Tool::GetAxisMapping(tool, grid.GetAxisNumber(1));

	// Transform the Z coordinate based on the average correction for each axis used as an X or Y axis.
	Tool::GetAxisMapping(tool, grid.GetAxisNumber(0))
		.Iterate([this, xyzPoint, tool, axis1Axes, &zCorrection, &numCorrections](unsigned int axis0Axis, unsigned int)
					{
						const float axis0Coord = xyzPoint[axis0Axis] + Tool::GetOffset(tool, axis0Axis);
						axis1Axes.Iterate([this, xyzPoint, tool, axis0Coord, &zCorrection, &numCorrections](unsigned int axis1Axis, unsigned int)
											{
												const float axis1Coord = xyzPoint[axis1Axis] + Tool::GetOffset(tool, axis1Axis);
												zCorrection += heightMap.GetInterpolatedHeightError(axis0Coord, axis1Coord);
												++numCorrections;
											}
										);
					}
				);

	if (numCorrections > 1)
	{
		zCorrection /= numCorrections;			// take an average
	}

	return zCorrection + zShift;
}

// Do the bed transform AFTER the axis transform
void Move::BedTransform(float xyzPoint[MaxAxes], const Tool *tool) const noexcept
{
	if (usingMesh)
	{
		const float toolHeight = xyzPoint[Z_AXIS] + Tool::GetOffset(tool, Z_AXIS);			// the requested nozzle height above the bed
		if (!useTaper || toolHeight < taperHeight)
		{
			const float zCorrection = ComputeHeightCorrection(xyzPoint, tool);
			xyzPoint[Z_AXIS] += (useTaper && zCorrection < taperHeight) ? (taperHeight - toolHeight) * recipTaperHeight * zCorrection : zCorrection;
		}
	}
}

// Invert the bed transform BEFORE the axis transform
void Move::InverseBedTransform(float xyzPoint[MaxAxes], const Tool *tool) const noexcept
{
	if (usingMesh)
	{
		const float zCorrection = ComputeHeightCorrection(xyzPoint, tool);
		if (!useTaper || zCorrection >= taperHeight)	// need check on zCorrection to avoid possible divide by zero
		{
			xyzPoint[Z_AXIS] -= zCorrection;
		}
		else
		{
			const float toolZoffset = Tool::GetOffset(tool, Z_AXIS);
			const float zreq = (xyzPoint[Z_AXIS] - (taperHeight - toolZoffset) * zCorrection * recipTaperHeight)/(1.0 - zCorrection * recipTaperHeight);
			if (zreq + toolZoffset < taperHeight)
			{
				xyzPoint[Z_AXIS] = zreq;
			}
		}
	}
}

// Normalise the bed transform to have zero height error at these bed coordinates
void Move::SetZeroHeightError(const float coords[MaxAxes]) noexcept
{
	if (usingMesh)
	{
		float tempCoords[MaxAxes];
		memcpyf(tempCoords, coords, ARRAY_SIZE(tempCoords));
		AxisTransform(tempCoords, nullptr);
		const GridDefinition& grid = GetGrid();
		zShift = -heightMap.GetInterpolatedHeightError(tempCoords[grid.GetAxisNumber(0)], tempCoords[grid.GetAxisNumber(1)]);
	}
	else
	{
		zShift = 0.0;
	}
}

void Move::SetIdentityTransform() noexcept
{
	probePoints.SetIdentity();
	heightMap.ClearGridHeights();
	heightMap.UseHeightMap(false);
	usingMesh = false;
	zShift = 0.0;
	reprap.MoveUpdated();
}

#if HAS_MASS_STORAGE || HAS_SBC_INTERFACE

// Load the height map from file, returning true if an error occurred with the error reason appended to the buffer
bool Move::LoadHeightMapFromFile(FileStore *f, const char *fname, const StringRef& r) noexcept
{
	const bool err = heightMap.LoadFromFile(f, fname, r
#if SUPPORT_PROBE_POINTS_FILE
											, false				// loading the height map, not the probe points file
#endif
											);
	if (err)
	{
		heightMap.ClearGridHeights();							// make sure we don't end up with a partial height map
	}
	else
	{
		zShift = 0.0;
	}
	float minError, maxError;
	(void)heightMap.GetStatistics(latestMeshDeviation, minError, maxError);
	reprap.MoveUpdated();
	return err;
}

// Save the height map to a file returning true if an error occurred
bool Move::SaveHeightMapToFile(FileStore *f, const char *fname) noexcept
{
	return heightMap.SaveToFile(f, fname, zShift);
}

# if SUPPORT_PROBE_POINTS_FILE

// Load the probe points map from a file returning true if an error occurred
bool Move::LoadProbePointsFromFile(FileStore *f, const char *fname, const StringRef& r) noexcept
{
	return heightMap.LoadFromFile(f, fname, r, true);
}

void Move::ClearProbePointsInvalid() noexcept
{
	heightMap.ClearProbePointsInvalid();
}

# endif

#endif	// HAS_MASS_STORAGE || HAS_SBC_INTERFACE

void Move::SetTaperHeight(float h) noexcept
{
	useTaper = (h > 1.0);
	if (useTaper)
	{
		taperHeight = h;
		recipTaperHeight = 1.0/h;
	}
	reprap.MoveUpdated();
}

// Enable mesh bed compensation
bool Move::UseMesh(bool b) noexcept
{
	usingMesh = heightMap.UseHeightMap(b);
	reprap.MoveUpdated();
	return usingMesh;
}

float Move::AxisCompensation(unsigned int axis) const noexcept
{
	return (axis < ARRAY_SIZE(tangents)) ? tangents[axis] : 0.0;
}

void Move::SetAxisCompensation(unsigned int axis, float tangent) noexcept
{
	if (axis < ARRAY_SIZE(tangents))
	{
		tangents[axis] = tangent;
		reprap.MoveUpdated();
	}
}

bool Move::IsXYCompensated() const
{
	return compensateXY;
}

void Move::SetXYCompensation(bool xyCompensation)
{
	compensateXY = xyCompensation;
	reprap.MoveUpdated();
}

// Calibrate or set the bed equation after probing, returning true if an error occurred
// sParam is the value of the S parameter in the G30 command that provoked this call.
// Caller already owns the GCode movement lock.
bool Move::FinishedBedProbing(int sParam, const StringRef& reply) noexcept
{
	bool error = false;
	const size_t numPoints = probePoints.NumberOfProbePoints();

	if (sParam < 0)
	{
		// A negative sParam just prints the probe heights
		probePoints.ReportProbeHeights(numPoints, reply);
	}
	else if (numPoints < (size_t)sParam)
	{
		reply.printf("Bed calibration : %d factor calibration requested but only %d points provided\n", sParam, numPoints);
		error = true;
	}
	else
	{
		if (reprap.GetDebugFlags(Module::Move).IsBitSet(MoveDebugFlags::ZProbing))
		{
			probePoints.DebugPrint(numPoints);
		}

		if (sParam == 0)
		{
			sParam = numPoints;
		}

		if (!probePoints.GoodProbePoints(numPoints))
		{
			reply.copy("Compensation or calibration cancelled due to probing errors");
			error = true;
		}
		else if (kinematics->SupportsAutoCalibration())
		{
			error = kinematics->DoAutoCalibration(sParam, probePoints, reply);
		}
		else
		{
			reply.copy("This kinematics does not support auto-calibration");
			error = true;
		}
	}

	// Clear out the Z heights so that we don't re-use old points.
	// This allows us to use different numbers of probe point on different occasions.
	probePoints.ClearProbeHeights();
	return error;
}

float Move::MotorStepsToMovement(size_t drive, int32_t endpoint) const noexcept
{
	return ((float)(endpoint))/driveStepsPerMm[drive];
}

// Return the transformed machine coordinates
void Move::GetCurrentUserPosition(float m[MaxAxes], MovementSystemNumber msNumber, uint8_t moveType, const Tool *tool) const noexcept
{
	GetCurrentMachinePosition(m, msNumber, IsRawMotorMove(moveType));
	if (moveType == 0)
	{
		InverseAxisAndBedTransform(m, tool);
	}
}

void Move::SetXYBedProbePoint(size_t index, float x, float y) noexcept
{
	if (index >= MaxProbePoints)
	{
		reprap.GetPlatform().Message(ErrorMessage, "Z probe point index out of range\n");
	}
	else
	{
		probePoints.SetXYBedProbePoint(index, x, y);
	}
}

void Move::SetZBedProbePoint(size_t index, float z, bool wasXyCorrected, bool wasError) noexcept
{
	if (index >= MaxProbePoints)
	{
		reprap.GetPlatform().Message(ErrorMessage, "Z probe point index out of range\n");
	}
	else
	{
		probePoints.SetZBedProbePoint(index, z, wasXyCorrected, wasError);
	}
}

// This returns the (X, Y) points to probe the bed at probe point count.  When probing, it returns false.
// If called after probing has ended it returns true, and the Z coordinate probed is also returned.
// If 'wantNozzlePosition is true then we return the nozzle position when the point is probed, else we return the probe point itself
float Move::GetProbeCoordinates(int count, float& x, float& y, bool wantNozzlePosition) const noexcept
{
	x = probePoints.GetXCoord(count);
	y = probePoints.GetYCoord(count);
	if (wantNozzlePosition)
	{
		const auto zp = reprap.GetPlatform().GetEndstops().GetZProbe(reprap.GetGCodes().GetCurrentZProbeNumber());
		if (zp.IsNotNull())
		{
			x -= zp->GetOffset(X_AXIS);
			y -= zp->GetOffset(Y_AXIS);
		}
	}
	return probePoints.GetZHeight(count);
}

// Enter or leave simulation mode
void Move::Simulate(SimulationMode simMode) noexcept
{
	simulationMode = simMode;
	if (simMode != SimulationMode::off)
	{
		rings[0].ResetSimulationTime();
	}
}

// Adjust the leadscrews
// This is only ever called after bed probing, so we can assume that no such move is already pending.
void Move::AdjustLeadscrews(const floatc_t corrections[]) noexcept
{
	const size_t numZdrivers = reprap.GetPlatform().GetAxisDriversConfig(Z_AXIS).numDrivers;
	for (size_t i = 0; i < MaxDriversPerAxis; ++i)
	{
		specialMoveCoords[i] = (i < numZdrivers) ? (float)corrections[i] : 0.0;
	}
	bedLevellingMoveAvailable = true;
	MoveAvailable();
}

// Return the idle timeout in seconds
float Move::IdleTimeout() const noexcept
{
	return (float)idleTimeout * 0.001;
}

// Set the idle timeout in seconds
void Move::SetIdleTimeout(float timeout) noexcept
{
	idleTimeout = (uint32_t)lrintf(timeout * 1000.0);
	reprap.MoveUpdated();
}

#if HAS_MASS_STORAGE || HAS_SBC_INTERFACE

// Write settings for resuming the print
// The GCodes module deals with the head position so all we need worry about is the bed compensation
// We don't handle random probe point bed compensation, and we assume that if a height map is being used it is the default one.
bool Move::WriteResumeSettings(FileStore *f) const noexcept
{
	return kinematics->WriteResumeSettings(f) && (!usingMesh || f->Write("G29 S1\n"));
}

#endif

// Process M595
GCodeResult Move::ConfigureMovementQueue(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException)
{
	const size_t ringNumber = (gb.Seen('Q')) ? gb.GetLimitedUIValue('Q', ARRAY_SIZE(rings)) : 0;
	return rings[ringNumber].ConfigureMovementQueue(gb, reply);
}

// Process M572
GCodeResult Move::ConfigurePressureAdvance(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException)
{
	if (gb.Seen('S'))
	{
		const float advance = gb.GetNonNegativeFValue();
		if (!reprap.GetGCodes().LockCurrentMovementSystemAndWaitForStandstill(gb))
		{
			return GCodeResult::notFinished;
		}

		GCodeResult rslt = GCodeResult::ok;

#if SUPPORT_CAN_EXPANSION
		CanDriversData<float> canDriversToUpdate;
#endif
		if (gb.Seen('D'))
		{
			uint32_t eDrive[MaxExtruders];
			size_t eCount = MaxExtruders;
			gb.GetUnsignedArray(eDrive, eCount, false);
#if SUPPORT_CAN_EXPANSION
			Platform& platform = reprap.GetPlatform();
#endif
			for (size_t i = 0; i < eCount; i++)
			{
				const uint32_t extruder = eDrive[i];
				if (extruder >= reprap.GetGCodes().GetNumExtruders())
				{
					reply.printf("Invalid extruder number '%" PRIu32 "'", extruder);
					rslt = GCodeResult::error;
					break;
				}
				GetExtruderShaperForExtruder(extruder).SetKseconds(advance);
#if SUPPORT_CAN_EXPANSION
				const DriverId did = platform.GetExtruderDriver(extruder);
				if (did.IsRemote())
				{
					canDriversToUpdate.AddEntry(did, advance);
				}
#endif
			}
		}
		else
		{
			const Tool * const ct = reprap.GetGCodes().GetConstMovementState(gb).currentTool;
			if (ct == nullptr)
			{
				reply.copy("No tool selected");
				rslt = GCodeResult::error;
			}
			else
			{
#if SUPPORT_CAN_EXPANSION
				ct->IterateExtruders([this, advance, &canDriversToUpdate](unsigned int extruder)
										{
											GetExtruderShaperForExtruder(extruder).SetKseconds(advance);
											const DriverId did = reprap.GetPlatform().GetExtruderDriver(extruder);
											if (did.IsRemote())
											{
												canDriversToUpdate.AddEntry(did, advance);
											}
										}
									);
#else
				ct->IterateExtruders([this, advance](unsigned int extruder)
										{
											GetExtruderShaperForExtruder(extruder).SetKseconds(advance);
										}
									);
#endif
			}
		}

		reprap.MoveUpdated();

#if SUPPORT_CAN_EXPANSION
		return max(rslt, CanInterface::SetRemotePressureAdvance(canDriversToUpdate, reply));
#else
		return rslt;
#endif
	}

	reply.copy("Extruder pressure advance");
	char c = ':';
	for (size_t i = 0; i < reprap.GetGCodes().GetNumExtruders(); ++i)
	{
		reply.catf("%c %.3f", c, (double)GetExtruderShaperForExtruder(i).GetKseconds());
		c = ',';
	}
	return GCodeResult::ok;
}

#if SUPPORT_REMOTE_COMMANDS

GCodeResult Move::EutSetRemotePressureAdvance(const CanMessageMultipleDrivesRequest<float>& msg, size_t dataLength, const StringRef& reply) noexcept
{
	const auto drivers = Bitmap<uint16_t>::MakeFromRaw(msg.driversToUpdate);
	if (dataLength < msg.GetActualDataLength(drivers.CountSetBits()))
	{
		reply.copy("bad data length");
		return GCodeResult::error;
	}

	GCodeResult rslt = GCodeResult::ok;
	drivers.Iterate([this, &msg, &reply, &rslt](unsigned int driver, unsigned int count) -> void
						{
							if (driver >= NumDirectDrivers)
							{
								reply.lcatf("No such driver %u.%u", CanInterface::GetCanAddress(), driver);
								rslt = GCodeResult::error;
							}
							else
							{
								dms[driver].extruderShaper.SetKseconds(msg.values[count]);
							}
						}
				   );
	return rslt;
}

void Move::RevertPosition(const CanMessageRevertPosition& msg) noexcept
{
	// Construct a MovementLinearShaped message to revert the position. The move must be shorter than clocksAllowed.
	// When writing this, clocksAllowed was equivalent to 40ms.
	// We allow 10ms delay time to allow the motor to stop and reverse direction, 10ms acceleration time, 5ms steady time and 10ms deceleration time.
	CanMessageMovementLinearShaped msg2;
	msg2.accelerationClocks = msg2.decelClocks = msg.clocksAllowed/4;
	msg2.steadyClocks = msg.clocksAllowed/8;
	msg2.whenToExecute = StepTimer::GetMasterTime() + msg.clocksAllowed/4;
	msg2.numDrivers = NumDirectDrivers;
	msg2.extruderDrives = 0;
	msg2.seq = 0;

	// We start and finish at zero speed, so we move (3/8)*clocksAllowed*topSpeed distance. Since we normalise moves to unit distance, this is equal to one.
	// So topSpeed is 8/(3 * clocksAllowed) and acceleration is (8/(3 * clocksAllowed))/(clocksAllowed/4) = 32/(3 * clocksAllowed^2).
	msg2.acceleration = msg2.deceleration = 32.0/(3.0 * msg.clocksAllowed * msg.clocksAllowed);

	size_t index = 0;
	bool needSteps = false;
	const volatile int32_t * const lastMoveStepsTaken = rings[0].GetLastMoveStepsTaken();
	constexpr size_t numDrivers = min<size_t>(NumDirectDrivers, MaxLinearDriversPerCanSlave);
	for (size_t driver = 0; driver < numDrivers; ++driver)
	{
		int32_t steps = 0;
		if (msg.whichDrives & (1u << driver))
		{
			const int32_t stepsWanted = msg.finalStepCounts[index++];
			const int32_t stepsTaken = lastMoveStepsTaken[driver];
			if (((stepsWanted >= 0 && stepsTaken > stepsWanted) || (stepsWanted <= 0 && stepsTaken < stepsWanted)))
			{
				steps = stepsWanted - stepsTaken;
				needSteps = true;
			}
		}
		msg2.perDrive[driver].steps = steps;
	}

	if (needSteps)
	{
		AddMoveFromRemote(msg2);
	}
}

#endif

// Return the current machine axis and extruder coordinates. They are needed only to service status requests from DWC, PanelDue, M114.
// Return the current machine axis and extruder coordinates. They are needed only to service status requests from DWC, PanelDue, M114.
// Transforming the machine motor coordinates to Cartesian coordinates is quite expensive, and a status request or object model request will call this for each axis.
// So we cache the latest coordinates and only update them if it is some time since we last did, or if we have just waited for movement to stop.
// Interrupts are assumed enabled on entry
// Note, this no longer applies inverse mesh bed compensation or axis skew compensation to the returned machine coordinates, so they are the compensated coordinates!
float Move::LiveMachineCoordinate(unsigned int axisOrExtruder) const noexcept
{
	if (forceLiveCoordinatesUpdate || (millis() - latestLiveCoordinatesFetchedAt > 200 && !liveCoordinatesValid))
	{
		UpdateLiveMachineCoordinates();
		forceLiveCoordinatesUpdate = false;
		latestLiveCoordinatesFetchedAt = millis();
	}
	return latestLiveCoordinates[axisOrExtruder];
}

// Force an update of the live machine coordinates
void Move::UpdateLiveMachineCoordinates() const noexcept
{
	const size_t numVisibleAxes = reprap.GetGCodes().GetVisibleAxes();		// do this before we disable interrupts
	const size_t numTotalAxes = reprap.GetGCodes().GetTotalAxes();			// do this before we disable interrupts

	// Get the positions of each motor
	int32_t currentMotorPositions[MaxAxesPlusExtruders];
	bool motionPending = false;
	motionAdded = false;
	for (size_t i = 0; i < MaxAxesPlusExtruders; ++i)
	{
		currentMotorPositions[i] = dms[i].GetCurrentMotorPosition();
		if (dms[i].MotionPending())
		{
			motionPending = true;
		}
	}

	MotorStepsToCartesian(currentMotorPositions, numVisibleAxes, numTotalAxes, latestLiveCoordinates);		// this is slow, so do it with interrupts enabled

	// Add extrusion so far in the current move to the accumulated extrusion
	for (size_t i = MaxAxesPlusExtruders - reprap.GetGCodes().GetNumExtruders(); i < MaxAxesPlusExtruders; ++i)
	{
		latestLiveCoordinates[i] = currentMotorPositions[i] / driveStepsPerMm[i];
	}

	// Optimisation: if no movement, save the positions for next time
	{
		AtomicCriticalSectionLocker lock;
		if (!motionPending && !motionAdded)
		{
			liveCoordinatesValid = true;
		}
	}
}

void Move::SetLatestCalibrationDeviation(const Deviation& d, uint8_t numFactors) noexcept
{
	latestCalibrationDeviation = d;
	numCalibratedFactors = numFactors;
	reprap.MoveUpdated();
}

void Move::SetInitialCalibrationDeviation(const Deviation& d) noexcept
{
	initialCalibrationDeviation = d;
	reprap.MoveUpdated();
}

// Set the mesh deviation. Caller must call MoveUpdated() after calling this. We don't do that here because the caller may change Move in other ways first.
void Move::SetLatestMeshDeviation(const Deviation& d) noexcept
{
	latestMeshDeviation = d;
}

const char *Move::GetCompensationTypeString() const noexcept
{
	return (usingMesh) ? "mesh" : "none";
}

void Move::WakeMoveTaskFromISR() noexcept
{
	if (moveTask.IsRunning())
	{
		moveTask.GiveFromISR(NotifyIndices::Move);
	}
}

// Laser, IOBits and scanning Z probe support

Task<Move::LaserTaskStackWords> *Move::laserTask = nullptr;		// the task used to manage laser power or IOBits

extern "C" [[noreturn]] void LaserTaskStart(void * pvParameters) noexcept
{
	reprap.GetMove().LaserTaskRun();
}

// This is called when laser mode is selected or IOBits is enabled or a scanning Z probe is configured
void Move::CreateLaserTask() noexcept
{
	TaskCriticalSectionLocker lock;
	if (laserTask == nullptr)
	{
		laserTask = new Task<LaserTaskStackWords>;
		laserTask->Create(LaserTaskStart, "LASER", nullptr, TaskPriority::LaserPriority);
	}
}

// Wake up the laser task, if there is one (must check!). Call this at the start of a new move from standstill (not from an ISR)
void Move::WakeLaserTask() noexcept
{
	if (laserTask != nullptr)
	{
		laserTask->Give(NotifyIndices::Laser);
	}
}

// Wake up the laser task if there is one (must check!) from an ISR
void Move::WakeLaserTaskFromISR() noexcept
{
	if (laserTask != nullptr)
	{
		laserTask->GiveFromISR(NotifyIndices::Laser);
	}
}

void Move::LaserTaskRun() noexcept
{
	for (;;)
	{
		// Sleep until we are woken up by the start of a move
		(void)TaskBase::TakeIndexed(NotifyIndices::Laser);
#if SUPPORT_SCANNING_PROBES || SUPPORT_LASER
		GCodes& gcodes = reprap.GetGCodes();
#endif
#if SUPPORT_SCANNING_PROBES
		if (probeReadingNeeded)
		{
			probeReadingNeeded = false;
			gcodes.TakeScanningProbeReading();
		}
		else
#endif

# if SUPPORT_LASER
			if (gcodes.GetMachineType() == MachineType::laser)
		{
			// Manage the laser power
			uint32_t ticks;
			while ((ticks = rings[0].ManageLaserPower()) != 0)
			{
				(void)TaskBase::TakeIndexed(NotifyIndices::Laser, ticks);
			}
		}
		else
# endif
		{
# if SUPPORT_IOBITS
			// Manage the IOBits
			uint32_t ticks;
			while ((ticks = rings[0].ManageIOBits()) != 0)
			{
				(void)TaskBase::TakeIndexed(NotifyIndices::Laser, ticks);
			}
# endif
		}
	}
}

// Get the accumulated extruder motor steps taken by an extruder since the last call. Used by the filament monitoring code.
// Returns the number of motor steps moved since the last call, and sets isPrinting true unless we are currently executing an extruding but non-printing move
// This is called from the filament monitor ISR and from FilamentMonitor::Spin
int32_t Move::GetAccumulatedExtrusion(size_t logicalDrive, bool& isPrinting) noexcept
{
	AtomicCriticalSectionLocker lock;							// we don't want a move to complete and the ISR update the movement accumulators while we are doing this
	const int32_t ret = movementAccumulators[logicalDrive];
	const int32_t adjustment = dms[logicalDrive].GetNetStepsTaken();
	movementAccumulators[logicalDrive] = -adjustment;
	isPrinting = dms[logicalDrive].IsPrintingExtruderMovement();
	return ret + adjustment;
}

// Add some linear segments to be executed by a driver, taking account of possible input shaping. This is used by linear axes and by extruders.
// We never add a segment that starts earlier than any existing segments, but we may add segments when there are none already.
void Move::AddLinearSegments(const DDA& dda, size_t logicalDrive, uint32_t startTime, const PrepParams& params, float steps, bool useInputShaping, MovementFlags moveFlags) noexcept
{
	if (reprap.GetDebugFlags(Module::Move).IsBitSet(MoveDebugFlags::Segments))
	{
		debugPrintf("AddLin: st=%" PRIu32 " steps=%.1f\n", startTime, (double)steps);
		dda.DebugPrint("addlin");
		params.DebugPrint();
	}

	DriveMovement* const dmp = &dms[logicalDrive];
	const float stepsPerMm = steps/dda.totalDistance;
	const MoveSegment *const oldSegs = dmp->segments;

	// The algorithm for merging segments into existing segments currently assumes that there are no gaps between the existing segments.
	// To ensure this, we must add all of the acceleration, steady speed, and deceleration parts of a move for one impulse before proceeding to the next impulse

	const uint32_t steadyStartTime = startTime + params.accelClocks;
	const uint32_t decelStartTime = steadyStartTime + params.steadyClocks;
	const float steadyDistance = params.decelStartDistance - params.accelDistance;
	const float decelDistance = dda.totalDistance - params.decelStartDistance;

	if (useInputShaping)
	{
		for (size_t index = 0; index < axisShaper.GetNumImpulses(); ++index)
		{
			const float factor = axisShaper.GetImpulseSize(index) * stepsPerMm;
			if (params.accelClocks > 0.0)
			{
				dmp->AddSegment(startTime + axisShaper.GetImpulseDelay(index), (uint32_t)params.accelClocks,
									params.accelDistance * factor, dda.startSpeed * factor, dda.acceleration * factor, moveFlags);
			}
			if (params.steadyClocks > 0.0)
			{
				dmp->AddSegment(steadyStartTime + axisShaper.GetImpulseDelay(index), (uint32_t)params.steadyClocks,
												steadyDistance * factor, dda.topSpeed * factor, 0.0, moveFlags);
			}
			if (params.decelClocks != 0)
			{
				dmp->AddSegment(decelStartTime + axisShaper.GetImpulseDelay(index), (uint32_t)params.decelClocks,
												decelDistance * factor, dda.topSpeed * factor, -(dda.deceleration * factor), moveFlags);
			}
		}
	}
	else
	{
		if (params.accelClocks > 0.0)
		{
			dmp->AddSegment(startTime, (uint32_t)params.accelClocks,
								params.accelDistance * stepsPerMm, dda.startSpeed * stepsPerMm, dda.acceleration * stepsPerMm, moveFlags);
		}
		if (params.steadyClocks > 0.0)
		{
			dmp->AddSegment(steadyStartTime, (uint32_t)params.steadyClocks,
											steadyDistance * stepsPerMm, dda.topSpeed * stepsPerMm, 0.0, moveFlags);
		}
		if (params.decelClocks != 0)
		{
			dmp->AddSegment(decelStartTime, (uint32_t)params.decelClocks,
											decelDistance * stepsPerMm, dda.topSpeed * stepsPerMm, -(dda.deceleration * stepsPerMm), moveFlags);
		}
	}

	// If there were no segments attached to this DM initially, we need to schedule the interrupt for the new segment at the start of the list.
	// Don't do this until we have added all the segments for this move, because the first segment we added may have been modified and/or split when we added further segments to implement input shaping
	if (oldSegs == nullptr)
	{
		AtomicCriticalSectionLocker lock;
		if (dmp->ScheduleFirstSegment())
		{
			const DriveMovement *adp = activeDMs;
			while (adp != dmp)																// if they are equal then this DM is already in the active list
			{
				if (adp == nullptr)
				{
					InsertDM(dmp);
					if (activeDMs == dmp && simulationMode == SimulationMode::off)			// if this is now the first DM in the active list
					{
						if (ScheduleNextStepInterrupt())
						{
							Interrupt();
						}
					}
					break;
				}
				adp = adp->nextDM;
			}
		}
		else if (dmp->state != DMState::idle)
		{
			++stepErrors;
			dmp->state = DMState::idle;
		}
	}
}

// Store the DDA that is executing a homing move involving this drive. Called from DDA::Prepare.
void Move::SetHomingDda(size_t drive, DDA *dda) noexcept
{
	dms[drive].homingDda = dda;
}

// Return true if none of the drives passed has any movement pending
bool Move::AreDrivesStopped(AxesBitmap drives) const noexcept
{
	return drives.IterateWhile([this](unsigned int drive, unsigned int index)->bool
								{
									return dms[drive].segments == nullptr;
								}
							  );
}

// ISR for the step interrupt
void Move::Interrupt() noexcept
{
	if (activeDMs != nullptr)
	{
		Platform& p = reprap.GetPlatform();
		uint32_t now = StepTimer::GetMovementTimerTicks();
		const uint32_t isrStartTime = now;
		for (;;)
		{
			// Generate steps for the current move segments
			StepDrivers(p, now);								// check endstops if necessary and step the drivers

			if (activeDMs == nullptr)
			{
				WakeMoveTaskFromISR();							// we may have just completed a special move, so wake up the Move task so that it can notice that
				break;
			}

			// Schedule a callback at the time when the next step is due, and quit unless it is due immediately
			if (!ScheduleNextStepInterrupt())
			{
				break;
			}

			// The next step is due immediately. Check whether we have been in this ISR for too long already and need to take a break
			now = StepTimer::GetMovementTimerTicks();
			const int32_t clocksTaken = (int32_t)(now - isrStartTime);
			if (clocksTaken >= (int32_t)MoveTiming::MaxStepInterruptTime)
			{
				// Force a break by updating the move start time.
				++numHiccups;
#if SUPPORT_CAN_EXPANSION
				uint32_t hiccupTimeInserted = 0;
#endif
				for (uint32_t hiccupTime = MoveTiming::HiccupTime; ; hiccupTime += MoveTiming::HiccupIncrement)
				{
#if SUPPORT_CAN_EXPANSION
					hiccupTimeInserted += hiccupTime;
#endif
					StepTimer::IncreaseMovementDelay(hiccupTime);

					// Reschedule the next step interrupt. This time it should succeed if the hiccup time was long enough.
					if (!ScheduleNextStepInterrupt())
					{
#if SUPPORT_CAN_EXPANSION
# if SUPPORT_REMOTE_COMMANDS
						if (CanInterface::InExpansionMode())
						{
							//TODO tell the main board we are behind schedule
						}
						else
# endif
						{
							CanMotion::InsertHiccup(hiccupTimeInserted);		// notify expansion boards of the increased delay
						}
#endif
						return;
					}
					// We probably had an interrupt that delayed us further. Recalculate the hiccup length, also we increase the hiccup time on each iteration.
					now = StepTimer::GetMovementTimerTicks();
				}
			}
		}
	}
}

// Move timer callback function
/*static*/ void Move::TimerCallback(CallbackParameter p) noexcept
{
	static_cast<Move*>(p.vp)->Interrupt();
}

// Remove this drive from the list of drives with steps due and put it in the completed list
// Called from the step ISR only.
void Move::DeactivateDM(DriveMovement *dmToRemove) noexcept
{
	DriveMovement **dmp = &activeDMs;
	while (*dmp != nullptr)
	{
		DriveMovement * const dm = *dmp;
		if (dm == dmToRemove)
		{
			(*dmp) = dm->nextDM;
			dm->state = DMState::idle;
			break;
		}
		dmp = &(dm->nextDM);
	}
}

// Check the endstops, given that we know that this move checks endstops.
// If executingMove is set then the move is already being executed; otherwise we are preparing to commit the move.
#if SUPPORT_CAN_EXPANSION
// Returns true if the caller needs to wake the async sender task because CAN-connected drivers need to be stopped
bool Move::CheckEndstops(Platform& platform, bool executingMove) noexcept
#else
void Move::CheckEndstops(Platform& platform, bool executingMove) noexcept
#endif
{
#if SUPPORT_CAN_EXPANSION
	bool wakeAsyncSender = false;
#endif
	while (true)
	{
		const EndstopHitDetails hitDetails = platform.GetEndstops().CheckEndstops();

		switch (hitDetails.GetAction())
		{
		case EndstopHitAction::stopAll:
#if SUPPORT_CAN_EXPANSION
			if (StopAllDrivers(executingMove)) { wakeAsyncSender = true; }
#else
			StopAllDrivers(executingMove);
#endif
			if (hitDetails.isZProbe)
			{
				reprap.GetGCodes().MoveStoppedByZProbe();
			}
			else
			{
				// Get the DDA associated with the axis that has triggered
				DDA *homingDda = dms[hitDetails.axis].homingDda;
				if (homingDda != nullptr && homingDda->GetState() == DDA::committed && homingDda->IsCheckingEndstops())
				{
					if (hitDetails.setAxisLow)
					{
						kinematics->OnHomingSwitchTriggered(hitDetails.axis, false, driveStepsPerMm, *homingDda);
						reprap.GetGCodes().SetAxisIsHomed(hitDetails.axis);
					}
					else if (hitDetails.setAxisHigh)
					{
						kinematics->OnHomingSwitchTriggered(hitDetails.axis, true, driveStepsPerMm, *homingDda);
						reprap.GetGCodes().SetAxisIsHomed(hitDetails.axis);
					}
				}
			}
#if SUPPORT_CAN_EXPANSION
			return wakeAsyncSender;
#else
			return;
#endif

		case EndstopHitAction::stopAxis:
			// We must stop the drive before we mess with its coordinates
#if SUPPORT_CAN_EXPANSION
			if (StopAxisOrExtruder(executingMove, hitDetails.axis)) { wakeAsyncSender = true; }
#else
			StopAxisOrExtruder(executingMove, hitDetails.axis);
#endif
			{
				// Get the DDA associated with the axis that has triggered
				DDA *homingDda = dms[hitDetails.axis].homingDda;
				if (homingDda != nullptr && homingDda->GetState() == DDA::committed && homingDda->IsCheckingEndstops())
				{
					if (hitDetails.setAxisLow)
					{
						kinematics->OnHomingSwitchTriggered(hitDetails.axis, false, driveStepsPerMm, *homingDda);
						reprap.GetGCodes().SetAxisIsHomed(hitDetails.axis);
					}
					else if (hitDetails.setAxisHigh)
					{
						reprap.GetMove().GetKinematics().OnHomingSwitchTriggered(hitDetails.axis, true, driveStepsPerMm, *homingDda);
						reprap.GetGCodes().SetAxisIsHomed(hitDetails.axis);
					}
				}
			}
			break;

		case EndstopHitAction::stopDriver:
#if SUPPORT_CAN_EXPANSION
			if (hitDetails.driver.IsRemote())
			{
				if (executingMove)
				{
					int32_t netStepsTaken;
					const bool wasMoving = dms[hitDetails.axis].StopDriver(netStepsTaken);
					if (wasMoving && CanMotion::StopDriverWhenExecuting(hitDetails.driver, netStepsTaken)) { wakeAsyncSender = true; }
				}
				else
				{
					CanMotion::StopDriverWhenProvisional(hitDetails.driver);
				}
			}
			else
#endif
			{
				platform.DisableSteppingDriver(hitDetails.driver.localDriver);
			}

			{
				// Get the DDA associated with the axis that has triggered
				DDA *homingDda = dms[hitDetails.axis].homingDda;
				if (homingDda != nullptr && homingDda->GetState() == DDA::committed && homingDda->IsCheckingEndstops())
				{
					if (hitDetails.setAxisLow)
					{
						kinematics->OnHomingSwitchTriggered(hitDetails.axis, false, driveStepsPerMm, *homingDda);
						reprap.GetGCodes().SetAxisIsHomed(hitDetails.axis);
					}
					else if (hitDetails.setAxisHigh)
					{
						kinematics->OnHomingSwitchTriggered(hitDetails.axis, true, driveStepsPerMm, *homingDda);
						reprap.GetGCodes().SetAxisIsHomed(hitDetails.axis);
					}
				}
			}
			break;

		default:
#if SUPPORT_CAN_EXPANSION
			return wakeAsyncSender;
#else
			return;
#endif
		}
	}
}

// Generate the step pulses of internal drivers used by this DDA
void Move::StepDrivers(Platform& p, uint32_t now) noexcept
{
	uint32_t driversStepping = 0;
	MovementFlags flags;
	flags.Clear();
	DriveMovement* dm = activeDMs;
	while (dm != nullptr && (int32_t)(now - dm->nextStepTime) >= 0)			// if the next step is due
	{
		driversStepping |= p.GetDriversBitmap(dm->drive);
		flags |= dm->segmentFlags;
		dm = dm->nextDM;
	}

	if (flags.checkEndstops)
	{
#if SUPPORT_CAN_EXPANSION
		if (CheckEndstops(p, true)) { CanInterface::WakeAsyncSender(); }
#else
		CheckEndstops(p, true);												// call out to a separate function because this may help cache locality in the more common and time-critical case where we don't call it
#endif

		// Calling CheckEndstops may have removed DMs from the active list, also it takes time; so re-check which drives need steps
		driversStepping = 0;
		now = StepTimer::GetMovementTimerTicks();
		dm = activeDMs;
		while (dm != nullptr && (int32_t)(now - dm->nextStepTime) >= 0)		// if the next step is due
		{
			driversStepping |= p.GetDriversBitmap(dm->drive);
			dm = dm->nextDM;
		}
	}

	driversStepping &= p.GetSteppingEnabledDrivers();

#ifdef DUET3_MB6XD
	if (driversStepping != 0)
	{
		// Wait until step low and direction setup time have elapsed
		const uint32_t locLastStepPulseTime = lastStepHighTime;
		const uint32_t locLastDirChangeTime = lastDirChangeTime;
		while (now - locLastStepPulseTime < p.GetSlowDriverStepPeriodClocks() || now - locLastDirChangeTime < p.GetSlowDriverDirSetupClocks())
		{
			now = StepTimer::GetTimerTicks();
		}

		StepPins::StepDriversLow(StepPins::AllDriversBitmap & (~driversStepping));		// disable the step pins of the drivers we don't want to step
		StepPins::StepDriversHigh(driversStepping);										// set up the drivers that we do want to step

		// Trigger the TC so that it generates a step pulse
		STEP_GATE_TC->TC_CHANNEL[STEP_GATE_TC_CHAN].TC_CCR = TC_CCR_SWTRG;
		lastStepHighTime = StepTimer::GetTimerTicks();
	}

	// Calculate the next step times. We must do this even if no local drivers are stepping in case endstops or Z probes are active.
	for (DriveMovement *dm2 = activeDMs; dm2 != dm; dm2 = dm2->nextDM)
	{
		(void)dm2->CalcNextStepTime();								// calculate next step times
	}
#else
# if SUPPORT_SLOW_DRIVERS											// if supporting slow drivers
	if ((driversStepping & p.GetSlowDriversBitmap()) != 0)			// if using some slow drivers
	{
		// Wait until step low and direction setup time have elapsed
		uint32_t lastStepPulseTime = lastStepLowTime;
		uint32_t rawNow;
		do
		{
			rawNow = StepTimer::GetTimerTicks();
		} while (rawNow - lastStepPulseTime < p.GetSlowDriverStepLowClocks() || rawNow - lastDirChangeTime < p.GetSlowDriverDirSetupClocks());

		StepPins::StepDriversHigh(driversStepping);					// step drivers high
		lastStepPulseTime = StepTimer::GetTimerTicks();

		for (DriveMovement *dm2 = activeDMs; dm2 != dm; dm2 = dm2->nextDM)
		{
			(void)dm2->CalcNextStepTime();							// calculate next step times
		}

		while (StepTimer::GetTimerTicks() - lastStepPulseTime < p.GetSlowDriverStepHighClocks()) {}
		StepPins::StepDriversLow(driversStepping);					// step drivers low
		lastStepLowTime = StepTimer::GetTimerTicks();
	}
	else
# endif
	{
		StepPins::StepDriversHigh(driversStepping);					// step drivers high
# if SAME70
		__DSB();													// without this the step pulse can be far too short
# endif
		for (DriveMovement *dm2 = activeDMs; dm2 != dm; dm2 = dm2->nextDM)
		{
			(void)dm2->CalcNextStepTime();							// calculate next step times
		}

		StepPins::StepDriversLow(driversStepping);					// step drivers low
	}
#endif

	// Remove those drives from the list, update the direction pins where necessary, and re-insert them so as to keep the list in step-time order.
	DriveMovement *dmToInsert = activeDMs;							// head of the chain we need to re-insert
	activeDMs = dm;													// remove the chain from the list
	while (dmToInsert != dm)										// note that both of these may be nullptr
	{
		DriveMovement * const nextToInsert = dmToInsert->nextDM;
		if (dmToInsert->state >= DMState::firstMotionState)
		{
			if (dmToInsert->directionChanged)
			{
				dmToInsert->directionChanged = false;
				SetDirection(p, dmToInsert->drive, dmToInsert->direction);
			}
			InsertDM(dmToInsert);
		}
		else if (dmToInsert->state != DMState::idle)
		{
			++stepErrors;
			dmToInsert->state = DMState::idle;
		}
		dmToInsert = nextToInsert;
	}
}

void Move::SetDirection(Platform& p, size_t axisOrExtruder, bool direction) noexcept
{
#ifdef DUET3_MB6XD
	while (StepTimer::GetTimerTicks() - lastStepHighTime < p.GetSlowDriverDirHoldClocksFromLeadingEdge()) { }
#else
	const bool isSlowDriver = (p.GetDriversBitmap(axisOrExtruder) & p.GetSlowDriversBitmap()) != 0;
	if (isSlowDriver)
	{
		while (StepTimer::GetTimerTicks() - lastStepLowTime < p.GetSlowDriverDirHoldClocksFromTrailingEdge()) { }
	}
#endif

	p.SetDriverDirection(axisOrExtruder, direction);

#ifndef DUET3_MB6XD
	if (isSlowDriver)
#endif
	{
		lastDirChangeTime = StepTimer::GetTimerTicks();
	}
}
// Simulate stepping the drivers, for debugging.
// This is basically a copy of StepDrivers except that instead of being called from the timer ISR and generating steps,
// it is called from the Move task and outputs info on the step timings. It ignores endstops.
void Move::SimulateSteppingDrivers(Platform& p) noexcept
{
	static uint32_t lastStepTime;
	static bool checkTiming = false;
	static uint8_t lastDrive = 0;

	DriveMovement* dm = activeDMs;
	if (dm != nullptr)
	{
		const uint32_t dueTime = dm->nextStepTime;
		while (dm != nullptr && dueTime >= dm->nextStepTime)			// if the next step is due
		{
			const uint32_t timeDiff = dm->nextStepTime - lastStepTime;
			const bool badTiming = checkTiming && dm->drive == lastDrive && (timeDiff < 10 || timeDiff > 100000000);
			if (dm->nextStep == 1)
			{
				dm->DebugPrint();
				MoveSegment::DebugPrintList('s', dm->segments);
			}
			debugPrintf("%10" PRIu32 " D%u %c ns=%" PRIi32 "%s", dm->nextStepTime, dm->drive, (dm->direction) ? 'F' : 'B', dm->nextStep, (badTiming) ? " *\n" : "\n");
			lastDrive = dm->drive;
			dm = dm->nextDM;
		}
		lastStepTime = dueTime;
		checkTiming = true;

		for (DriveMovement *dm2 = activeDMs; dm2 != dm; dm2 = dm2->nextDM)
		{
			(void)dm2->CalcNextStepTime();								// calculate next step times
		}

		// Remove those drives from the list, update the direction pins where necessary, and re-insert them so as to keep the list in step-time order.
		DriveMovement *dmToInsert = activeDMs;							// head of the chain we need to re-insert
		activeDMs = dm;													// remove the chain from the list
		while (dmToInsert != dm)										// note that both of these may be nullptr
		{
			DriveMovement * const nextToInsert = dmToInsert->nextDM;
			if (dmToInsert->state >= DMState::firstMotionState)
			{
				dmToInsert->directionChanged = false;
				InsertDM(dmToInsert);
			}
			else if (dmToInsert->state != DMState::idle)
			{
				dmToInsert->DebugPrint();
			}
			dmToInsert = nextToInsert;
		}
	}

	// If there are no more steps to do and the time for the move has nearly expired, flag the move as complete
	if (activeDMs == nullptr)
	{
		checkTiming = false;		// don't check the timing of the first step in the next move
	}
}

// This is called when we abort a move because we have hit an endstop.
// It stops all drives and adjusts the end points of the current move to account for how far through the move we got.
bool Move::StopAllDrivers(bool executingMove) noexcept
{
	bool wakeAsyncSender = false;
	for (size_t drive = 0; drive < MaxAxesPlusExtruders; ++drive)
	{
		if (StopAxisOrExtruder(executingMove, drive)) { wakeAsyncSender = true; }
	}
	return wakeAsyncSender;
}

// Stop a drive and re-calculate the end position. Return true if any remote drivers were scheduled to be stopped.
bool Move::StopAxisOrExtruder(bool executingMove, size_t logicalDrive) noexcept
{
	int32_t netStepsTaken;
	const bool wasMoving = dms[logicalDrive].StopDriver(netStepsTaken);
	bool wakeAsyncSender = false;
#if SUPPORT_CAN_EXPANSION
	const Platform& p = reprap.GetPlatform();
	if (logicalDrive < reprap.GetGCodes().GetTotalAxes())
	{
		const AxisDriversConfig& cfg = p.GetAxisDriversConfig(logicalDrive);
		for (size_t i = 0; i < cfg.numDrivers; ++i)
		{
			const DriverId driver = cfg.driverNumbers[i];
			if (driver.IsRemote())
			{
				if (executingMove)
				{
					if (wasMoving)
					{
						if (CanMotion::StopDriverWhenExecuting(driver, netStepsTaken)) { wakeAsyncSender = true; }
					}
				}
				else
				{
					CanMotion::StopDriverWhenProvisional(driver);
				}
			}
		}
	}
	else
	{
		const DriverId driver = p.GetExtruderDriver(LogicalDriveToExtruder(logicalDrive));
		if (executingMove)
		{
			if (wasMoving)
			{
				if (CanMotion::StopDriverWhenExecuting(driver, netStepsTaken)) { wakeAsyncSender = true; }
			}
		}
		else
		{
			CanMotion::StopDriverWhenProvisional(driver);
		}
	}
#else
	(void)wasMoving;
#endif
	motorPositionsAfterScheduledMoves[logicalDrive] = dms[logicalDrive].GetCurrentMotorPosition();
	return wakeAsyncSender;
}

#if SUPPORT_REMOTE_COMMANDS

// Stop a drive and re-calculate the end position
void Move::StopDriveFromRemote(size_t drive) noexcept
{
	dms[drive].StopDriverFromRemote();
	motorPositionsAfterScheduledMoves[drive] = dms[drive].GetCurrentMotorPosition();
}

#endif

#if 0

// THIS CODE IS NOT USED. It's here because we need to replicate the functionality somewhere else.
void Move::OnMoveCompleted(DDA *cdda, Platform& p) noexcept
{
	bool wakeLaserTask = false;
	if (cdda->IsScanningProbeMove())
	{
		reprap.GetMove().SetProbeReadingNeeded();
		wakeLaserTask = true;						// wake the laser task to take a reading
	}

	// The following finish time is wrong if we aborted the move because of endstop or Z probe checks.
	// However, following a move that checks endstops or the Z probe, we always wait for the move to complete before we schedule another, so this doesn't matter.
	const uint32_t finishTime = cdda->GetMoveFinishTime();	// calculate when this move should finish

	CurrentMoveCompleted();							// tell the DDA ring that the current move is complete

	// Try to start a new move
	const DDA::DDAState st = getPointer->GetState();
	if (st == DDA::frozen)
	{
#if SUPPORT_LASER || SUPPORT_IOBITS
		if (StartNextMove(p, finishTime))
		{
			wakeLaserTask = true;
		}
#else
		(void)StartNextMove(p, finishTime);
#endif
	}
	else
	{
		if (st == DDA::provisional)
		{
			++numPrepareUnderruns;					// there are more moves available, but they are not prepared yet. Signal an underrun.
		}
		else if (!waitingForRingToEmpty)
		{
			++numNoMoveUnderruns;
		}
		p.ExtrudeOff();								// turn off ancillary PWM
		if (cdda->GetTool() != nullptr)
		{
			cdda->GetTool()->StopFeedForward();
		}
#if SUPPORT_LASER
		if (reprap.GetGCodes().GetMachineType() == MachineType::laser)
		{
			p.SetLaserPwm(0);						// turn off the laser
		}
#endif
		waitingForRingToEmpty = false;
	}

	if (wakeLaserTask)
	{
		Move::WakeLaserTaskFromISR();
	}
}

#endif

// Adjust the motor endpoints without moving the motors. Called after auto-calibrating a linear delta or rotary delta machine.
// There must be no pending movement when calling this!
void Move::AdjustMotorPositions(const float adjustment[], size_t numMotors) noexcept
{
	for (size_t drive = 0; drive < numMotors; ++drive)
	{
		dms[drive].AdjustMotorPosition(lrintf(adjustment[drive] * driveStepsPerMm[drive]));
		motorPositionsAfterScheduledMoves[drive] = dms[drive].GetCurrentMotorPosition();
	}

	liveCoordinatesValid = false;		// force the live XYZ position to be recalculated
}

// Reset all extruder positions to zero. Called when we start a print.
void Move::ResetExtruderPositions() noexcept
{
	for (size_t drive = MaxAxesPlusExtruders - reprap.GetGCodes().GetNumExtruders(); drive < MaxAxesPlusExtruders; ++drive)
	{
		dms[drive].SetMotorPosition(0);
	}
}

#if SUPPORT_CAN_EXPANSION

// This is called when we update endstop states because of a message from a remote board.
// In time we may use it to help implement interrupt-driven local endstops too, but for now those are checked in the step ISR by a direct call to DDA::CheckEndstops().
void Move::OnEndstopOrZProbeStatesChanged() noexcept
{
	const uint32_t oldPrio = ChangeBasePriority(NvicPriorityStep);		// shut out the step interrupt
	const bool wakeAsyncSender = CheckEndstops(reprap.GetPlatform(), true);
	RestoreBasePriority(oldPrio);										// allow step interrupts again
	if (wakeAsyncSender) { CanInterface::WakeAsyncSender(); }
}

#endif

#if SUPPORT_REMOTE_COMMANDS

// Stop some drivers and update the corresponding motor positions
void Move::StopDriversFromRemote(uint16_t whichDrives) noexcept
{
	DriversBitmap dr(whichDrives);
	dr.Iterate([this](size_t drive, unsigned int)
				{
					StopDriveFromRemote(drive);
				}
			  );
}

#endif

#if SUPPORT_ASYNC_MOVES

// Get and lock the aux move buffer. If successful, return a pointer to the buffer.
// The caller must not attempt to lock the aux buffer more than once, and must call ReleaseAuxMove to release the buffer.
AsyncMove *Move::LockAuxMove() noexcept
{
	InterruptCriticalSectionLocker lock;
	if (!auxMoveLocked && !auxMoveAvailable)
	{
		auxMoveLocked = true;
		return &auxMove;
	}
	return nullptr;
}

// Release the aux move buffer and optionally signal that it contains a move
// The caller must have locked the buffer before calling this. If it calls this with hasNewMove true, it must have populated the move buffer with the move details
void Move::ReleaseAuxMove(bool hasNewMove) noexcept
{
	auxMoveAvailable = hasNewMove;
	auxMoveLocked = false;
	MoveAvailable();
}

// Configure height following
GCodeResult Move::ConfigureHeightFollowing(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException)
{
	if (heightController == nullptr)
	{
		heightController = new HeightController;
	}
	return heightController->Configure(gb, reply);
}

// Start/stop height following
GCodeResult Move::StartHeightFollowing(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException)
{
	if (heightController == nullptr)
	{
		reply.copy("Height following has not been configured");
		return GCodeResult::error;
	}
	return heightController->StartHeightFollowing(gb, reply);
}

#endif

// End
