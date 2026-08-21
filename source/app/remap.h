// Universal input remapping: pure logic layer.
//
// All physical buttons can be remapped to any action. This header exposes the
// binding table, conflict resolution (swap policy), and reset-to-defaults —
// everything that can be tested on the host without <3ds.h>.
//
// The Options struct (app/options.h) is the persistence owner: its bindings[]
// array is what optionsSave/optionsLoad write. This module operates on a working
// copy; the caller applies changes back to Options and calls inputMapSet() when
// the player confirms.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app/options.h"

// Working state for the remap screen. Separate from Options so the player can
// cancel without having modified the live settings.
typedef struct {
	uint32_t bindings[ACTION_COUNT];
	int      capture_action;   // ACTION_COUNT when not capturing
} RemapState;

// Fills `rs` with the current bindings from `o`. Call once when opening the
// remap screen.
void remapInit(RemapState* rs, const Options* o);

// Apply the remap state back into `o`. Call when the player confirms.
void remapApply(const RemapState* rs, Options* o);

// Number of mappable actions. Equals ACTION_COUNT but avoids exposing the enum
// to callers that only need a count.
int remapActionCount(void);

// Human-readable name for an action index. Returns NULL for out-of-range.
const char* remapActionName(int action);

// Human-readable name for a key bit. Returns "???" for unknown values.
const char* remapKeyName(uint32_t key);

// The current binding for `action`. Returns 0 for out-of-range.
uint32_t remapGetBinding(const RemapState* rs, int action);

// Begin capture mode for `action`. The next call to remapApplyCapture will
// assign the pressed key to this action.
void remapStartCapture(RemapState* rs, int action);

// True while waiting for a button press.
bool remapIsCapturing(const RemapState* rs);

// Which action is being captured, or ACTION_COUNT if not capturing.
int remapCaptureAction(const RemapState* rs);

// Apply a captured key press. If the key is already bound to another action,
// the two bindings are swapped (conflict resolution). Returns true if a swap
// occurred, false if the key was unbound or bound to the same action.
// After this call, capture mode ends regardless.
bool remapApplyCapture(RemapState* rs, uint32_t key);

// Reset all bindings to the shipped defaults.
void remapReset(RemapState* rs);

// Find which action (if any) is already bound to `key`. Returns ACTION_COUNT
// if no action uses this key. Used internally and by the UI to show conflicts.
int remapFindAction(const RemapState* rs, uint32_t key);
