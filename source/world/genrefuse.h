// v1.8.3. The two questions main.c's world-entry path asks about a GenVersionStatus: does this
// stop the load, and what does the player get told about it.
//
// ── Why this is a file of its own, and why it is a header ─────────────────────────────
//
// world/genversion.c decides what a world's stamp MEANS. It has never decided what to do about
// it — that belongs to whoever is opening the world, which is main.c. The gap between those two
// is exactly where GENVER_STAMP_FAILED shipped: 82d4a1e added the status, proved it with a
// red/green test, and nothing routed it, because main.c gated the refusal on
//
//     if (gv == GENVER_TOO_NEW || gv == GENVER_DAMAGED)
//
// — an `if` naming two statuses by hand. A third status added later falls straight through an
// expression like that, silently, and the world loads anyway. There is no warning for it and no
// test that can see it, because main.c cannot be linked into a host binary (see app/session_test.c
// for the same problem stated one module over).
//
// So the predicate moves out of main.c into a switch over the WHOLE enum with no default. That
// makes the same mistake a compile error instead: -Wswitch is in -Wall, the host suite and the
// console build both run -Werror, and a new GENVER_* added to genversion.h without a decision
// here stops the build rather than shipping an unrefused world. That is the actual fix — the
// routing in main.c is only correct today, this is what keeps it correct.
//
// A header of static inlines rather than a .c file because there is no state and no I/O here,
// only a total function over five enumerators; and because a .c would have to be added to every
// link line that already compiles genversion.c, which is a build-script change for a function
// the compiler will inline anyway. world/world_test.c includes it and tests it directly.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "world/genversion.h"

// What the player is told when a world will not open, or NULL when the status is not a refusal
// at all. NULL is the "carry on" answer for both non-refusals, and they are spelled out as
// separate cases rather than folded into a default for the reason in the file comment.
//
// The strings are short because the place they land is short: they go on scene/title.c's status
// line, which is a char[48] (TitleState.status) drawn at scale 1 into 320 px — 53 characters
// wide at gfx/font.h's FONT_ADVANCE of 6. Every message below is 29 characters, which is the
// length of the two error lines title.c already shows there ("world create failed (sd write)",
// "bad name: use A-Z 0-9 _ - only"), so nothing here needs new layout to fit.
//
// Three different sentences rather than one shared "world refused", because the three faults ask
// the player for three different things and genversion.h's comment on GENVER_STAMP_FAILED makes
// that the reason the status exists at all: a newer build is something they update to get, a
// damaged version file is something they can only lose, and a card that would not take the write
// is something they can fix on the spot — free space, take the write-lock off, reseat it.
static inline const char* genVersionRefusalText(GenVersionStatus st)
{
	switch (st) {
	// A generator this build can run. Nothing to say and nothing to stop.
	case GENVER_OK:           return NULL;

	// No world directory: a joined server session, which has no stamp to disagree with by
	// design (genversion.h's genVersionForSession). Not a fault, so not a refusal.
	case GENVER_NO_WORLD_DIR: return NULL;

	case GENVER_TOO_NEW:      return "world made by a newer version";
	case GENVER_DAMAGED:      return "world version file is damaged";
	case GENVER_STAMP_FAILED: return "world stamp failed (sd write)";
	}

	// Not reachable through the enum, and deliberately AFTER the switch rather than as a
	// `default:` inside it — a default would satisfy -Wswitch and take away the whole point of
	// the file. This is only here for a value cast in from outside the type, where refusing is
	// the wrong answer: an unknown status is not evidence that the world is bad.
	return NULL;
}

// The gate itself. One caller (main.c's genStart) and one rule: a world with a refusal text is a
// world that must not be entered.
static inline bool genVersionRefuses(GenVersionStatus st)
{
	return genVersionRefusalText(st) != NULL;
}
