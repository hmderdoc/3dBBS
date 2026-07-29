#ifndef SETTINGS_H
#define SETTINGS_H

#include <3ds/types.h>
#include <stdbool.h>

// Global (not per-board) preferences, persisted at sdmc:/3dBBS/settings.txt
// as plain "key=value" lines. The file is written once with commented
// defaults on first run so it can be edited on a PC; unknown keys are
// preserved-by-ignoring (we rewrite only when a value actually changes).

typedef struct {
	// Minutes to hold a live connection open after the lid closes before
	// letting the system sleep normally. 0 disables the hold entirely
	// (lid close sleeps immediately, the stock behaviour).
	int lidKeepaliveMin;
	// Drive the RGB notification LED from receive activity (hue sweep
	// speed and brightness track throughput). 0 leaves the LED alone.
	bool led;
} Settings;

void settingsLoad(void);
const Settings* settingsGet(void);

#endif
