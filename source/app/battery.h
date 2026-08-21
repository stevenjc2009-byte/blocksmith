#pragma once

#include <stdbool.h>
#include <stdint.h>

// Opens the PTMU service. Call once at boot after screenInit().
void batteryInit(void);

// Closes the PTMU service. Call at app_shutdown after sleepExit().
void batteryExit(void);

// Reads battery level (0-5) and charge state, cached to at most once/second.
void batteryPoll(void);

// Last polled battery level, 0-5. 0 = empty, 5 = full.
int batteryLevel(void);

// True if the charger is connected.
bool batteryCharging(void);

// Level mapped to a 0-4 bar count for a 4-bar gauge display.
int batteryBars(void);

// True if battery is critically low (level <= 1 and not charging).
bool batteryLow(void);

// Draws a 4-segment battery gauge at (x, y) on the bottom screen.
// Requires an active sprite pass (spriteBegin already called).
// The font texture must already be bound.
void batteryDraw(float x, float y);
