// Debug menu registry: pure logic, host-testable, no <3ds.h>.
//
// A registry of named entries that the debug-menu UI draws and dispatches.
// Each entry has a kind (TOGGLE, SLIDER_INT, ACTION, INFO), a getter/setter
// or callback, and an "available" flag so future features can register
// placeholders that show greyed-out rows without faking behaviour.
//
// The registry is a linked list threaded through a fixed-capacity pool, so
// iteration is O(n) and indexing is O(n) — acceptable because the menu has
// a handful of entries, not thousands. The pool is statically allocated so
// no malloc is needed and the host test can reset it cleanly.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define DEBUG_MENU_MAX_ENTRIES 32

typedef enum {
	DEBUG_TOGGLE,
	DEBUG_SLIDER_INT,
	DEBUG_ACTION,
	DEBUG_INFO,
} DebugEntryKind;

typedef struct DebugEntry {
	const char*       name;
	DebugEntryKind    kind;
	bool              available;

	// TOGGLE
	bool (*getBool)(void* ctx);
	void (*setBool)(void* ctx, bool val);

	// SLIDER_INT
	int  (*getInt)(void* ctx);
	void (*setInt)(void* ctx, int val);
	int               slider_min, slider_max;

	// ACTION
	void (*action)(void* ctx);

	// INFO: fill buf, return buf, or return NULL to show nothing
	const char* (*info)(char* buf, int cap, void* ctx);

	void*              ctx;
	int                next;   // index into pool, -1 = end
} DebugEntry;

// Reset the registry to empty. Call once at init or in tests.
void debugMenuReset(void);

// Register an entry. Returns the entry pointer, or NULL if the pool is full.
// The caller must fill in the fields after registration (name, kind, callbacks,
// etc.) — this function only allocates a slot and links it in.
DebugEntry* debugMenuRegister(void);

// Number of registered entries (including unavailable ones).
int debugMenuCount(void);

// Entry by iteration index (0 .. count-1). NULL if out of range.
DebugEntry* debugMenuEntry(int index);

// Head of the linked list (first registered entry). -1 = empty.
int debugMenuHead(void);

// Next entry after `index`, or -1 if this was the last.
int debugMenuNext(int index);
