/*
 * Filament.h
 *
 *  Created on: 13 Jun 2017
 *      Author: Christian
 */

#ifndef SRC_TOOLS_FILAMENT_H
#define SRC_TOOLS_FILAMENT_H

#include <cstddef>

const size_t FilamentNameLength = 32;

class Filament
{
public:
	explicit Filament(int extr) noexcept;

	int GetExtruder() const noexcept { return extruder; }				// Returns the assigned extruder drive
	const char *_ecv_array GetName() const noexcept { return name.c_str(); }	// Returns the name of the currently loaded filament

	// TODO: Add support for filament counters, tool restrictions etc.
	// These should be stored in a dedicate file per filament directory like /filaments/<material>/filament.json

	bool IsLoaded() const noexcept { return !name.IsEmpty(); }			// Returns true if a valid filament is assigned to this instance
	void Load(const char *_ecv_array filamentName) noexcept;			// Loads filament parameters from the SD card
	void Unload() noexcept;												// Unloads the current filament

	void LoadAssignment() noexcept;										// Read the assigned material for the given extruder from the SD card

	static void SaveAssignments() noexcept;								// Rewrite the CSV file containing the extruder <-> filament assignments
	static Filament *_ecv_null GetFilamentByExtruder(const int extr) noexcept;	// Retrieve the Filament instance assigned to the given extruder drive

private:
	static const char *_ecv_array const FilamentAssignmentFile;			// In which file the extruder <-> filament assignments are stored
	static const char *_ecv_array const FilamentAssignmentFileComment;	// The comment we write at the start of this file to ensure its integrity

	static Filament *_ecv_null filamentList;
	Filament *_ecv_null next;

	int extruder;
	String<FilamentNameLength> name;
};

#endif
