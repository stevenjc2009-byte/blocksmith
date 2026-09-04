// v1.7.1 task 48b. See debug/loadprof.h for what this measures and why it is not part of the
// frame CSV ring.
//
// _POSIX_C_SOURCE before every include, because the host suite compiles with -std=c11 rather
// than -std=gnu11 and clock_gettime/CLOCK_MONOTONIC are hidden behind the POSIX feature test
// in that mode. The console build never sees this line's effect — it takes the __3DS__ branch.
#ifndef __3DS__
#define _POSIX_C_SOURCE 200809L
#endif

#include "debug/loadprof.h"

#include <stdio.h>
#include <string.h>
#include <time.h>   // time() — for loadprofWrite's unix_time_s column, both platforms

#include "version.h"   // BLOCKSMITH_VERSION / BLOCKSMITH_VERSION_SET, for loadprofWrite's row

#ifdef __3DS__
#include <3ds.h>
#endif
// clock_gettime/CLOCK_MONOTONIC (nowTicks' host arm) come from the unconditional <time.h>
// above, which was added for loadprofWallSeconds — time() and clock_gettime() share a header.

static LoadProfile s_p;

// Armed/disarmed by Begin/End. Not volatile and not atomic: the main thread is the only writer,
// and the worker only ever reads it — through loadprofMark — at points where the worst a stale
// read can do is charge one already-running bracket to a profile that has just finished, or
// drop one that has just started. Both are a single column's worth of a stage that is reported
// with its call count beside it. A lock here would cost more than the thing it protects.
static int s_active;

double loadprofTicksPerUs(void)
{
#ifdef __3DS__
	// SYSCLOCK_ARM11 is 268111856 Hz. Kept as a double so the division is done once, at report
	// time, rather than losing a fraction of a tick on every one of the thousands of brackets.
	return (double)SYSCLOCK_ARM11 / 1000000.0;
#else
	return 1000.0;   // the host tick is a nanosecond
#endif
}

static uint64_t nowTicks(void)
{
#ifdef __3DS__
	return svcGetSystemTick();
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

void loadprofReset(void)
{
	memset(&s_p, 0, sizeof s_p);
	s_active = 0;
}

void loadprofBegin(const char* world, int reloaded)
{
	loadprofReset();

	s_p.reloaded = reloaded ? 1 : 0;
	if (world) {
		// Copied by hand rather than with strncpy for the same reason metrics.c's
		// metricsSetSelfTest does it: the truncation is deliberate and strncpy's is the kind
		// gcc warns about under -Werror.
		size_t i = 0;
		for (; i + 1 < sizeof s_p.world && world[i] != '\0'; i++) s_p.world[i] = world[i];
		s_p.world[i] = '\0';
	}

	// Last, so the wall clock starts after the bookkeeping rather than including it, and so no
	// bracket can be opened against a half-initialised profile.
	s_p.wall_ticks = nowTicks();
	s_active = 1;
}

void loadprofSetReloaded(int reloaded)
{
	if (!s_active) return;
	s_p.reloaded = reloaded ? 1 : 0;
}

void loadprofEnd(void)
{
	if (!s_active) return;
	s_active = 0;
	s_p.wall_ticks = nowTicks() - s_p.wall_ticks;
}

int loadprofActive(void) { return s_active; }

uint64_t loadprofMark(void)
{
	if (!s_active) return 0;

	const uint64_t t = nowTicks();
	// A clock that legitimately reads 0 would be indistinguishable from "not armed" and the
	// bracket would be silently dropped. Cannot happen on either platform in practice — the
	// console tick counts from power-on and CLOCK_MONOTONIC from boot or the epoch — but one
	// tick of skew costs nothing and removes the case entirely.
	return t ? t : 1;
}

void loadprofSince(LoadStage s, uint64_t mark)
{
	if (!s_active || mark == 0) return;
	if ((unsigned)s >= (unsigned)LOAD_STAGE_COUNT) return;

	const uint64_t t = nowTicks();
	// Guarded rather than assumed: on the host CLOCK_MONOTONIC is monotone, on the console the
	// tick is a free-running 64-bit counter, so this cannot go backwards — but an underflow
	// here would add roughly 2^64 ticks to a stage and make the whole report unreadable, and
	// the test is one compare.
	//
	// ── v1.8.8: atomic, because a stage can now have TWO writers ──────────────────────
	//
	// loadprof.h's thread-safety argument was "each slot has exactly one writer": region_io,
	// decode, generate and light on the worker thread and everything else on the main thread.
	// The second generator lane (app/worker.c, New 3DS only) breaks that — those four stages
	// are now written by both lanes — and `+=` on a uint64_t is a load, an add and a store,
	// which on this 32-bit part is TWO stores. Two of them interleaved does not merely lose an
	// accumulation: it can publish a torn 64-bit value and report a `generate` figure that is
	// wrong by billions of ticks, which is worse than no instrument at all.
	//
	// RELAXED because these are accumulators with no ordering relationship to anything: the
	// only requirement is that each add lands exactly once and no half-value is ever visible,
	// and the reader (loadprofEnd, main thread) runs after every lane is parked with nothing
	// outstanding. On ARM11 __atomic_fetch_add on a uint64_t lowers to an ldrexd/strexd loop,
	// so it is a handful of instructions on a path that already read the system tick twice.
	if (t > mark) __atomic_fetch_add(&s_p.ticks[s], t - mark, __ATOMIC_RELAXED);
	__atomic_fetch_add(&s_p.calls[s], 1, __ATOMIC_RELAXED);
}

void loadprofFrame(void)
{
	if (!s_active) return;
	s_p.frames++;
}

const LoadProfile* loadprofGet(void) { return &s_p; }

// Index-matched to the LoadStage enum. A stage added without a name here would read off the end
// of this table, so the count is asserted at the one place that walks it (loadprofStageName).
static const char* const kStageName[LOAD_STAGE_COUNT] = {
	"worlddir", "played", "setup", "region_io", "decode", "generate",
	"light", "install", "queue", "mesh", "present",
};

const char* loadprofStageName(LoadStage s)
{
	if ((unsigned)s >= (unsigned)LOAD_STAGE_COUNT) return "?";
	return kStageName[s];
}

// The four stages loadprofSince's v1.8.8 comment describes as having two writers (the New 3DS's
// second generator lane, plus the main thread's own bracket around the same call site). Their
// ms is CPU time summed across up to three threads, not wall time, so it can legitimately run
// past wall_ms — loadprofFormat uses this to withhold a "% of wall" figure that would otherwise
// print a number over 100% and read as a bug.
static int loadprofStageIsConcurrent(LoadStage s)
{
	return s == LOAD_STAGE_REGION_IO || s == LOAD_STAGE_DECODE ||
	       s == LOAD_STAGE_GENERATE  || s == LOAD_STAGE_LIGHT;
}

static double ticksToMs(uint64_t t)
{
	return (double)t / (loadprofTicksPerUs() * 1000.0);
}

int loadprofFormat(char* buf, size_t cap)
{
	if (!buf || cap == 0) return 0;

	const double wall_ms = ticksToMs(s_p.wall_ticks);
	uint64_t sum = 0;
	for (int i = 0; i < LOAD_STAGE_COUNT; i++) sum += s_p.ticks[i];

	// `len` is bytes actually placed, never snprintf's return. snprintf reports the length it
	// WOULD have written, so accumulating it directly overshoots the moment anything truncates
	// — measured at 683 against a 640-byte buffer — and this function's return value is
	// documented in loadprof.h as "the length written", which is exactly the number a caller
	// would hand to fwrite. A negative return is worse still: `buf + n` would index backwards.
	// Same saturating shape as app/crash.c's dumpAppend and app/watchdog.c's gxAppend.
	size_t len = 0;

	int n = snprintf(buf, cap, "load %s (%s) %.1f ms over %lu frames\n",
	                 s_p.world[0] ? s_p.world : "?",
	                 s_p.reloaded ? "reloaded" : "fresh",
	                 wall_ms, (unsigned long)s_p.frames);
	if (n > 0) len = ((size_t)n < cap) ? (size_t)n : cap - 1;

	for (int i = 0; i < LOAD_STAGE_COUNT && len + 400 < cap; i++) {
		const double ms  = ticksToMs(s_p.ticks[i]);
		const double per = s_p.calls[i] ? ms * 1000.0 / (double)s_p.calls[i] : 0.0;
		int m;
		if (loadprofStageIsConcurrent((LoadStage)i)) {
			// No "% of wall" here — see loadprofStageIsConcurrent. A percentage that can read
			// over 100% is worse than no percentage, so the column prints the literal "cpu"
			// instead: ms and us/call are still real and still comparable to this stage's own
			// history, just not to wall_ms.
			m = snprintf(buf + len, cap - len, "  %-9s %9.3f ms %6s n=%-6lu %8.1f us/call\n",
			            kStageName[i], ms, "cpu", (unsigned long)s_p.calls[i], per);
		} else {
			const double pct = wall_ms > 0.0 ? ms * 100.0 / wall_ms : 0.0;
			m = snprintf(buf + len, cap - len,
			            "  %-9s %9.3f ms %5.1f%% n=%-6lu %8.1f us/call\n",
			            kStageName[i], ms, pct, (unsigned long)s_p.calls[i], per);
		}
		if (m <= 0) break;
		const size_t room = cap - len - 1;
		len += ((size_t)m < room) ? (size_t)m : room;
	}

	if (len + 1 < cap) {
		// Wall minus the stages. On a vsynced loading screen this is the VBlank wait the
		// present stage's own C3D_FrameBegin is blocked in, plus whatever the frame loop does
		// outside every bracket; it is printed rather than hidden because a load that is
		// frame-bound rather than work-bound is exactly the answer it gives. Renamed from
		// "other" to "unaccounted" to match the CSV column (loadprofWrite) — see loadprof.h for
		// why this is expected to be NEGATIVE whenever a New 3DS's worker lanes overlap the
		// main thread's own stages, and why that is correct rather than a broken timer.
		const double unaccounted = wall_ms - ticksToMs(sum);
		const int m = snprintf(buf + len, cap - len, "  %-9s %9.3f ms %5.1f%%\n", "unaccounted",
		                       unaccounted, wall_ms > 0.0 ? unaccounted * 100.0 / wall_ms : 0.0);
		if (m > 0) {
			const size_t room = cap - len - 1;
			len += ((size_t)m < room) ? (size_t)m : room;
		}
	}

	return (int)len;
}

// time(NULL)'s documented failure value, checked rather than assumed for the same reason
// world/worldseed.c's worldSeedMint checks it: on a console with a dead or unset RTC, time()
// answering -1 is a real state, not a hypothetical one. Reported as 0 — a wall-clock reading
// of 0 (1970) is not otherwise reachable on either platform, so it reads unambiguously as "no
// wall clock available" rather than as a bogus early timestamp that could be mistaken for one.
static uint64_t loadprofWallSeconds(void)
{
	const time_t t = time(NULL);
	return (t == (time_t)-1) ? 0 : (uint64_t)t;
}

void loadprofWrite(const char* path)
{
	if (!path || s_p.wall_ticks == 0) return;

	// Header only when the file is not there yet. Asked by opening for read rather than by
	// looking at the append handle's offset, which is not portable enough to rely on for a
	// diagnostic that has to be right the first time it runs on a card.
	//
	// This never has to reconcile with an OLDER header: LOADPROF_PATH itself carries the
	// schema version (see loadprof.h), so a file this function finds already sitting at `path`
	// was written by THIS schema or not at all — never by the pre-version/timestamp code, which
	// wrote a different filename. There is deliberately no code here that inspects an existing
	// file's header and decides what to do about a mismatch; see loadprof.h for why that
	// approach (and rename()-based rotation) was rejected in favour of the filename split.
	FILE* probe = fopen(path, "rb");
	const int have = probe != NULL;
	if (probe) fclose(probe);

	FILE* f = fopen(path, "ab");
	if (!f) return;

	if (!have) {
		fputs("version,unix_time_s,world,reloaded,wall_ms,frames", f);
		for (int i = 0; i < LOAD_STAGE_COUNT; i++)
			fprintf(f, ",%s_ms,%s_n", kStageName[i], kStageName[i]);
		// unaccounted_ms, not other_ms: see loadprof.h for why this is expected to be
		// negative under the parallel loader and is not a broken timer.
		fputs(",unaccounted_ms\n", f);
	}

	uint64_t sum = 0;
	for (int i = 0; i < LOAD_STAGE_COUNT; i++) sum += s_p.ticks[i];

	fprintf(f, "%s,%lu,%s,%d,%.3f,%lu",
	        BLOCKSMITH_VERSION_SET ? BLOCKSMITH_VERSION : "(unset)",
	        (unsigned long)loadprofWallSeconds(),
	        s_p.world[0] ? s_p.world : "?", s_p.reloaded,
	        ticksToMs(s_p.wall_ticks), (unsigned long)s_p.frames);
	for (int i = 0; i < LOAD_STAGE_COUNT; i++)
		fprintf(f, ",%.3f,%lu", ticksToMs(s_p.ticks[i]), (unsigned long)s_p.calls[i]);
	fprintf(f, ",%.3f\n", ticksToMs(s_p.wall_ticks) - ticksToMs(sum));

	fclose(f);
}
