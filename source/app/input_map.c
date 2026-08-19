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

uint32_t inputKey(OptionsAction a)
{
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
	return current()->look_sensitivity;
}

bool inputInvertLook(void)
{
	return current()->invert_look;
}
