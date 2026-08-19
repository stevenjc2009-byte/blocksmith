// Crash handler dumping to SD (step 8.5).
//
// The console this game ships on has no attached debugger and no way to read a log after the
// fact — closing Azahar keeps its console output, closing a real 3DS after a crash keeps
// nothing. If a build faults on real hardware the only evidence that will ever exist is
// whatever this module manages to get onto the SD card in the instant before the system's
// own crash screen takes over, so that is the whole job: catch the fault, write down enough
// to diagnose it, then get out of the way.
//
// ── How the catch actually works ──────────────────────────────────────────────────────
//
// libctru exposes this through a per-thread TLS slot rather than a signal or a callback list:
// thread.h's threadOnException() pokes a handler pointer, a handler stack and a data buffer
// straight into the calling thread's TLS, and the ARM11 kernel reads that slot when the
// thread it belongs to takes a CPU exception (prefetch abort, data abort, undefined
// instruction, or a VFP exception — see ERRF_ExceptionType in 3ds/errf.h). There is no
// "register a handler for the process"; this project has one thread that matters for this
// (the main thread), so crashInit() sets that thread's slot once and that is the whole
// installation.
//
// The chain on a real fault is: kernel sees the exception -> reads this thread's TLS ->
// jumps to crashHandler() on the stack we gave it, with the exception info and the register
// dump already sitting in the buffer we gave it -> crashHandler writes the SD dump -> hands
// off to libctru's own ERRF_ExceptionHandler(), which is what actually puts the "the software
// has crashed" screen up and reboots. Skipping that last step would leave the console sitting
// on a frozen frame with no signal to the player that anything went wrong at all.
//
// ── Why every buffer here is static and every call is the low-level one ──────────────────
//
// The thread that is about to run crashHandler() is, by definition, one that just executed an
// instruction the CPU could not complete — its own stack may be the very thing that overflowed
// or got corrupted, and its heap (if it was mid-malloc, mid-free, or the allocator's own
// bookkeeping was what got clobbered) cannot be trusted either. So:
//
//   - The handler never runs on the faulting thread's own stack. crashInit() hands
//     threadOnException a small static array instead of RUN_HANDLER_ON_FAULTING_STACK, so a
//     stack overflow is exactly the kind of fault this can still report on.
//   - The exception data is written by the kernel into a static struct we own, not pushed
//     onto whichever stack (WRITE_DATA_TO_HANDLER_STACK / WRITE_DATA_TO_FAULTING_STACK), for
//     the same reason.
//   - The dump text is built in a fixed-size static buffer with plain snprintf-family calls,
//     never malloc — which rules out stdio's fopen/fwrite for the write itself, because
//     newlib's fopen allocates the FILE structure on the heap. crash.c reaches the SD card
//     through the raw FSUSER_*/FSFILE_* calls instead (3ds/services/fs.h), which only ever
//     touch the caller's own stack and the kernel's IPC command buffer.
//   - The work is kept to one pass: format the text, open, (re)size, write, close. No retry
//     loops, no directory listing, nothing that could itself fault a second time inside a
//     handler that is not expecting to be re-entered.
//
// crashLastDumpExists() and crashClearDump() are not part of any of the above — they run in
// the ordinary, unbroken process, before or after a crash has happened, so they use plain
// stdio like the rest of the game's save code (world/region.c) rather than the raw FS calls.
//
// ── Timestamping ───────────────────────────────────────────────────────────────────────
//
// There is deliberately no timestamp written into the dump. A trustworthy one would mean
// calling into the RTC-backed C library clock or the cfg/ptm services from inside the
// handler — more IPC, on a session that may itself be the thing that faulted — for a value
// that, even if it worked, would only be wall-clock time of day and would not identify which
// boot or which build produced the crash. FS_WRITE_UPDATE_TIME is passed on the write instead,
// which lets the filesystem itself stamp the file's mtime using the OS's own clock rather than
// this code reaching for one.
//
// ── Overwrite, not append ─────────────────────────────────────────────────────────────
//
// Each crash replaces the previous dump rather than being appended after it. The API this
// step asks for — crashLastDumpExists() / crashClearDump() — is already named for a single
// "did the last run crash" fact, not a history, and an ever-growing file across a
// development session's worth of repeated crashes is exactly the kind of unbounded work
// requirement 5 above rules out: appending safely means finding out how big the file already
// is first, which is an extra IPC round trip a broken process does not need to make when
// simply re-creating the file does the same job in fewer steps.
#pragma once

#include <stdbool.h>

// Installs the exception handler on the calling thread (call this from the main thread, as
// early in main() as possible — it has no dependency on gfx, fs, or anything else being up
// yet, so there is no reason to wait). Safe to call more than once; each call just re-writes
// the same TLS slot with the same values.
void crashInit(void);

// libctru has no "unregister" call for threadOnException — the TLS slot simply stops
// mattering once the thread exits, which for the main thread is process exit. This exists so
// crash.c has the same init/exit shape as every other app/ module and so a future handler
// (e.g. one that also wants to flush something on the way out) has somewhere to go; today it
// does nothing.
void crashExit(void);

// True if sdmc:/blocksmith/crash.txt exists, i.e. the previous run ended in a crash this
// module caught. Meant for the title screen to check once at boot, per requirement 7 —
// nothing here decides what to do with the answer.
bool crashLastDumpExists(void);

// Deletes sdmc:/blocksmith/crash.txt if present. Not treated as an error if there was nothing
// to delete — "no dump" is the normal state of a game that has not crashed.
void crashClearDump(void);
