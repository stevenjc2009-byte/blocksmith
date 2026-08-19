// See crash.h for the design: why this exists, why every buffer here is static, why the
// write goes through the raw FS calls instead of stdio, and why the dump overwrites rather
// than appends.
#include "app/crash.h"

#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>

// Where the game already puts everything else on the card (app/worker.h, main.c's
// saveWorldDir()). Two spellings because the two contexts need two APIs: crashHandler runs
// after a fault and must use the raw FSUSER_*/FSFILE_* calls (no stdio — see crash.h), which
// take archive-relative paths with no "sdmc:" prefix; crashLastDumpExists/crashClearDump run
// in the ordinary process and use plain stdio like the rest of the save code, which wants the
// full "sdmc:" path.
#define CRASH_DIR_REL     "/blocksmith"
#define CRASH_FILE_REL    "/blocksmith/crash.txt"
#define CRASH_FILE_STDIO  "sdmc:/blocksmith/crash.txt"

// 4 KB for the handler's own execution (formatting the dump text and issuing a handful of FS
// IPC calls) — generous next to worker.c's 32 KB for a full worldgen frame, because this
// thread's locals are a lot smaller: no recursion, no tables, just snprintf calls into a
// buffer that lives outside this stack entirely.
#define CRASH_STACK_BYTES 4096

// How many raw words to read back from the stack pointer at the moment of the fault. Fixed
// and short on purpose — see the comment in dumpBuild() for why this is a peek and not a walk.
#define CRASH_STACK_PEEK_WORDS 16

// The dump's field list (type, fsr/far, pc/lr/sp/cpsr, 13 general registers, 16 stack words,
// a few header/blank lines) comes to well under a kilobyte of text; this leaves several times
// that as margin without the buffer being a meaningful amount of memory on this console.
#define CRASH_DUMP_MAX 2048

// Handler-owned. Never touched by the faulting thread's own stack or heap, per crash.h.
static u8 s_handler_stack[CRASH_STACK_BYTES] __attribute__((aligned(8)));
static ERRF_ExceptionData s_exc_data;

static char   s_dump[CRASH_DUMP_MAX];
static size_t s_dump_len;

static void dumpReset(void)
{
	s_dump_len = 0;
	s_dump[0]  = '\0';
}

// Appends one formatted line (or fragment) to s_dump, saturating rather than overflowing.
// Plain vsnprintf — not malloc-backed on this toolchain — so this is the one string-building
// primitive the handler needs.
static void dumpAppend(const char* fmt, ...)
{
	// Once fewer than 2 bytes remain (room for one char plus the NUL) there is nothing a
	// further call could add; stop asking rather than pay for a vsnprintf whose result is
	// thrown away.
	if (s_dump_len + 1 >= sizeof(s_dump)) return;

	va_list ap;
	va_start(ap, fmt);
	const int n = vsnprintf(s_dump + s_dump_len, sizeof(s_dump) - s_dump_len, fmt, ap);
	va_end(ap);

	// n < 0 is an encoding error and adds nothing. Otherwise vsnprintf reports how many
	// characters it *would* have written, which can exceed the space it actually had —
	// clamp to what it really placed before the NUL it always terminates with.
	if (n <= 0) return;
	const size_t avail = sizeof(s_dump) - s_dump_len - 1;
	s_dump_len += ((size_t)n < avail) ? (size_t)n : avail;
}

static const char* exceptionTypeName(ERRF_ExceptionType t)
{
	switch (t) {
	case ERRF_EXCEPTION_PREFETCH_ABORT: return "PREFETCH_ABORT";
	case ERRF_EXCEPTION_DATA_ABORT:     return "DATA_ABORT";
	case ERRF_EXCEPTION_UNDEFINED:      return "UNDEFINED_INSTRUCTION";
	case ERRF_EXCEPTION_VFP:            return "VFP_EXCEPTION";
	default:                            return "UNKNOWN";
	}
}

// Formats the whole dump into s_dump. Runs on the handler's dedicated stack with the fault
// already captured in *excep/*regs (see crash.h for how those buffers got there), so nothing
// here reads anything the fault itself could have corrupted.
static void dumpBuild(const ERRF_ExceptionInfo* excep, const CpuRegisters* regs)
{
	dumpReset();
	dumpAppend("Blocksmith crash dump\n");
	dumpAppend("(no timestamp: no time source is trusted from inside a fault handler;\n");
	dumpAppend(" see crash.h. the SD card's own write-time stamp is on this file instead.)\n");
	dumpAppend("\n");

	dumpAppend("exception type : %s\n", exceptionTypeName(excep->type));
	dumpAppend("fsr (status)   : 0x%08lX\n", (unsigned long)excep->fsr);
	dumpAppend("far (address)  : 0x%08lX\n", (unsigned long)excep->far);
	dumpAppend("\n");

	dumpAppend("pc   : 0x%08lX\n", (unsigned long)regs->pc);
	dumpAppend("lr   : 0x%08lX\n", (unsigned long)regs->lr);
	dumpAppend("sp   : 0x%08lX\n", (unsigned long)regs->sp);
	dumpAppend("cpsr : 0x%08lX\n", (unsigned long)regs->cpsr);
	dumpAppend("\n");

	for (int i = 0; i < 13; i++)
		dumpAppend("r%-2d  : 0x%08lX\n", i, (unsigned long)regs->r[i]);
	dumpAppend("\n");

	// Not a real unwind — the API hands us sp and nothing that walks frames (libctru's own
	// ERRF_ExceptionHandler produces no backtrace either), and writing an unwinder inside a
	// handler that is meant to do minimal, bounded work is exactly the extra risk crash.h
	// says to avoid. What sp points at right after a fault is ordinary stack memory in the
	// overwhelming majority of crashes — logically stale, not physically unmapped — so a
	// short, fixed run of raw words above it is worth reading. The alignment/non-null check
	// is not a validity proof, just a cheap filter for the two most common "this is not a
	// real stack pointer" cases (a null or a badly misaligned sp), which is the difference
	// between skipping a hopeless read and risking a second fault inside this one.
	dumpAppend("stack (%d words from sp):\n", CRASH_STACK_PEEK_WORDS);
	if (regs->sp != 0 && (regs->sp & 3u) == 0) {
		const u32* sp = (const u32*)regs->sp;
		for (int i = 0; i < CRASH_STACK_PEEK_WORDS; i++)
			dumpAppend("  [sp+0x%02X] 0x%08lX\n", i * 4, (unsigned long)sp[i]);
	} else {
		dumpAppend("  sp looks invalid (0x%08lX), skipped\n", (unsigned long)regs->sp);
	}
}

// Writes s_dump to sdmc:/blocksmith/crash.txt via the raw FS calls (no stdio — see crash.h).
// Every failure here is silent by necessity: there is no console, no log and no second
// attempt worth making from inside a fault handler, and whatever happens next, crashHandler
// still has to hand off to ERRF_ExceptionHandler either way.
static void dumpWriteToSd(void)
{
	FS_Archive archive;
	Result rc = FSUSER_OpenArchive(&archive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""));
	if (R_FAILED(rc)) return;   // no SD card, or FS itself is what is broken

	// Best-effort: the game already creates sdmc:/blocksmith at boot (main.c's
	// saveWorldDir()), so this ordinarily fails with "already exists", which is not told
	// apart from any other failure because there is nothing left to try either way — the
	// open just below is what actually decides whether the dump gets written.
	FSUSER_CreateDirectory(archive, fsMakePath(PATH_ASCII, CRASH_DIR_REL), FS_ATTRIBUTE_DIRECTORY);

	Handle file;
	rc = FSUSER_OpenFile(&file, archive, fsMakePath(PATH_ASCII, CRASH_FILE_REL),
	                      FS_OPEN_WRITE | FS_OPEN_CREATE, 0);
	if (R_SUCCEEDED(rc)) {
		// Truncate to exactly what is about to be written — the overwrite crash.h explains
		// choosing over append. Without this, a shorter dump than the previous one would
		// leave stale bytes trailing it.
		FSFILE_SetSize(file, (u64)s_dump_len);

		u32 written = 0;
		FSFILE_Write(file, &written, 0, s_dump, (u32)s_dump_len,
		             FS_WRITE_FLUSH | FS_WRITE_UPDATE_TIME);
		FSFILE_Close(file);
	}

	FSUSER_CloseArchive(archive);
}

// The handler itself. Installed via threadOnException in crashInit(); never called except by
// the kernel, on the dedicated stack and with the dedicated exception-data buffer that
// crashInit() registered.
static void crashHandler(ERRF_ExceptionInfo* excep, CpuRegisters* regs)
{
	dumpBuild(excep, regs);
	dumpWriteToSd();

	// Must not return (thread.h's ExceptionHandler typedef) — this hands off to libctru's
	// own handler, which is what actually puts up the crash screen and reboots the console.
	// Skipping this would leave the machine sitting on a frozen frame with no signal to
	// whoever is holding it that anything went wrong.
	ERRF_ExceptionHandler(excep, regs);
}

void crashInit(void)
{
	threadOnException(crashHandler, s_handler_stack + sizeof(s_handler_stack), &s_exc_data);
}

void crashExit(void)
{
	// See crash.h: libctru has no call that undoes threadOnException, so there is nothing to
	// tear down. Kept as a real function so this module's shape matches every other app/
	// module's init/exit pair rather than main.c needing to know this one is different.
}

bool crashLastDumpExists(void)
{
	FILE* f = fopen(CRASH_FILE_STDIO, "rb");
	if (!f) return false;
	fclose(f);
	return true;
}

void crashClearDump(void)
{
	// remove() failing because the file was never there is the normal case — a game that
	// has not crashed — and not worth reporting.
	remove(CRASH_FILE_STDIO);
}
