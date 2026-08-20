// See watchdog.h for why this exists and why the thresholds are what they are.
#include "app/watchdog.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>

#if BS_DRAW_PROBE
// Only for C3D_FrameCounter, and only in the probe build. It is a two-instruction read of a
// plain .bss array (objdump: `ldr r3,[pc,#4]` / `ldr r0,[r3,r0,lsl #2]` / `bx lr`) — no lock,
// no allocation, no service call — which is the only reason this thread is allowed to call it.
// Nothing else in citro3d is safe from here; see the thread rules at the top of watchdogMain.
#include <citro3d.h>

// citro3d's C3D_Context is opaque outside the library — internal.h is not installed — but it
// does not need to be declared to be read. __C3D_Context is a *global* symbol in the linked
// ELF (arm-none-eabi-nm on blocksmith.elf: 00278828 B __C3D_Context), and C3D_FrameBegin
// loads that exact address into r5 and hands it to gxCmdQueueWait unchanged:
//
//   1b5b7c: ldr r5,[pc,#..]   ; -> 0x00278828, __C3D_Context
//   1b5b84: mov r0,r5
//           bl  gxCmdQueueWait
//
// The queue it waits on therefore sits at offset 0 of the context, so the context base IS a
// gxCmdQueue_s*. Measured by disassembling our own binary, not assumed from citro3d's source.
// Declared as an array so the name yields its address without needing the struct's layout.
extern u32 __C3D_Context[];
#endif

#include "app/gputest.h"
#include "version.h"

// Alongside the crash dump and the save data (app/crash.c, main.c's saveWorldDir()). Two
// spellings for the same reason crash.c needs two: the write goes through the raw FS calls,
// which take archive-relative paths, while the "has one been left behind" check on the next
// boot is ordinary stdio.
#define WD_DIR_REL     "/blocksmith"
#define WD_FILE_REL    "/blocksmith/hang.txt"

// The GX queue self-test's output. A separate file from hang.txt on purpose: it is written
// during normal play, before anything has gone wrong, and must never be mistaken for — or
// overwrite — the report that says the console froze. See watchdogGxSelfTest.
#define WD_GX_FILE_REL "/blocksmith/gxprobe.txt"

// The stuck command list's own contents, and the previous frame's for comparison.
//
// gxAppend names WHICH command the GPU never finished; these two files carry what was actually
// inside it. That is the next question and it cannot be answered from the console: the list is
// a stream of PICA200 register writes, and reading it means decoding it, which is done off the
// device so the decoder can be corrected without another boot.
//
// The comparison is the point. hang.txt measured the stuck list at 0x2430 bytes and gxprobe.txt
// measured a healthy frame's at 0x2430 as well - the same length, which for a citro3d command
// buffer means the same sequence of calls produced it. So the frame that hung issued the same
// commands as the 334 frames before it and differs only in the VALUES inside them, and a diff
// of these two files is the shortest path to which value.
#define WD_CMD_HANG_REL "/blocksmith/cmdhang.bin"
#define WD_CMD_PREV_REL "/blocksmith/cmdprev.bin"

// 0x2430 measured on hardware and in the emulator; 24 KB is ~2.6x that. A list longer than this
// is truncated rather than refused, and hang.txt says so, because a truncated head is still
// worth reading and a silent cap is not.
#define WD_CMD_MAX (24u * 1024u)

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

// v1.1.5's report was cut off mid-word at the old 1280 — its last line ended at "citro3d's
// two frame counter". Nothing decisive was lost that time, because the two lines that
// mattered came out above the cut. The GX-queue section added below is decisive, and must
// not be the thing that falls off the end.
#if BS_DRAW_PROBE
#define WD_REPORT_MAX 4096
#else
#define WD_REPORT_MAX 768
#endif

static volatile u32  s_phase;
static volatile u32  s_beat;
static volatile s32  s_columns;
static volatile s32  s_meshes;
static volatile s32  s_queued;
static volatile bool s_worker_busy;

// The draw guard's verdict, already formatted by the main thread into a buffer it owns and
// keeps alive for the rest of the process. A pointer and not a copy because this thread must
// not allocate, must not take a lock, and must not call anything that might: the whole value
// of this file is that it still writes a report when everything else is wedged.
//
// Declared out here rather than beside the other probe state because watchdogGuardLine() is
// compiled into EVERY build while the code that reads this is probe-only. With the declaration
// inside #if BS_DRAW_PROBE, a build without the probe flags failed to compile at all — which
// went unnoticed from v1.1.2 to v1.1.8 because every one of those builds set them.
static const char* volatile s_guard_line;

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

#if BS_DRAW_PROBE
// The two citro3d frame counters, sampled twice by watchdogMain once the stall is confirmed.
// See the paragraph that formats them in reportBuild for what the pair is for.
#define WD_FC_GAP_MS 1000
static u32 s_fc_a[2], s_fc_b[2];
#endif

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
	[WD_DRAW_FRAME_WAIT]  = "C3D_FrameBegin SYNCDRAW (v1.1.4's undivided marker)",
	[WD_DRAW_FRAME_VSYNC] = "C3D_FrameSync - waiting for a VBLANK TICK, not for the GPU",
	[WD_DRAW_FRAME_QUEUE] = "C3D_FrameBegin(0) - waiting for the GX QUEUE to drain",
};

static const char* drawStageName(s32 s)
{
	if (s < 0 || s >= WD_DRAW_STAGE_COUNT || !s_draw_stage_name[s]) return "(none recorded)";
	return s_draw_stage_name[s];
}

// The type byte of a GX command entry. Only the three the frame actually uses are named, and
// each of those three was read out of the binary rather than out of a header: libctru's
// helpers build their entry with a literal word whose low byte is the type, and objdump shows
//
//   GX_ProcessCommandList  literal at 0x1ba384 = 01 01 00 01  -> type 1
//   GX_MemoryFill          literal at 0x1ba3f8 = 02 01 00 01  -> type 2
//   GX_DisplayTransfer     literal at 0x1ba46c = 03 01 00 01  -> type 3
//
// Anything else prints as its raw number and says so, because guessing a name for a byte that
// was never measured would put a claim in the report that no evidence supports.
static const char* gxTypeName(unsigned t)
{
	switch (t) {
	case 1: return "ProcessCommandList - OUR draw commands";
	case 2: return "MemoryFill - a colour or depth buffer clear";
	case 3: return "DisplayTransfer - copying a framebuffer to the screen";
	default: return "(type not identified by disassembly)";
	}
}
#endif

static volatile bool s_quit;
static volatile bool s_fired;
static Thread        s_thread;
static char          s_report[WD_REPORT_MAX];

#if BS_DRAW_PROBE
// The self-test's own buffer rather than a share of s_report. A share would work today, since
// the self-test runs long before any hang, but "today" is not a guarantee worth taking on the
// one buffer whose contents are the entire point of the build.
// 1280 and not 768: at 768 the first run truncated its last entry mid-word, ending the file at
// "args     : 1f0bb800 30151800 0".
static char          s_gxsnap[1280];

// A ring of the last WD_CMD_RING frames' command lists, captured by watchdogCmdCapture after
// every C3D_FrameEnd, because by the time the watchdog fires the main thread is parked inside the
// next C3D_FrameBegin and cannot capture anything.
//
// Eight and not two. v1.1.7 kept two, on the reasoning that the interesting comparison is the
// hung frame against the one immediately before it. That is still the first comparison, but it
// assumes the fault appears in the frame that hangs — and if a value drifts, or a buffer is freed
// and reused a frame or two ahead of the draw that trips over it, then the last healthy frame
// already contains the damage and a two-frame diff shows nothing. Eight frames is a third of a
// second of history, and it is 192 KB of BSS in BS_DRAW_PROBE builds only, against 27 MB of
// linear heap free on the console that froze.
#define WD_CMD_RING 8u

static u8   s_cmd_copy[WD_CMD_RING][WD_CMD_MAX];
static u32  s_cmd_len[WD_CMD_RING];
static u32  s_cmd_addr[WD_CMD_RING];
static u32  s_cmd_full[WD_CMD_RING];  // the list's real length, before any truncation to WD_CMD_MAX
static volatile u32 s_cmd_seq;        // frames captured; newest slot is (seq-1) % WD_CMD_RING
#endif

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
static void fileWrite(const char* rel_path, const char* data, size_t len)
{
	FS_Archive archive;
	Result rc = FSUSER_OpenArchive(&archive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""));
	if (R_FAILED(rc)) return;

	FSUSER_CreateDirectory(archive, fsMakePath(PATH_ASCII, WD_DIR_REL), FS_ATTRIBUTE_DIRECTORY);

	Handle file;
	rc = FSUSER_OpenFile(&file, archive, fsMakePath(PATH_ASCII, rel_path),
	                     FS_OPEN_WRITE | FS_OPEN_CREATE, 0);
	if (R_SUCCEEDED(rc)) {
		// Truncate to exactly this report, so a shorter one cannot leave the tail of a longer
		// previous one trailing it and be read as one confused document.
		FSFILE_SetSize(file, (u64)len);

		u32 written = 0;
		FSFILE_Write(file, &written, 0, data, (u32)len,
		             FS_WRITE_FLUSH | FS_WRITE_UPDATE_TIME);
		FSFILE_Close(file);
	}

	FSUSER_CloseArchive(archive);
}

// The same writer, for app/gputest.c. Its reports are written from the main thread rather than
// the monitor, but from a frame that has already failed — so it wants the same no-stdio path for
// the same reason, and there is no sense having two of them.
void watchdogWriteFile(const char* rel_path, const void* data, size_t len)
{
	fileWrite(rel_path, (const char*)data, len);
}

#if BS_DRAW_PROBE
// Appends the state of the GX command queue to buf, returning the new length.
//
// v1.1.5's hardware report answered the previous question and posed this one: the wait was
// gxCmdQueueWait (`draw stage : GX QUEUE`) while vblanks were still arriving
// (`gsp vblank : ALIVE (+60 / +60)`), so the GSP event thread was alive and a command it had
// already submitted still never reported finishing. The queue itself records how far it got.
//
// Reading it is four u16 loads and a walk of a small array — no lock, no allocation, no
// service call — so it obeys the same thread rules as everything else in this file. It is also
// stable while the watchdog reads it: the main thread is parked inside gxCmdQueueWait, and the
// main thread is the only thing that ever adds to this queue.
//
// Per <3ds/gpu/gx.h>, `curEntry` is the index of the first command not yet submitted and
// `lastEntry` is the number GX has *completed*. So the command in flight is entries[lastEntry],
// and there are two possible answers — which is the whole point of asking:
//
//   completed < queued  -> entries[lastEntry] is the command that never finished, and its type
//                          says whether that is our world's command list (1), a buffer clear
//                          (2), or the copy of a framebuffer to the screen (3).
//   completed == queued -> the queue drained and the wait still did not return. Then a stuck
//                          GPU is not the bug at all: gxCmdQueueWait polls a libctru global
//                          (`isRunning`, 0x002799a1 in our ELF) and not these counters, so the
//                          flag was simply never cleared.
//
// `oldest_label` names the first incomplete entry. It differs by caller and the difference is
// not cosmetic: in the hang report that entry is the one the console is stuck on, while in the
// self-test — where the frame is proceeding perfectly normally — it is merely the one GX has
// not got to yet, and calling that "STUCK ON" would be a false statement in a file whose whole
// job is to be believed.
//
// Deliberately NOT printed here: libctru's gpuCmdBuf / gpuCmdBufSize / gpuCmdBufOffset. They
// were in the first version of this function and the self-test measured them as
// `base 00000000  size 0 words  used 0 words` — citro3d does not route through libctru's
// GPUCMD_* layer, so those globals are never set in this process and the line was three zeroes
// pretending to be evidence. The type-1 entry's own args[0]/args[1] give the command list's
// address and byte size directly, which is what the line was for.
static size_t gxAppend(char* buf, size_t cap, size_t len, const char* oldest_label)
{
	const gxCmdQueue_s* q = (const gxCmdQueue_s*)(const void*)__C3D_Context;
	const unsigned qcap = q->maxEntries;
	const unsigned num  = q->numEntries;
	const unsigned cur  = q->curEntry;
	const unsigned done = q->lastEntry;

	const int qm = snprintf(buf + len, cap - len,
		"\n"
		"gx queue     : cap %u  queued %u  submitted %u  completed %u\n",
		qcap, num, cur, done);
	if (qm > 0) {
		const size_t room = cap - len - 1;
		len += ((size_t)qm < room) ? (size_t)qm : room;
	}

	// Bounds before the walk. If any of this is implausible then the queue pointer is wrong or
	// the memory is corrupt, and indexing entries[] would fault — which would cost the entire
	// report, including the lines above it that are already correct. 64 is far above citro3d's
	// real capacity and exists only as a sanity ceiling.
	if (!q->entries || qcap == 0 || qcap > 64 || num > qcap || cur > qcap || done > qcap) {
		const int bm = snprintf(buf + len, cap - len,
			"  entries    : NOT READ - the counters above are implausible, so the entry array\n"
			"               was not walked. entries=%08lx\n",
			(unsigned long)(u32)q->entries);
		if (bm > 0) {
			const size_t room = cap - len - 1;
			len += ((size_t)bm < room) ? (size_t)bm : room;
		}
	} else if (done < num) {
		// Which way round the stall is, and this line is the difference between aiming the next
		// build at the GPU and aiming it at the CPU.
		//
		// `cur` is the first entry not yet HANDED to GX; `done` is how many it has reported
		// finishing. If cur has not moved past done then nothing is in flight at all: the GPU
		// finished everything it was given and these entries are merely waiting to be submitted,
		// which means the thing that stopped is the main thread, not the hardware.
		//
		// Written after a red-arm run with a stall planted in the CPU-side draw loop printed
		// "queued 1 submitted 0 completed 0" under a heading reading "STUCK ON: MemoryFill".
		// The numbers were right and the heading was a lie; a report that has to be believed
		// cannot carry one.
		const bool in_flight = (cur > done);
		if (!in_flight) {
			const int fm = snprintf(buf + len, cap - len,
				"  NOTE       : submitted == completed, so NOTHING is in flight. The GPU finished\n"
				"               everything it was handed. The entr%s below %s never sent to it,\n"
				"               so the stall is on the CPU side, not in the hardware.\n",
				(num - done) == 1u ? "y" : "ies", (num - done) == 1u ? "was" : "were");
			if (fm > 0) {
				const size_t room = cap - len - 1;
				len += ((size_t)fm < room) ? (size_t)fm : room;
			}
		}

		// Every command GX has been given and not reported back, oldest first. The oldest is
		// the one it is actually on; anything after it is queued behind it.
		for (unsigned i = done; i < num && i < done + 4u; i++) {
			const gxCmdEntry_s* e = &q->entries[i];
			const int em = snprintf(buf + len, cap - len,
				"  %-10s : entry %u  type %u  %s\n"
				"    args     : %08lx %08lx %08lx %08lx\n",
				(i == done) ? (in_flight ? oldest_label : "NOT SENT") : "behind it",
				i, (unsigned)e->type, gxTypeName(e->type),
				(unsigned long)e->args[0], (unsigned long)e->args[1],
				(unsigned long)e->args[2], (unsigned long)e->args[3]);
			if (em <= 0) break;
			const size_t room = cap - len - 1;
			len += ((size_t)em < room) ? (size_t)em : room;
		}
	} else {
		const int nm = snprintf(buf + len, cap - len,
			"  STUCK ON   : NOTHING - every queued command reported complete, yet the wait\n"
			"               never returned. The GPU is not the thing that is stuck; libctru's\n"
			"               isRunning flag was never cleared.\n");
		if (nm > 0) {
			const size_t room = cap - len - 1;
			len += ((size_t)nm < room) ? (size_t)nm : room;
		}

		// An empty queue and a reader pointed at the wrong memory produce the same two words.
		// Printing the most recently completed entry tells them apart: a plausible type and a
		// command-list address matching `cmd buffer base` above means this code really is
		// looking at citro3d's queue. Without this, "NOTHING" would be unfalsifiable.
		if (num > 0 && q->entries) {
			const gxCmdEntry_s* e = &q->entries[num - 1];
			const int lm = snprintf(buf + len, cap - len,
				"  last done  : entry %u  type %u  %s\n"
				"    args     : %08lx %08lx %08lx %08lx\n",
				num - 1, (unsigned)e->type, gxTypeName(e->type),
				(unsigned long)e->args[0], (unsigned long)e->args[1],
				(unsigned long)e->args[2], (unsigned long)e->args[3]);
			if (lm > 0) {
				const size_t room = cap - len - 1;
				len += ((size_t)lm < room) ? (size_t)lm : room;
			}
		}
	}

	return len;
}

// Takes a copy of this frame's command list. Called from the main thread immediately after
// C3D_FrameEnd, where the entry is guaranteed to be in the queue: FrameEnd has just added it and
// nothing clears the queue until the next C3D_FrameBegin.
//
// The entry is found by SCANNING for type 1, not by indexing at lastEntry. lastEntry is the
// right index at hang time and the wrong one here — a healthy frame is exactly the case where
// the GPU has already finished the list and moved lastEntry past it.
void watchdogCmdCapture(void)
{
	const gxCmdQueue_s* q = (const gxCmdQueue_s*)(const void*)__C3D_Context;
	if (!q->entries || q->maxEntries == 0 || q->maxEntries > 64 || q->numEntries > q->maxEntries)
		return;

	for (unsigned i = 0; i < q->numEntries; i++) {
		const gxCmdEntry_s* e = &q->entries[i];
		if (e->type != 1) continue;

		const u32 addr = e->args[0];
		const u32 size = e->args[1];
		if (!addr || !size) return;

		const unsigned slot = s_cmd_seq % WD_CMD_RING;
		const u32 take = (size < WD_CMD_MAX) ? size : WD_CMD_MAX;
		memcpy(s_cmd_copy[slot], (const void*)addr, take);
#ifndef BS_CMD_SCRIBBLE_TEST
#define BS_CMD_SCRIBBLE_TEST 0
#endif
#if BS_CMD_SCRIBBLE_TEST
		// The red arm for cmdDump's live-vs-submitted comparison, and nothing else uses it.
		// Corrupts one byte of the list immediately AFTER the copy is taken — precisely the event
		// the comparison exists to detect. Without it "IDENTICAL" has only ever been seen agreeing
		// with itself and would be unfalsifiable, which is the mistake v1.1.5's build script made.
		((u8*)addr)[16] ^= 0xFFu;
#endif
		s_cmd_len[slot]  = take;
		s_cmd_addr[slot] = addr;
		s_cmd_full[slot] = size;
		s_cmd_seq++;

#if BS_GPU_TESTS
		// Check the list the GPU has just been given, before anything has gone wrong. This is the
		// test most likely to name the bug outright: it sees the actual bytes of every draw in the
		// frame, including the ones scene/chunk_render.c's draw guard does not cover — the
		// highlight cage, other players, name tags and the whole bottom-screen sprite batch.
		gpuTestValidateList(s_cmd_copy[slot], take, s_cmd_seq);

		// And once, on the very first list, prove the replay machinery works while there is still
		// a known-good list to prove it against. See gpuTestListProbe.
		if (s_cmd_seq == 1u) gpuTestListProbe(s_cmd_copy[slot], take);
#endif
		return;
	}
}

const uint8_t* watchdogLastCmdList(uint32_t* len, uint32_t* addr)
{
	const u32 seq = s_cmd_seq;
	if (!seq) return NULL;
	const unsigned slot = (seq - 1u) % WD_CMD_RING;
	if (len)  *len  = s_cmd_len[slot];
	if (addr) *addr = s_cmd_addr[slot];
	return s_cmd_copy[slot];
}

// Writes cmdhang.bin and cmdprev.bin, and appends to the report what could only be established
// here: whether the command buffer still holds what was handed to the GPU.
//
// That check has to happen on the console. If the live memory differs from the copy taken at
// C3D_FrameEnd, then something overwrote the list after the GPU was pointed at it, and the bug
// is that overwrite rather than anything inside the list — a conclusion no offline decode of
// either file could reach, because both files would decode as perfectly valid command streams.
static size_t cmdDump(char* buf, size_t cap, size_t len)
{
	const u32 seq = s_cmd_seq;
	int m;

	if (seq == 0) {
		m = snprintf(buf + len, cap - len,
			"\ncmd list     : NOT CAPTURED - no frame ever reached C3D_FrameEnd.\n");
		if (m > 0) { const size_t room = cap - len - 1; len += ((size_t)m < room) ? (size_t)m : room; }
		return len;
	}

	const unsigned cur  = (seq - 1u) % WD_CMD_RING;   // the frame the GPU never finished
	const unsigned prev = (seq - 2u) % WD_CMD_RING;   // the last frame it did finish
	const u8* live = (const u8*)s_cmd_addr[cur];

	// Byte-compare the live list against the copy taken the instant it was submitted.
	u32 diff_at = 0xFFFFFFFFu, was = 0, now = 0, diffs = 0;
	for (u32 i = 0; i < s_cmd_len[cur]; i++) {
		if (live[i] == s_cmd_copy[cur][i]) continue;
		if (diffs == 0) { diff_at = i; was = s_cmd_copy[cur][i]; now = live[i]; }
		diffs++;
	}

	m = snprintf(buf + len, cap - len,
		"\n"
		"cmd list     : addr %08lx  %lu bytes%s  (frame captures: %lu)\n",
		(unsigned long)s_cmd_addr[cur], (unsigned long)s_cmd_full[cur],
		(s_cmd_full[cur] > WD_CMD_MAX) ? "  TRUNCATED to 24 KB in the file" : "",
		(unsigned long)seq);
	if (m > 0) { const size_t room = cap - len - 1; len += ((size_t)m < room) ? (size_t)m : room; }

	if (diffs == 0)
		m = snprintf(buf + len, cap - len,
			"  live vs    : IDENTICAL - the list the GPU is executing is byte-for-byte what was\n"
			"  submitted    submitted to it, so nothing overwrote it after submission.\n");
	else
		m = snprintf(buf + len, cap - len,
			"  live vs    : DIFFERS in %lu bytes, first at offset %lu (submitted %02lx, now %02lx).\n"
			"  submitted    The command buffer was overwritten AFTER the GPU was pointed at it.\n"
			"               That is the bug; nothing inside the list needs to be wrong.\n",
			(unsigned long)diffs, (unsigned long)diff_at,
			(unsigned long)was, (unsigned long)now);
	if (m > 0) { const size_t room = cap - len - 1; len += ((size_t)m < room) ? (size_t)m : room; }

	fileWrite(WD_CMD_HANG_REL, (const char*)live, s_cmd_len[cur]);
	if (seq >= 2)
		fileWrite(WD_CMD_PREV_REL, (const char*)s_cmd_copy[prev], s_cmd_len[prev]);

	// The rest of the ring, oldest reachable frame first. cmd2.bin is two frames before the hang,
	// cmd7.bin is seven. They exist because a two-frame diff only finds the fault if the fault
	// appears in the frame that hangs; if a value drifts over several frames, or a buffer is
	// reused ahead of the draw that trips over it, the damage is already in cmdprev.bin and only
	// a longer history shows where it started.
	unsigned written = 0;
	for (unsigned k = 2; k < WD_CMD_RING && k < seq; k++) {
		const unsigned slot = (seq - 1u - k) % WD_CMD_RING;
		if (!s_cmd_len[slot]) continue;
		char rel[48];
		snprintf(rel, sizeof(rel), "%s/cmd%u.bin", WD_DIR_REL, k);
		fileWrite(rel, (const char*)s_cmd_copy[slot], s_cmd_len[slot]);
		written++;
	}

	m = snprintf(buf + len, cap - len,
		"  files      : cmdhang.bin = this list, live. %s\n"
		"               plus %u older frame(s) as cmdN.bin, N frames before the hang.\n",
		(seq >= 2) ? "cmdprev.bin = the frame before it."
		           : "cmdprev.bin NOT written (only one frame captured).",
		written);
	if (m > 0) { const size_t room = cap - len - 1; len += ((size_t)m < room) ? (size_t)m : room; }

#if BS_GPU_TESTS
	// What the per-frame validator saw on the way here. If it flagged a frame, that frame number
	// against the capture count above says whether the damage arrived with the hang or before it.
	len = gpuTestValidatorReport(buf, cap, len);
#endif

	return len;
}

// Proof that gxAppend can print something other than "NOTHING".
//
// The section above is only worth reading if it is known to work, and neither place it would
// normally run can establish that: on hardware the queue state is the unknown being measured,
// and the artificially-parked RED arm (-DBS_FRAME_HANG_TEST=WD_DRAW_FRAME_QUEUE) never submits
// anything, so it would print an empty queue whether the reader worked or not — indistinguish-
// able from a reader that is silently reading the wrong address.
//
// So the same function is run once from the main thread at the one instant the queue is
// guaranteed to be loaded: immediately after C3D_FrameEnd(0), which has just added this
// frame's command list and its display transfers and returned without waiting for them. If
// gxprobe.txt comes back naming a type-1 ProcessCommandList whose args[0] equals the
// `cmd buffer base` printed beside it, the reader is looking at the right memory and the entry
// walk works. If it comes back "NOTHING" or "NOT READ", the reader is broken and the hang
// report's queue section means nothing — which is exactly what needs to be known before
// shipping a build to be booted once.
void watchdogGxSelfTest(void)
{
	static bool done_once;
	if (done_once) return;
	done_once = true;

	size_t len = 0;
	const int n = snprintf(s_gxsnap, sizeof(s_gxsnap),
		"Blocksmith GX queue self-test\n"
		"\n"
		"Sampled on the main thread immediately after C3D_FrameEnd(0) returned, where the\n"
		"queue must be non-empty. This file proves the queue reader in source/app/watchdog.c\n"
		"can see real commands; it is not a hang report and does not mean anything went wrong.\n");
	if (n > 0) len = ((size_t)n < sizeof(s_gxsnap)) ? (size_t)n : sizeof(s_gxsnap) - 1;

	len = gxAppend(s_gxsnap, sizeof(s_gxsnap), len, "in flight");
	fileWrite(WD_GX_FILE_REL, s_gxsnap, len);
}
#endif

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

	// Is GSP still delivering events while the main thread is stuck? citro3d bumps
	// frameCounter[0] and [1] from its two VBlank callbacks (onVBlank0 / onVBlank1), which run
	// on libctru's GSP event thread — so these two numbers advance if and only if VBlank
	// interrupts are still arriving and being dispatched. They say nothing about the GPU
	// finishing a draw; that is exactly why they split the two waits apart:
	//
	//   ALIVE   -> vblanks are arriving, so C3D_FrameSync could not be the thing that is stuck.
	//              The stall is gxCmdQueueWait, i.e. a GPU command that never completed.
	//   STOPPED -> event delivery or the frame-pacing tick has died. The GPU is a red herring
	//              and the world draw is irrelevant.
	//
	// Two samples a second apart rather than one, because a single number cannot be moving or
	// still. Sampled by the caller before this runs, so the report only formats them.
	{
		const unsigned long d0 = (unsigned long)(s_fc_b[0] - s_fc_a[0]);
		const unsigned long d1 = (unsigned long)(s_fc_b[1] - s_fc_a[1]);
		const int fm = snprintf(s_report + len, sizeof(s_report) - len,
			"\n"
			"gsp vblank   : %s  (+%lu / +%lu in %lu ms)\n"
			"  counters   : %lu / %lu  then  %lu / %lu\n"
			"\n"
			"'gsp vblank' is citro3d's two frame counters, read twice while the main thread was\n"
			"already stuck. They are bumped by the GSP event thread on every vblank tick.\n"
			"ALIVE means the GSP event thread is running and delivering VBlank. It does NOT\n"
			"prove command-completion interrupts are being delivered - those are a different\n"
			"GSP event, so an incomplete GX command may mean the GPU never finished it OR that\n"
			"its completion was never signalled. STOPPED means event delivery itself has died,\n"
			"and the drawing is not the bug.\n",
			(d0 || d1) ? "ALIVE" : "STOPPED",
			d0, d1, (unsigned long)WD_FC_GAP_MS,
			(unsigned long)s_fc_a[0], (unsigned long)s_fc_a[1],
			(unsigned long)s_fc_b[0], (unsigned long)s_fc_b[1]);
		if (fm > 0) {
			const size_t roomf = sizeof(s_report) - len - 1;
			len += ((size_t)fm < roomf) ? (size_t)fm : roomf;
		}
	}

	len = gxAppend(s_report, sizeof(s_report), len, "STUCK ON");
	len = cmdDump(s_report, sizeof(s_report), len);

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

	fileWrite(WD_FILE_REL, s_report, len);
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
		if (stuck_ms >= WD_TIMEOUT_MS && !s_fired) {
#if BS_DRAW_PROBE
			// Straddle the report, not before it: the question is whether vblanks are STILL
			// arriving now that the main thread has been stuck for WD_TIMEOUT_MS, so both
			// samples have to be taken from inside the stall.
			s_fc_a[0] = C3D_FrameCounter(0);
			s_fc_a[1] = C3D_FrameCounter(1);
			svcSleepThread((u64)WD_FC_GAP_MS * 1000000ULL);
			s_fc_b[0] = C3D_FrameCounter(0);
			s_fc_b[1] = C3D_FrameCounter(1);
			stuck_ms += WD_FC_GAP_MS;
#endif
			reportBuild(phase, beat, stuck_ms);
		}
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
