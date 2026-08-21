#pragma once

#include <stdbool.h>

// Installs an APT hook that tracks sleep/wake transitions.
// Call once at boot after netInit(). Safe to call before networldInit().
void sleepInit(void);

// Removes the APT hook. Call at app_shutdown after updaterExit().
void sleepExit(void);

// True between ATHOOK_ONSLEEP and APTHOOK_ONWAKEUP.
bool sleepIsAsleep(void);

// Called at the top of the main-loop body (after hidScanInput).
// Returns true if the app is asleep and the caller should skip
// simulation, net updates, and drawing for this frame.
bool sleepShouldSkip(void);

// Notify the net module to enter/exit heartbeat mode.
// Defined here so callers do not need to know the net internals.
void sleepNetHeartbeat(void);
