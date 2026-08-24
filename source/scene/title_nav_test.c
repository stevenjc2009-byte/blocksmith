// Host self-test for scene/title_nav.c — the multiplayer screen's navigation decision, pulled
// out of scene/title.c precisely so it could be checked here instead of by pressing B on a
// console at the right millisecond (see title_nav.h's file comment). Self-contained (its own
// main()), same shape and same reason as scene/ui_layout_test.c.
//
// The CHECK macro and the PASS/FAIL summary line are copied from scene/ui_layout_test.c, which
// copied them from world/inventory_test.c — same shape everywhere so a failure here reads the
// same way a failure anywhere else in this project does.
//
// The __3DS__ guard around the *whole file* is load-bearing, not tidy — copied verbatim from
// ui_layout_test.c's own comment on this: the Makefile globs every .c under source/scene into
// the console build, so without the guard this file's main() links against source/main.c's and
// the build dies with "multiple definition of `main'".
#ifndef __3DS__

#include <stdio.h>

#include "scene/title_nav.h"

static int  s_checks;
static int  s_fails;
static char s_first[160];

#define CHECK(cond) do {                                                       \
		s_checks++;                                                             \
		if (!(cond)) {                                                          \
			s_fails++;                                                          \
			/* Printed per failure, not just the first: this project's own test    \
			 * standard is that every case of a red run gets read, and a summary   \
			 * naming one of four hides the other three. */                        \
			printf("  FAIL  L%d %s\n", __LINE__, #cond);                        \
			if (!s_first[0])                                                    \
				snprintf(s_first, sizeof(s_first), "L%d %.140s", __LINE__, #cond);  \
		}                                                                        \
	} while (0)

// A frame in which the entry gate is wide open: joined, seed known, registry settled. Every
// case below varies exactly one thing away from this, so a check that goes green can only have
// gone green for the reason it names.
static TitleMpNav readyFrame(void)
{
	TitleMpNav in = {false, true, true, false};
	return in;
}

// Nothing is asked for and nothing is ready: the baseline that proves the two outputs are not
// simply stuck on.
static void testAnIdleFrameNavigatesNowhere(void)
{
	TitleMpNav in = {false, false, false, false};
	TitleMpNavOut out = titleMpNav(in);
	CHECK(!out.leave_to_main);
	CHECK(!out.start_server);
}

// The entry gate itself, unchanged: all three conditions and nothing else lets the player in.
static void testTheEntryGateStillOpensWhenItShould(void)
{
	TitleMpNavOut out = titleMpNav(readyFrame());
	CHECK(out.start_server);
	CHECK(!out.leave_to_main);

	// Each condition on its own is load-bearing.
	TitleMpNav no_session = readyFrame();
	no_session.connected = false;
	CHECK(!titleMpNav(no_session).start_server);

	// This is the DISCONNECT case, and the model the fix below is built on: netDisconnect()
	// reaches networldInit(), which clears the seed, so the seed is already false on the frame
	// the button was pressed. Nothing had to be added for DISCONNECT to be safe.
	TitleMpNav no_seed = readyFrame();
	no_seed.have_world_seed = false;
	CHECK(!titleMpNav(no_seed).start_server);

	TitleMpNav still_syncing = readyFrame();
	still_syncing.registry_waiting = true;
	CHECK(!titleMpNav(still_syncing).start_server);
}

// Backing out of a screen the player is merely SITTING on: nothing else is true, so this only
// proves BACK is wired at all.
static void testBackFromAnIdleScreenLeaves(void)
{
	TitleMpNav in = {true, false, false, false};
	TitleMpNavOut out = titleMpNav(in);
	CHECK(out.leave_to_main);
	CHECK(!out.start_server);
}

// The defect. "Joined - syncing block table..." is on screen and the UI is explicitly inviting
// the player to sit through a wait of up to NETWORLD_REG_SYNC_DEADLINE_MS (2000 ms) — so giving
// up on it is a thing the player is being invited to do, across up to two seconds of frames.
// On the single frame the last BS_APP_REGISTRY_DEFS batch lands, or the frame the deadline
// expires, networldRegistryWaiting() flips to false while BACK is down. Leaving must win: the
// player asked to leave, the gate merely stopped being closed.
static void testBackOnTheFrameTheSyncCompletesDoesNotEnterTheWorld(void)
{
	TitleMpNav in = readyFrame();
	in.back = true;

	TitleMpNavOut out = titleMpNav(in);
	CHECK(out.leave_to_main);
	CHECK(!out.start_server);
}

// The same frame reached the other way: the deadline expired rather than the table arriving.
// Identical inputs by construction — networldRegistryWaiting() is one bool and the caller
// cannot tell which arm released it — so this is the same rule stated from the case that made
// the window two seconds wide rather than one frame.
static void testBackOnTheFrameTheSyncDeadlineExpiresDoesNotEnterTheWorld(void)
{
	TitleMpNav in = readyFrame();
	in.back            = true;
	in.registry_waiting = false;

	TitleMpNavOut out = titleMpNav(in);
	CHECK(out.leave_to_main);
	CHECK(!out.start_server);
}

// And the older, one-frame-wide version of the same window, which existed before the registry
// work: BACK on the frame BS_APP_WORLD_INFO's seed arrives, with no registry wait involved.
static void testBackOnTheFrameTheSeedArrivesDoesNotEnterTheWorld(void)
{
	TitleMpNav in = {true, true, true, false};
	TitleMpNavOut out = titleMpNav(in);
	CHECK(out.leave_to_main);
	CHECK(!out.start_server);
}

// The two outputs are mutually exclusive over the whole input space, not just the frames above.
// Sixteen combinations is small enough to check exhaustively rather than argue about.
static void testLeavingAndEnteringAreNeverBothTrue(void)
{
	for (int bits = 0; bits < 16; bits++) {
		TitleMpNav in;
		in.back             = (bits & 1) != 0;
		in.connected        = (bits & 2) != 0;
		in.have_world_seed  = (bits & 4) != 0;
		in.registry_waiting = (bits & 8) != 0;

		TitleMpNavOut out = titleMpNav(in);
		CHECK(!(out.leave_to_main && out.start_server));

		// BACK always leaves, whatever else is true — the screen must never become one the
		// player cannot get out of.
		CHECK(out.leave_to_main == in.back);
	}
}

int main(void)
{
	testAnIdleFrameNavigatesNowhere();
	testTheEntryGateStillOpensWhenItShould();
	testBackFromAnIdleScreenLeaves();
	testBackOnTheFrameTheSyncCompletesDoesNotEnterTheWorld();
	testBackOnTheFrameTheSyncDeadlineExpiresDoesNotEnterTheWorld();
	testBackOnTheFrameTheSeedArrivesDoesNotEnterTheWorld();
	testLeavingAndEnteringAreNeverBothTrue();

	if (s_fails == 0)
		printf("title_nav self-test: PASS  %d checks\n", s_checks);
	else
		printf("title_nav self-test: FAIL %d/%d  %s\n", s_fails, s_checks, s_first);

	return s_fails ? 1 : 0;
}

#else

// Console build: nothing here. An empty translation unit is not valid ISO C, so give the
// compiler one declaration to chew on rather than let -Wpedantic complain about the guard.
typedef int title_nav_test_host_only_t;

#endif   // !__3DS__
