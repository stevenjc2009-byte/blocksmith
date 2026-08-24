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

#ifdef __3DS__
#include <3ds.h>
#else
#include <time.h>
#endif

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
	if (t > mark) s_p.ticks[s] += t - mark;
	s_p.calls[s]++;
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

	int n = snprintf(buf, cap, "load %s (%s) %.1f ms over %lu frames\n",
	                 s_p.world[0] ? s_p.world : "?",
	                 s_p.reloaded ? "reloaded" : "fresh",
	                 wall_ms, (unsigned long)s_p.frames);

	for (int i = 0; i < LOAD_STAGE_COUNT && n < (int)cap; i++) {
		const double ms  = ticksToMs(s_p.ticks[i]);
		const double pct = wall_ms > 0.0 ? ms * 100.0 / wall_ms : 0.0;
		const double per = s_p.calls[i] ? ms * 1000.0 / (double)s_p.calls[i] : 0.0;
		n += snprintf(buf + n, cap - (size_t)n, "  %-9s %9.3f ms %5.1f%% n=%-6lu %8.1f us/call\n",
		              kStageName[i], ms, pct, (unsigned long)s_p.calls[i], per);
	}

	if (n < (int)cap) {
		// Wall minus the stages. On a vsynced loading screen this is the VBlank wait the
		// present stage's own C3D_FrameBegin is blocked in, plus whatever the frame loop does
		// outside every bracket; it is printed rather than hidden because a load that is
		// frame-bound rather than work-bound is exactly the answer it gives.
		const double other = wall_ms - ticksToMs(sum);
		n += snprintf(buf + n, cap - (size_t)n, "  %-9s %9.3f ms %5.1f%%\n", "other", other,
		              wall_ms > 0.0 ? other * 100.0 / wall_ms : 0.0);
	}

	return n;
}

void loadprofWrite(const char* path)
{
	if (!path || s_p.wall_ticks == 0) return;

	// Header only when the file is not there yet. Asked by opening for read rather than by
	// looking at the append handle's offset, which is not portable enough to rely on for a
	// diagnostic that has to be right the first time it runs on a card.
	FILE* probe = fopen(path, "rb");
	const int have = probe != NULL;
	if (probe) fclose(probe);

	FILE* f = fopen(path, "ab");
	if (!f) return;

	if (!have) {
		fputs("world,reloaded,wall_ms,frames", f);
		for (int i = 0; i < LOAD_STAGE_COUNT; i++)
			fprintf(f, ",%s_ms,%s_n", kStageName[i], kStageName[i]);
		fputs(",other_ms\n", f);
	}

	uint64_t sum = 0;
	for (int i = 0; i < LOAD_STAGE_COUNT; i++) sum += s_p.ticks[i];

	fprintf(f, "%s,%d,%.3f,%lu", s_p.world[0] ? s_p.world : "?", s_p.reloaded,
	        ticksToMs(s_p.wall_ticks), (unsigned long)s_p.frames);
	for (int i = 0; i < LOAD_STAGE_COUNT; i++)
		fprintf(f, ",%.3f,%lu", ticksToMs(s_p.ticks[i]), (unsigned long)s_p.calls[i]);
	fprintf(f, ",%.3f\n", ticksToMs(s_p.wall_ticks) - ticksToMs(sum));

	fclose(f);
}
