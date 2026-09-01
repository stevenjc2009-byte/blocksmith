#include "app/sleep.h"

// ── The lid-close behaviour, and nothing console-specific ──────────────────────────────
//
// No <3ds.h> above the __3DS__ guard further down, which is the whole point: the host
// suite links THIS file, so the ordering and the budget it checks are the ordering and
// the budget that ship. Until v1.6.0 tools/run_host_tests.sh compiled tests/sleep_test.c
// as a lone translation unit carrying its own hand-copied flag machine, and its 14 checks
// passed no matter what this file did. Same split as app/battery.c and app/debugmenu_ui.c.

static SleepFlushStep s_flush_step;
static SleepLeaveHook s_leave_hook;
static bool           s_asleep;

void sleepSetFlushHook(SleepFlushStep step) { s_flush_step = step; }
void sleepSetLeaveHook(SleepLeaveHook hook) { s_leave_hook = hook; }

bool sleepIsAsleep(void) { return s_asleep; }

int sleepFlushBounded(SleepFlushStep step, SleepClockMs now_ms, unsigned budget_ms)
{
	// A NULL step is the title screen and the world list, where there is no world to save
	// and no worker to save it with. A NULL clock is a caller with no way to stop, and
	// looping on a step that never runs out would hold the console awake indefinitely, so
	// both are refused rather than worked around.
	if (!step || !now_ms) return 0;

	const unsigned start = now_ms();
	int            saved = 0;

	while (step()) {
		saved++;

		// After the write, not before it. Checking first would test a budget nothing had
		// spent yet and let the loop start a column it had no time for; checking here means
		// the overshoot is exactly one column's write, whatever that turns out to cost on
		// real hardware. Unsigned subtraction so a clock that wraps still measures a
		// difference rather than a huge positive number.
		if (now_ms() - start >= budget_ms) break;
	}

	return saved;
}

void sleepOnSleep(SleepClockMs now_ms)
{
	s_asleep = true;

	// The network first, and it is one UDP send. The gateway drops a silent session after
	// BS_SESSION_IDLE_MS — 30 000 ms, deps/blocksmith-server/gateway/bsgate.c:75, enforced
	// by the idle sweep at bsgate.c:1032 which calls game_notify_leave() and frees the slot.
	// A lid that stays shut for longer than half a minute therefore comes back to a session
	// the server has already forgotten, and main.c's own comment above the netStatus() break
	// is emphatic about what that is worth: carrying on would be "a convincing forgery of a
	// shared world".
	//
	// This runs before the flush rather than after it because the flush can spend the whole
	// SLEEP_FLUSH_BUDGET_MS and the Wi-Fi is being torn down around us; the leave is
	// microseconds and should not be queued behind SD writes for the sake of it.
	//
	// It is unconditional — there is no "only leave if the sleep will be a long one" test —
	// because at ONSLEEP nobody knows how long the lid will stay shut, including the
	// player. Deciding on elapsed time would mean measuring it on the wake side and acting
	// a session too late. A short lid-close during multiplayer now costs a rejoin; that is
	// the price of never showing a forged world, and it is the trade main.c already makes
	// everywhere else.
	if (s_leave_hook) s_leave_hook();

	// Then as much of the player's building as fits in the budget. See sleep.h for why the
	// budget is a guess and what would replace it.
	(void)sleepFlushBounded(s_flush_step, now_ms, SLEEP_FLUSH_BUDGET_MS);
}

void sleepOnWake(void)
{
	s_asleep = false;

	// Nothing else, on purpose. The obvious thing to want here is an automatic rejoin, and
	// it is deliberately not done: it would need somewhere to have kept the world the
	// player was in and the route back into it, it would silently reconnect a player who
	// closed the lid precisely to stop playing, and it would do it on a console whose Wi-Fi
	// has only just come back. The leave on the sleep side puts netStatus() somewhere other
	// than NET_CONNECTED, and main.c's existing `if (s_server_session && netStatus() !=
	// NET_CONNECTED) break;` takes it from there through the ordinary teardown and back to
	// the Multiplayer screen — the same path a server timeout already used.
}

// ── APT and the system clock — console build only ──────────────────────────────────────
#ifdef __3DS__

#include <3ds.h>

static aptHookCookie s_hook;

// Same conversion app/battery.c uses for its poll interval. Truncated to unsigned on
// purpose: only differences under a second are ever asked of it, and unsigned subtraction
// gives the right answer across the 32-bit wrap.
static unsigned tickMs(void)
{
	return (unsigned)(svcGetSystemTick() / (SYSCLOCK_ARM11 / 1000));
}

static void onAptHook(APT_HookType hook, void* param)
{
	(void)param;
	switch (hook) {
	case APTHOOK_ONSLEEP:
		sleepOnSleep(tickMs);
		break;
	case APTHOOK_ONWAKEUP:
		sleepOnWake();
		break;
	default:
		break;
	}
	// No osSetSpeedupEnable() in either arm. There used to be — false on sleep, true on
	// wake — and the wake half was the only call to it anywhere in the tree, so the app
	// booted at the clock cia/blocksmith.rsf asks for (CpuSpeed: 268MHz) and then ran a New
	// 3DS at 804 MHz for the rest of the session from the first lid-close onwards. Nothing
	// asked for that, and app/battery.c's tickMs() divides by SYSCLOCK_ARM11 on the
	// assumption that the clock it was told about is the clock it is running at. Setting
	// the CPU speed is not this module's business in either direction: whoever wants
	// speedup should ask for it once, visibly, at boot.
	//
	// v1.8.4: that is now app/hw.c, called from the top of main(). The timing worry above --
	// that battery.c's tickMs() divides by SYSCLOCK_ARM11 and would be wrong at 804 MHz -- was
	// checked and does NOT apply. devkitPro/libctru/include/3ds/os.h:16 documents the constant
	// as "the clock rate for the Arm11 in CTR mode and in svcGetSystemTick"; the separate
	// SYSCLOCK_ARM11_LGR2 (os.h:21) is the *core* clock. The system tick counter runs at
	// SYSCLOCK_ARM11 whatever LGR mode the ARM11 is in, so every tick-to-milliseconds division
	// in the tree stays correct and none of them were touched. The objection to putting the
	// call HERE stands and is why it is not here -- a lid-close is not a decision point.
}

void sleepInit(void)
{
	sleepOnWake();
	aptHook(&s_hook, onAptHook, NULL);
}

void sleepExit(void)
{
	aptUnhook(&s_hook);
	sleepOnWake();
}

#endif  // __3DS__
