// See watchdog.h for why this exists and why the thresholds are what they are.
#include "app/watchdog.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>

#include "version.h"

// Alongside the crash dump and the save data (app/crash.c, main.c's saveWorldDir()). Two
// spellings for the same reason crash.c needs two: the write goes through the raw FS calls,
// which take archive-relative paths, while the "has one been left behind" check on the next
// boot is ordinary stdio.
#define WD_DIR_REL     "/blocksmith"
#define WD_FILE_REL    "/blocksmith/hang.txt"

// How often the monitor looks. 250 ms is fine-grained enough that the recorded stall time is
// accurate to a quarter second, and coarse enough that the thread costs nothing: forty wakeups
// per WD_TIMEOUT_MS, each one comparing two integers.
#define WD_POLL_NS 250000000ULL
#define WD_POLL_MS 250

// The monitor formats one short report and issues a handful of FS calls. crash.c's handler does
// the same work on 4 KB; this thread has the additional snprintf of the report body, so 4 KB
// with nothing recursive in it is the same generous margin.
#define WD_STACK_BYTES 4096

// The draw-bisect build (BS_DRAW_PROBE, see main.c) adds three lines to the report and needs
// the room for them. Left alone in a shipped build: this is a diagnostic's cost, not the
// game's.
#ifndef BS_DRAW_PROBE
#define BS_DRAW_PROBE 0
#endif

#if BS_DRAW_PROBE
#define WD_REPORT_MAX 1280
#else
#define WD_REPORT_MAX 768
#endif

static volatile u32  s_phase;
static volatile u32  s_beat;
static volatile s32  s_columns;
static volatile s32  s_meshes;
static volatile s32  s_queued;
static volatile bool s_worker_busy;

#if BS_DRAW_PROBE
// Sampled by the main thread once per frame and only read here, so this thread never calls
// linearSpaceFree() itself. That matters: those queries take libctru's own heap lock, and a
// watchdog that can block on a lock is a watchdog that writes no report at all — the exact
// failure the raw-FS comment on reportWrite below exists to avoid.
static volatile s32  s_probe_arm = -1;
static volatile u32  s_linear_free;
static volatile u32  s_vram_free;

// Where inside chunkRenderDraw the main thread was last seen. Round 1 of the bisect proved
// the freeze is inside that one call; this is the same question asked at a finer grain, and
// it is asked by a breadcrumb rather than by another six-boot bisect because a breadcrumb
// answers it on the first boot that freezes. Three plain stores per chunk, in a diagnostic
// build only — nothing here is compiled into a shipped one.
//
// s_draw_k is the loop index within a pass and s_draw_slot the mesh slot it was drawing, so
// a freeze does not just name the pass, it names the chunk. -1 means "not in a loop".
static volatile s32  s_draw_stage = -1;
static volatile s32  s_draw_k     = -1;
static volatile s32  s_draw_slot  = -1;

// The draw guard's verdict, already formatted by the main thread into a buffer it owns and
// keeps alive for the rest of the process. A pointer and not a copy because this thread must
// not allocate, must not take a lock, and must not call anything that might: the whole value
// of this file is that it still writes a report when everything else is wedged.
static const char* volatile s_guard_line;

static const char* const s_draw_stage_name[WD_DRAW_STAGE_COUNT] = {
	[WD_DRAW_IDLE]        = "not in chunkRenderDraw",
	[WD_DRAW_ENTER]       = "entered, before the cull",
	[WD_DRAW_CULL]        = "inside cullFrame (CPU only, no GPU commands)",
	[WD_DRAW_BIND]        = "pipelineBind + projection uniform",
	[WD_DRAW_OPAQUE]      = "opaque pass",
	[WD_DRAW_TRANSPARENT] = "transparent pass",
	[WD_DRAW_DONE]        = "chunkRenderDraw returned (world submitted)",
	[WD_DRAW_EYE_SETUP]   = "eye setup: RenderTargetClear + FrameDrawOn",
	[WD_DRAW_HIGHLIGHT]   = "highlightDraw (block cage)",
	[WD_DRAW_PLAYERS]     = "playerModelDraw (remote bodies)",
	[WD_DRAW_TAGS]        = "playerModelDrawTags (name-tag sprites)",
	[WD_DRAW_BOTTOM]      = "drawBottomUi (bottom screen + UI sprites)",
	[WD_DRAW_FRAME_END]   = "C3D_FrameEnd (submitting to the GPU)",
	[WD_DRAW_FRAME_WAIT]  = "C3D_FrameBegin SYNCDRAW (waiting on the GPU)",
};

static const char* drawStageName(s32 s)
{
	if (s < 0 || s >= WD_DRAW_STAGE_COUNT || !s_draw_stage_name[s]) return "(none recorded)";
	return s_draw_stage_name[s];
}
#endif

static volatile bool s_quit;
static volatile bool s_fired;
static Thread        s_thread;
static char          s_report[WD_REPORT_MAX];

// Indexed by WdPhase. Short and upper-case because these end up being read off a photograph of
// a screen or out of a text file by someone who is not looking at this source.
static const char* const s_phase_name[WD_PHASE_COUNT] = {
	[WD_PHASE_APT]             = "APT",
	[WD_PHASE_INPUT]           = "INPUT",
	[WD_PHASE_LOAD_GEN]        = "LOAD_GEN",
	[WD_PHASE_LOAD_MESH]       = "LOAD_MESH",
	[WD_PHASE_LOAD_DRAW]       = "LOAD_DRAW",
	[WD_PHASE_HANDOFF_GPU]     = "HANDOFF_GPU",
	[WD_PHASE_HANDOFF_SAVE]    = "HANDOFF_SAVE",
	[WD_PHASE_HANDOFF_REPORT]  = "HANDOFF_REPORT",
	[WD_PHASE_NET]             = "NET",
	[WD_PHASE_INSTALL]         = "INSTALL",
	[WD_PHASE_SIM]             = "SIM",
	[WD_PHASE_MESH]            = "MESH",
	[WD_PHASE_DRAW]            = "DRAW",
	[WD_PHASE_SAVE]            = "SAVE",
};

static const char* phaseName(u32 p)
{
	return (p < WD_PHASE_COUNT) ? s_phase_name[p] : "?";
}

// Writes s_report to sdmc:/blocksmith/hang.txt through the raw FS calls rather than stdio,
// copying crash.c's dumpWriteToSd for a reason specific to this file: the main thread is by
// definition stuck somewhere unknown, and if where it is stuck happens to be inside newlib —
// mid-fopen on a region file, say — then it is holding newlib's own lock, and a stdio call from
// this thread would block on that lock and the report would never be written. The FS service
// calls take no locks this process owns.
//
// Silent on every failure, same as the crash path: there is no console in a shipped build and
// nothing useful to do about an SD card that will not take a 500-byte file.
static void reportWrite(size_t len)
{
	FS_Archive archive;
	Result rc = FSUSER_OpenArchive(&archive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""));
	if (R_FAILED(rc)) return;

	FSUSER_CreateDirectory(archive, fsMakePath(PATH_ASCII, WD_DIR_REL), FS_ATTRIBUTE_DIRECTORY);

	Handle file;
	rc = FSUSER_OpenFile(&file, archive, fsMakePath(PATH_ASCII, WD_FILE_REL),
	                     FS_OPEN_WRITE | FS_OPEN_CREATE, 0);
	if (R_SUCCEEDED(rc)) {
		// Truncate to exactly this report, so a shorter one cannot leave the tail of a longer
		// previous one trailing it and be read as one confused document.
		FSFILE_SetSize(file, (u64)len);

		u32 written = 0;
		FSFILE_Write(file, &written, 0, s_report, (u32)len,
		             FS_WRITE_FLUSH | FS_WRITE_UPDATE_TIME);
		FSFILE_Close(file);
	}

	FSUSER_CloseArchive(archive);
}

static void reportBuild(u32 phase, u32 frames, u32 stuck_ms)
{
	const int n = snprintf(s_report, sizeof(s_report),
		"Blocksmith hang report\n"
		"\n"
		"The main thread stopped completing frames. This file was written by the watchdog\n"
		"thread (source/app/watchdog.c), not by a crash handler: no exception was raised,\n"
		"the main thread simply stopped returning to aptMainLoop(), which is what makes the\n"
		"HOME button stop responding as well.\n"
		"\n"
		"version      : %s\n"
		"phase        : %s\n"
		"frames drawn : %lu\n"
		"stalled for  : %lu ms\n"
		"columns in   : %ld\n"
		"meshes       : %ld\n"
		"mesh queued  : %ld\n"
		"worker       : %s\n"
		"\n"
		"'phase' is the last place the main thread reported being, listed in\n"
		"source/app/watchdog.h. That name is the answer this file exists to give.\n",
		BLOCKSMITH_VERSION_SET ? BLOCKSMITH_VERSION : "(unset)",
		phaseName(phase),
		(unsigned long)frames,
		(unsigned long)stuck_ms,
		(long)s_columns, (long)s_meshes, (long)s_queued,
		s_worker_busy ? "BUSY" : "IDLE");

	if (n <= 0) return;
	size_t len = ((size_t)n < sizeof(s_report)) ? (size_t)n : sizeof(s_report) - 1;

#if BS_DRAW_PROBE
	// Appended rather than woven into the format above, so a shipped report stays byte-for-byte
	// what it already was and only the bisect build carries the extra three lines.
	const int m = snprintf(s_report + len, sizeof(s_report) - len,
		"\n"
		"probe arm    : %ld\n"
		"linear free  : %lu B\n"
		"vram free    : %lu B\n"
		"draw stage   : %s\n"
		"  loop index : %ld\n"
		"  mesh slot  : %ld\n"
		"\n"
		"'draw stage' is where in the frame's drawing the main thread was, from entering\n"
		"chunkRenderDraw (scene/chunk_render.c) through to the wait on the GPU at the top of\n"
		"the next frame. CULL means it never issued a GPU command at all, so the console is\n"
		"stuck on the CPU. Any later stage means it is stuck on or after a GPU submission,\n"
		"and the stage names which one.\n",
		(long)s_probe_arm, (unsigned long)s_linear_free, (unsigned long)s_vram_free,
		drawStageName(s_draw_stage), (long)s_draw_k, (long)s_draw_slot);
	if (m > 0) {
		const size_t room = sizeof(s_report) - len - 1;
		len += ((size_t)m < room) ? (size_t)m : room;
	}

	// The draw guard's verdict. Printed even when nothing was wrong, because "no bad draw was
	// ever issued" is the more useful half of the answer: it says the commands the GPU was
	// handed were well formed and it stopped for some other reason, which is a different bug
	// from the one being hunted.
	const char* g = s_guard_line;
	if (g) {
		const int gm = snprintf(s_report + len, sizeof(s_report) - len,
			"\n"
			"draw guard   : %s\n", g);
		if (gm > 0) {
			const size_t room2 = sizeof(s_report) - len - 1;
			len += ((size_t)gm < room2) ? (size_t)gm : room2;
		}
	}
#endif

	reportWrite(len);
	s_fired = true;
}

static void watchdogMain(void* arg)
{
	(void)arg;

	u32 last_beat = s_beat;
	u32 stuck_ms  = 0;

	while (!s_quit) {
		svcSleepThread(WD_POLL_NS);

		const u32 beat  = s_beat;
		const u32 phase = s_phase;

		if (beat != last_beat) {
			last_beat = beat;
			stuck_ms  = 0;
			continue;
		}

		// A stall inside aptMainLoop() is the HOME menu holding the app suspended, which is
		// normal and unbounded — see watchdog.h. Not a hang, and the clock does not run.
		if (phase == WD_PHASE_APT) {
			stuck_ms = 0;
			continue;
		}

		stuck_ms += WD_POLL_MS;
		if (stuck_ms >= WD_TIMEOUT_MS && !s_fired)
			reportBuild(phase, beat, stuck_ms);
	}
}

bool watchdogStart(void)
{
	if (s_thread) return true;

	s_quit  = false;
	s_fired = false;

	// Two priority steps below the main thread (higher number is lower priority here), so this
	// can never delay a frame, and one step below app/worker.c's generator for the same reason —
	// a thread that wakes forty times a second must not be able to push the thread doing the
	// real work off the CPU.
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	prio += 2;
	if (prio > 0x3F) prio = 0x3F;

	// Core 1 first, then core 0, exactly as worker.c does and for a sharper reason here: the
	// case being watched for includes a main thread spinning without ever yielding, and on this
	// kernel a lower-priority thread pinned to the same core as a spinning one never gets
	// scheduled at all. On core 1 the monitor still runs and still writes its report. Core 0 is
	// the fallback because threadCreate on core 1 is refused outright under some launch paths,
	// and a watchdog that only works on core 1 would silently not exist on those.
	s_thread = threadCreate(watchdogMain, NULL, WD_STACK_BYTES, prio, 1, false);
	if (!s_thread)
		s_thread = threadCreate(watchdogMain, NULL, WD_STACK_BYTES, prio, 0, false);

	return s_thread != NULL;
}

void watchdogStop(void)
{
	if (!s_thread) return;

	s_quit = true;

	// Up to one poll interval plus slack. If it somehow does not come back, leak the thread
	// rather than block the shutdown path forever — a hung watchdog must not become the thing
	// that hangs the console.
	threadJoin(s_thread, WD_POLL_NS * 4);
	threadFree(s_thread);
	s_thread = NULL;
}

void watchdogPhase(WdPhase p)
{
	s_phase = (u32)p;
}

void watchdogBeat(void)
{
	s_beat++;
}

void watchdogCounters(int columns, int meshes, int queued, bool worker_busy)
{
	s_columns     = columns;
	s_meshes      = meshes;
	s_queued      = queued;
	s_worker_busy = worker_busy;
}

#if BS_DRAW_PROBE
void watchdogProbeState(int arm, uint32_t linear_free, uint32_t vram_free)
{
	s_probe_arm   = arm;
	s_linear_free = linear_free;
	s_vram_free   = vram_free;
}

void watchdogDrawStage(int stage, int k, int slot)
{
	s_draw_stage = stage;
	s_draw_k     = k;
	s_draw_slot  = slot;
}
#endif

void watchdogGuardLine(const char* line)
{
	s_guard_line = line;
}

bool watchdogFired(void)
{
	return s_fired;
}
