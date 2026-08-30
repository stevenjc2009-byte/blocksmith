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
	// {back, connected, have_world_seed, registry_waiting, gen_waiting}. v1.8.3 Phase 4 added
	// the fifth member, and every positional initialiser in this file names it rather than
	// letting it zero-fill: -Wextra's -Wmissing-field-initializers plus this suite's -Werror
	// makes a short initialiser a build failure, and a frame here is meant to read as the
	// complete set of conditions anyway.
	TitleMpNav in = {false, true, true, false, false};
	return in;
}

// Nothing is asked for and nothing is ready: the baseline that proves the two outputs are not
// simply stuck on.
static void testAnIdleFrameNavigatesNowhere(void)
{
	TitleMpNav in = {false, false, false, false, false};
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

	// v1.8.3 Phase 4. The fourth condition, load-bearing in the same way.
	TitleMpNav still_asking = readyFrame();
	still_asking.gen_waiting = true;
	CHECK(!titleMpNav(still_asking).start_server);
}

// v1.8.3 Phase 4, frame (i) of three. Everything the pre-Phase-4 gate asked for is true —
// joined, seed known, registry settled — and BS_APP_WORLD_GEN has not arrived yet. This is the
// one-frame window between WORLD_INFO and the WORLD_GEN sent immediately behind it, and it must
// NOT enter.
//
// The reason it matters is that entering here does not fail; it succeeds at the wrong thing.
// networldServerGenVersion() would answer false, world/genversion.h's
// genVersionForSessionResolve() would read that as "the server did not say" and hand back
// GEN_VERSION_LEGACY, and the session would generate legacy terrain against a server that was
// one datagram away from declaring something else. No error, no log line, and the mismatch
// refusal cannot fire because the client never learned there was a mismatch.
static void testEntryWaitsForTheGeneratorAnswer(void)
{
	TitleMpNav in = readyFrame();
	in.gen_waiting = true;

	TitleMpNavOut out = titleMpNav(in);
	CHECK(!out.start_server);
	CHECK(!out.leave_to_main);
}

// Frame (ii): WORLD_GEN arrived, so networldGenWaiting() went false and the gate opens. Written
// as a state change from frame (i) rather than as another call on readyFrame(), so that what is
// being checked is the transition and not a second copy of the open-gate case above.
//
// Frame (iii) — the grace expired against a pre-v1.8.3 server that will never send WORLD_GEN —
// is deliberately the SAME check and not a separate one. networldGenWaiting() is a single bool
// and titleMpNav() cannot tell which arm released it, exactly as it cannot for
// networldRegistryWaiting(); writing frame (iii) out separately would be this assertion typed
// twice and would suggest a distinction the type does not carry. That an old server is let in
// at all is networld.c's guarantee (every arm of networldGenWaiting() is bounded) and is
// checked in net/networld_test.c against a fake clock, which is where the clock is.
static void testEntryOpensOnceTheGeneratorAnswerSettles(void)
{
	TitleMpNav in = readyFrame();
	in.gen_waiting = true;
	CHECK(!titleMpNav(in).start_server);

	in.gen_waiting = false;
	TitleMpNavOut out = titleMpNav(in);
	CHECK(out.start_server);
	CHECK(!out.leave_to_main);
}

// The Phase 4 member joins the same race every other member of this struct is already in: BACK
// held down on the exact frame the wait clears. Leaving still wins, for the reason title_nav.h
// gives — the player asked to leave, the gate merely stopped being closed.
static void testBackOnTheFrameTheGeneratorAnswerArrivesDoesNotEnterTheWorld(void)
{
	TitleMpNav in = readyFrame();
	in.back        = true;
	in.gen_waiting = false;

	TitleMpNavOut out = titleMpNav(in);
	CHECK(out.leave_to_main);
	CHECK(!out.start_server);
}

// Backing out of a screen the player is merely SITTING on: nothing else is true, so this only
// proves BACK is wired at all.
static void testBackFromAnIdleScreenLeaves(void)
{
	TitleMpNav in = {true, false, false, false, false};
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
	TitleMpNav in = {true, true, true, false, false};
	TitleMpNavOut out = titleMpNav(in);
	CHECK(out.leave_to_main);
	CHECK(!out.start_server);
}

// The two outputs are mutually exclusive over the whole input space, not just the frames above.
// Thirty-two combinations (sixteen before v1.8.3 Phase 4 added gen_waiting) is small enough to
// check exhaustively rather than argue about.
static void testLeavingAndEnteringAreNeverBothTrue(void)
{
	for (int bits = 0; bits < 32; bits++) {
		TitleMpNav in;
		in.back             = (bits & 1) != 0;
		in.connected        = (bits & 2) != 0;
		in.have_world_seed  = (bits & 4) != 0;
		in.registry_waiting = (bits & 8) != 0;
		in.gen_waiting      = (bits & 16) != 0;

		TitleMpNavOut out = titleMpNav(in);
		CHECK(!(out.leave_to_main && out.start_server));

		// BACK always leaves, whatever else is true — the screen must never become one the
		// player cannot get out of.
		CHECK(out.leave_to_main == in.back);

		// v1.8.3 Phase 4. The entry gate written out as its whole predicate, over the whole
		// input space, so that dropping ANY term from title_nav.c reddens here and not only in
		// whichever hand-written frame happens to name that term. Written as an equality rather
		// than an implication on purpose: an implication would still pass against a gate that
		// had stopped opening at all.
		CHECK(out.start_server == (!in.back
		                           && in.connected
		                           && in.have_world_seed
		                           && !in.registry_waiting
		                           && !in.gen_waiting));
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
	testEntryWaitsForTheGeneratorAnswer();
	testEntryOpensOnceTheGeneratorAnswerSettles();
	testBackOnTheFrameTheGeneratorAnswerArrivesDoesNotEnterTheWorld();
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
