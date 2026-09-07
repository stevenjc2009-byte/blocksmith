#include "app/input_map.h"

// Initialised from optionsDefaults rather than from a hand-written table, so there is exactly
// one definition of "the shipped bindings" in the program and it lives in app/options.c next
// to the ini keys that name them. A second table here would be a second thing to keep in
// step, and the symptom of it drifting would be that the game plays with one set of buttons
// until the first options load and a different set afterwards.
//
// s_init exists because C has no way to run optionsDefaults into a static initialiser. It is
// only ever written on the main thread, from the accessors below, before the worker thread
// exists — nothing under scene/ runs off the main thread (see app/worker.c's JOB_MESH
// comment for why meshing in particular stayed there), so this needs no lock.
static Options s_opts;
static bool    s_init;

// v1.9.1. Same thread and same lifetime argument as s_init above: written once per frame from
// main.c's loop, read from the same thread by every inputKey call in that frame.
static bool    s_menu_owns_pad;

static const Options* current(void)
{
	if (!s_init) {
		optionsDefaults(&s_opts);
		s_init = true;
	}
	return &s_opts;
}

void inputMapSet(const Options* o)
{
	if (o) s_opts = *o;
	else   optionsDefaults(&s_opts);
	s_init = true;
}

void inputMapSetMenuOwnsPad(bool owns)
{
	s_menu_owns_pad = owns;
}

uint32_t inputKey(OptionsAction a)
{
	// Checked before the bound lookup, not after: an out-of-range action already answers 0, so
	// the order only matters for readability, and a reader should see the modal gate first.
	if (s_menu_owns_pad) return 0;

	// Compared as unsigned, and only against the upper bound. `a < 0` was the obvious way to
	// write this and is not writable: OptionsAction has no negative enumerator, so the
	// compiler picks an unsigned underlying type for it and -Wtype-limits correctly reports
	// the test as always false. The cast makes the one comparison that can actually be false
	// catch both ends — a caller that passed a negative int arrives here as a very large
	// unsigned and still fails the bound.
	if ((unsigned)a >= (unsigned)ACTION_COUNT) return 0;
	return current()->bindings[a];
}

float inputLookScale(void)
{
	// v1.9.1 S4. Zero while a modal menu owns the pad, which freezes the camera without any new
	// API and without skipping playerUpdate.
	//
	// The blueprint asked for `if (!paused && !bar_open) cameraUpdate(...)` in main.c. That line
	// would have compiled and done nothing: main.c's only cameraUpdate call is inside the
	// `#if BS_FLY` branch (main.c:6285), and the shipped build takes the other one, where the
	// look lives INSIDE playerUpdate — scene/player.c:84 calls cameraLook before it does any
	// physics, and cameraLook reads the circle pad directly (scene/camera.c:79-80) rather than
	// through inputKey. So the pad would have driven the bar cursor and the camera at once, and
	// the guard meant to stop that would have been in a branch nobody builds.
	//
	// Skipping playerUpdate instead was rejected by the design and stays rejected: a player
	// frozen mid-jump is visible to every other client in a multiplayer world. Adding a
	// playerSetLookEnabled/cameraSetLookEnabled pair was rejected too, as two more APIs for what
	// one guard here does. This is that one guard. cameraLook multiplies its rotation by
	// LOOK_SPEED * inputLookScale() (camera.c:89, the only caller of this function in the whole
	// tree), so zero here is exactly "no rotation this frame" — the pitch clamp and everything
	// downstream still run on the unchanged angles.
	//
	// Returning 0 rather than latching the angles also means nothing has to be restored when the
	// menu closes: the very next frame reads the real sensitivity again.
	if (s_menu_owns_pad) return 0.0f;

	return current()->look_sensitivity;
}

bool inputInvertLook(void)
{
	return current()->invert_look;
}
