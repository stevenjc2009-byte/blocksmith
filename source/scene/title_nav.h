// The multiplayer screen's navigation decision, pulled out of scene/title.c so it can be
// host-tested — the same split, for the same reason, as scene/ui_layout.h (rect arithmetic out
// of scene/ui.c) and app/whatsnew.h (note parsing out of title.c). title.c itself draws with
// gfx/sprite.h and gfx/font.h and includes <3ds.h> for KEY_*, so it cannot be linked outside a
// devkitARM build; nothing in this pair needs a key constant, a rectangle or a render target,
// only the four booleans title.c has already gathered by the time the decision is made.
//
// What it decides is one frame's answer to two questions that were previously answered by two
// unrelated `if`s twenty lines apart, and that is the whole point of it being one function:
// they are not independent. Backing out set the screen to TITLE_SCR_MAIN and then fell straight
// through into the entry gate, so pressing B on the exact frame the block table finished
// syncing — or the frame the sync deadline expired — backed out of multiplayer AND entered the
// server's world in the same frame. main.c acts on the action, so the player got the world.
//
// scene/title.c's drawMultiplayer() is the only caller. It gathers the inputs and applies the
// outputs; it holds no copy of the rule below (source/scene/title_nav_test.c is what proves the
// rule, and it links this real file — see its own header).
#pragma once

#include <stdbool.h>

// One frame of the multiplayer screen, after its buttons have been drawn and handled.
typedef struct {
	// BACK was tapped, or KEY_B went down this frame. Both are the same intent — leave — and
	// title.c already treats them as one condition.
	bool back;

	// A live session, as read at the TOP of the frame. Deliberately the top-of-frame value:
	// the DISCONNECT button on this same screen runs netDisconnect() midway through, and this
	// flag is what the screen's whole layout was built from, so re-reading it here would make
	// the button row and the decision disagree about which screen was just drawn.
	bool connected;

	// networldWorldSeed(NULL) and networldRegistryWaiting(), both read AFTER this frame's
	// buttons have run. That ordering is load-bearing and is why DISCONNECT was already safe:
	// netDisconnect() reaches networldInit(), which clears the seed, so the gate below is false
	// on the very same frame the player pressed it. Nothing else in this struct protects it.
	bool have_world_seed;
	bool registry_waiting;
} TitleMpNav;

typedef struct {
	bool leave_to_main;   // go back to the title screen's main list
	bool start_server;    // enter the server's world (TITLE_START_SERVER)
} TitleMpNavOut;

// Never both. Leaving wins: the player asked for a specific thing, and the entry gate is a
// condition that merely happened to become true on the same frame.
TitleMpNavOut titleMpNav(TitleMpNav in);
