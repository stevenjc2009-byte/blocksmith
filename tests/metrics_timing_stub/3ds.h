// Host stub for tests/metrics_timing_test.c: the smallest <3ds.h> that source/debug/metrics.c
// compiles against. This is not a general-purpose libctru stub — it names exactly the surface
// metrics.c and metrics.h reference and nothing more. The real libctru <3ds.h> pulls in ARM/
// thumb intrinsics and is not compilable by a host gcc, which is why metrics.c has never been
// linkable into any binary in tools/run_host_tests.sh before this stanza: every other file that
// touches libctru either has no <3ds.h> at all, or keeps its libctru half behind an
// `#ifdef __3DS__` seam the way source/app/hw.c and source/gfx/weatherdraw.c do. metrics.c has
// no such seam — the whole file, including the CSV writer thread and every accessor, depends on
// <3ds.h> and <citro3d.h> unconditionally — so the only way to host-test it at all is to stand
// the real translation unit up against a stand-in header, the same way a hardware test stands a
// real board up against a bench power supply instead of a simulated one.
//
// The one declaration here that is more than a type or a name is svcGetSystemTick: the test
// file supplies its OWN definition of it (never this header), because owning the clock is what
// lets a frame be handed an exact number of milliseconds of work and of waiting. See the long
// comment at the top of tests/metrics_timing_test.c for why that is the whole point of the
// suite.
//
// Original shape: METRICS-VISIBLE's scratchpad harness (metricsvis_stub/3ds.h), which proved
// this approach standalone before this file promoted it into the tree. Nothing here re-derives
// metrics.c's logic; it only supplies the libctru names metrics.c and metrics.h use so the real
// metrics.c compiles unmodified.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t  s32;
typedef int64_t  s64;

#define U64_MAX 0xFFFFFFFFFFFFFFFFull

// libctru: SYSCLOCK_ARM11 / 1000.0, with SYSCLOCK_ARM11 = 268111856. Verified against
// C:/devkitPro/libctru/include/3ds/os.h; tests/metrics_timing_test.c prints the value it is
// using at the top of its own run, so a stale copy of this constant shows up in the very first
// line of output rather than silently skewing every arm's tolerance.
#define CPU_TICKS_PER_MSEC (268111856.0 / 1000.0)

#define CUR_THREAD_HANDLE 0xFFFF8000u

// Driven entirely by the test file: manual-clock mode for arms 1-3, the machine's own
// CLOCK_MONOTONIC for arm 4. Declared here (as the real libctru header would), defined in
// tests/metrics_timing_test.c.
u64 svcGetSystemTick(void);

typedef struct { int dummy; } LightEvent;
#define RESET_ONESHOT 0

void LightEvent_Init(LightEvent* e, int reset_type);
void LightEvent_Wait(LightEvent* e);
void LightEvent_Signal(LightEvent* e);

typedef void* Thread;
Thread threadCreate(void (*fn)(void*), void* arg, size_t stack, int prio, int core, bool detached);
void   threadJoin(Thread t, u64 timeout);
void   threadFree(Thread t);
s32    svcGetThreadPriority(s32* out, u32 handle);

size_t linearSpaceFree(void);

static inline void __dsb(void) { __sync_synchronize(); }
