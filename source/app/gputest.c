// See gputest.h for what this is for and why it is built the way it is.
#include "app/gputest.h"

#if BS_GPU_TESTS

#include <3ds.h>
#include <citro3d.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "app/watchdog.h"
#include "version.h"

// citro3d's context is opaque outside the library, but __C3D_Context is a global symbol in the
// linked ELF and its gxQueue sits at offset 0 — measured by disassembling our own binary, see the
// comment on the same declaration in app/watchdog.c. Re-verify the address per build; it moves.
extern u32 __C3D_Context[];
#define C3D_QUEUE ((gxCmdQueue_s*)(void*)__C3D_Context)

#define SELFTEST_REL   "/blocksmith/selftest.txt"
#define POSTMORTEM_REL "/blocksmith/postmortem.txt"
#define BISECT_REL     "/blocksmith/bisect.bin"

// Every test waits this long before calling a submission wedged. Generous on purpose: a false
// TIMEOUT would send the next round of work in the wrong direction, and the cost of being
// generous is only ever paid on a test that was going to fail anyway.
#define TEST_TIMEOUT_NS (2000000000LL)

// The largest command list this can hold a working copy of. The real one measured 0x2430 bytes on
// both hardware and the emulator; 32 KB is over three times that, and a longer list is refused
// with a line in the report rather than silently truncated.
#define LIST_MAX (32u * 1024u)

// ---------------------------------------------------------------------------------------------
// Report buffers. Static, because everything here can run from a frame that is already in
// trouble and must not depend on the heap being in a sane state.

static char s_self[8192];
static char s_post[8192];

static size_t app(char* buf, size_t cap, size_t len, const char* fmt, ...)
{
	if (len >= cap) return len;
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf + len, cap - len, fmt, ap);
	va_end(ap);
	if (n < 0) return len;
	len += (size_t)n;
	return len > cap ? cap : len;
}

static u32 tickUs(u64 ticks)
{
	return (u32)(ticks / (u64)(SYSCLOCK_ARM11 / 1000000));
}

// ---------------------------------------------------------------------------------------------
// A private GX queue. Tests submit on this rather than citro3d's, so nothing here can disturb the
// frame loop's own bookkeeping, and — the part that matters during the post-mortem — so a wait
// with a timeout is possible at all. citro3d only binds its queue inside C3D_FrameEnd, so
// borrowing the binding between frames is safe.

static gxCmdEntry_s s_entries[8];
static gxCmdQueue_s s_q;

static void qBegin(void)
{
	memset(&s_q, 0, sizeof(s_q));
	s_q.entries    = s_entries;
	s_q.maxEntries = (u16)(sizeof(s_entries) / sizeof(s_entries[0]));
	gxCmdQueueClear(&s_q);
	GX_BindQueue(&s_q);
	gxCmdQueueRun(&s_q);
}

// Returns true if the queue drained inside the timeout. us is how long the wait actually took,
// which is as interesting as the verdict: a PASS that took 900 ms is not the same as a PASS that
// took 200 us, and only one of them is normal.
//
// The rebind at the end is NOT tidiness, it is a correctness fix for a bug this file had on its
// first run. It ended with GX_BindQueue(NULL), on the assumption that unbinding restored the
// default. It does not: citro3d binds its queue exactly ONCE, in C3Di_RenderQueueInit, and
// nothing rebinds it per frame — measured by disassembling the linked ELF, where the only callers
// of GX_BindQueue outside this file are C3Di_RenderQueueInit and C3Di_RenderQueueExit. So
// unbinding permanently detached citro3d's queue: every later GX_* call went straight to GSP,
// the queue stayed empty for the rest of the run, and gxprobe.txt came back reading
// "gx queue: cap 32 queued 0 submitted 0 completed 0" where the previous build measured five
// entries. On hardware that would have silently destroyed the very report this build exists to
// produce.
static bool qEndT(u32* us, s64 timeout)
{
	const u64 t0 = svcGetSystemTick();
	const bool ok = gxCmdQueueWait(&s_q, timeout);
	*us = tickUs(svcGetSystemTick() - t0);
	gxCmdQueueStop(&s_q);
	GX_BindQueue(C3D_QUEUE);
	return ok;
}

static bool qEnd(u32* us)
{
	return qEndT(us, TEST_TIMEOUT_NS);
}

// Lets a queue that was deliberately abandoned mid-flight finish on its own terms.
//
// Only test 8 needs this, and it needs it for a reason worth writing down: qBegin() memsets the
// queue struct, so using it to "reset" a queue that still has work in flight would hand libctru's
// completion callback a structure that no longer describes the commands GSP is executing. So the
// same queue is resumed instead, and drained by watching its own counters rather than by calling
// gxCmdQueueWait again — the wait is exactly the thing this queue has just been shown unable to
// rely on. Bounded at one second, because a drain that cannot finish must not become a second
// hang inside the code that exists to report the first one.
static void qDrain(void)
{
	volatile const u16* completed = &s_q.lastEntry;
	volatile const u16* queued    = &s_q.numEntries;
	const u64 t0 = svcGetSystemTick();

	GX_BindQueue(&s_q);
	gxCmdQueueRun(&s_q);
	while (*completed != *queued && tickUs(svcGetSystemTick() - t0) < 1000000u)
		svcSleepThread(1000000LL);  // 1 ms
	gxCmdQueueStop(&s_q);
	GX_BindQueue(C3D_QUEUE);
}

// ---------------------------------------------------------------------------------------------
// Scratch memory for the primitive tests. Allocated once; a failure to allocate is reported and
// skips only the tests that needed it.

#define SCRATCH_BYTES (256u * 1024u)
static u32* s_src;
static u32* s_dst;

static bool scratchInit(void)
{
	if (s_src && s_dst) return true;
	if (!s_src) s_src = (u32*)linearAlloc(SCRATCH_BYTES);
	if (!s_dst) s_dst = (u32*)linearAlloc(SCRATCH_BYTES);
	if (!s_src || !s_dst) return false;
	memset(s_src, 0xA5, SCRATCH_BYTES);
	memset(s_dst, 0x00, SCRATCH_BYTES);
	GSPGPU_FlushDataCache(s_src, SCRATCH_BYTES);
	GSPGPU_FlushDataCache(s_dst, SCRATCH_BYTES);
	return true;
}

// ---------------------------------------------------------------------------------------------
// Command-list walking. Shared by the validator and the bisector, and it is the one piece of
// PICA200 knowledge in this file:
//
//   word0 = the parameter
//   word1 = the header: [15:0] register id, [19:16] byte-enable, [27:20] extra parameter words,
//           [31] consecutive-write (each extra param goes to the NEXT register, not this one)
//
// and the whole command is padded to an even number of words.

typedef struct {
	u32 reg;
	u32 nparams;   // 1 + extra
	u32 at;        // word index of word0
	u32 words;     // total words consumed, padding included
	bool consec;
} Cmd;

static bool cmdAt(const u32* w, u32 nwords, u32 i, Cmd* out)
{
	if (i + 1 >= nwords) return false;
	const u32 hdr   = w[i + 1];
	const u32 extra = (hdr >> 20) & 0xFFu;
	u32 n = 2u + extra;
	if (n & 1u) n++;
	if (i + n > nwords) return false;
	out->reg     = hdr & 0xFFFFu;
	out->nparams = 1u + extra;
	out->at      = i;
	out->words   = n;
	out->consec  = ((hdr >> 31) & 1u) != 0u;
	return true;
}

// ---------------------------------------------------------------------------------------------
// B. The per-frame validator.

// PICA float24: [23] sign, [22:16] exponent (bias 63), [15:0] mantissa. An exponent of all ones
// is Inf or NaN, which is the value a rasteriser cannot do anything sensible with. Anything from
// 2^57 upward is flagged separately: legal, but nothing in a 16-block chunk placed in a world
// bounded by the save format has any business being that large, so it means a matrix went wrong.
#define F24_EXP(f) (((f) >> 16) & 0x7Fu)
#define F24_INFNAN 0x7Fu
#define F24_HUGE   0x78u

// The same two thresholds for IEEE float32, which is the format citro3d actually uploads in.
// Exponent is [30:23] with bias 127, so all-ones is Inf/NaN and 127+57 = 184 is the 2^57 cutoff.
#define F32_HUGE   0xB8u

static bool addrReadable(u32 a)
{
	return (a >= 0x18000000u && a < 0x18600000u) ||   // VRAM
	       (a >= 0x1F000000u && a < 0x1F600000u) ||   // QTM / extra VRAM on N3DS
	       (a >= 0x20000000u && a < 0x30000000u);     // FCRAM, including the N3DS extension
}

static int      s_vcode;        // 0 = nothing wrong has been seen this session
static u32      s_vframe;       // the frame it was seen on
static u32      s_vword;        // word offset within that frame's list
static u32      s_vreg, s_vval;
static u32      s_vhits;
static u32      s_vframes_ok;
static char     s_vwhat[96];

static void flag(int code, u32 frame, u32 word, u32 reg, u32 val, const char* what)
{
	s_vhits++;
	if (s_vcode) return;
	s_vcode  = code;
	s_vframe = frame;
	s_vword  = word;
	s_vreg   = reg;
	s_vval   = val;
	snprintf(s_vwhat, sizeof(s_vwhat), "%s", what);
}

bool gpuTestValidateList(const uint8_t* list, uint32_t len, uint32_t frame)
{
	if (!list || len < 8u) return true;
	const u32* w = (const u32*)(const void*)list;
	const u32 nwords = len / 4u;

	const int before = s_vcode;
	u32 base = 0x18000000u;   // citro3d expresses every buffer as an offset from the VRAM base
	u32 voff = 0, nverts = 0;

	// -1 until a 0x2C0 config write says which format the uniform bursts are in. A burst reached
	// without one is left UNCHECKED rather than decoded on a guess: guessing is what produced 597
	// false "Inf or NaN" reports, and a validator that invents evidence is worse than one that
	// stays quiet. In the real captured list every one of the 16 bursts is preceded by a config
	// write, so this only bites on a list captured mid-stream.
	int f32_uniforms = -1;

	// Parameter p of the command starting at word i. Parameter 0 IS word0, at i; every parameter
	// after it lives past the header, at i+1+p. Getting this wrong reads the header as a value
	// and turns the whole validator into noise, so it is written once and used everywhere.
	#define PARAM(i, p) ((p) == 0u ? w[(i)] : w[(i) + 1u + (p)])

	Cmd c;
	for (u32 i = 0; cmdAt(w, nwords, i, &c); i += c.words) {
		// Which format the uniform burst that follows is in. Bit 31 of 0x2C0 selects it, and
		// getting this wrong is not a small error: citro3d puts this GPU in FLOAT32 mode for
		// every uniform it uploads, four plain IEEE words per vec4, and reading those words as
		// packed float24 slices 24-bit fields straight across the float boundaries.
		//
		// That is not hypothetical. Until this was added the validator assumed float24
		// unconditionally and reported "float uniform is Inf or NaN" on 597 of 795 captured
		// frames from a run whose world rendered perfectly and whose draw guard was clean. The
		// real words in that capture were bf800000 (-1.0), 3f3504f3 (0.70710678), c19069e8
		// (-18.05) — a correct view matrix. Every one of those 597 flags was the decoder
		// mis-slicing its own input, and any conclusion ever drawn from a "list check" line in
		// a hang report predating this fix has to be thrown away.
		if (c.reg == 0x2C0u) {
			f32_uniforms = (PARAM(i, 0u) & 0x80000000u) != 0u;
			continue;
		}

		// Float uniforms. A consecutive write to 0x2C1 is a burst of vec4s: four IEEE float32
		// words each in f32 mode, three packed float24 words each in f24 mode.
		if (c.reg == 0x2C1u) {
			if (f32_uniforms < 0) continue;   // format unknown - see the declaration
			if (f32_uniforms) {
				for (u32 p = 0; p < c.nparams; p++) {
					const u32 v = PARAM(i, p);
					const u32 e = (v >> 23) & 0xFFu;
					if (e == 0xFFu)
						flag(1, frame, i + p, c.reg, v, "float uniform is Inf or NaN");
					else if (e >= F32_HUGE)
						flag(2, frame, i + p, c.reg, v, "float uniform is absurdly large");
				}
				continue;
			}
			for (u32 p = 0; p + 2u < c.nparams; p += 3u) {
				const u32 w0 = PARAM(i, p), w1 = PARAM(i, p + 1u), w2 = PARAM(i, p + 2u);
				const u32 f[4] = {
					 w0 >> 8,
					((w0 & 0x00FFu) << 16) | (w1 >> 16),
					((w1 & 0xFFFFu) << 8)  | (w2 >> 24),
					  w2 & 0x00FFFFFFu,
				};
				for (int k = 0; k < 4; k++) {
					if (F24_EXP(f[k]) == F24_INFNAN)
						flag(1, frame, i + p, c.reg, f[k], "float uniform is Inf or NaN");
					else if (F24_EXP(f[k]) >= F24_HUGE)
						flag(2, frame, i + p, c.reg, f[k], "float uniform is absurdly large");
				}
			}
			continue;
		}

		if (c.reg == 0x200u) {
			base = PARAM(i, 0) << 3;
			// BufInfo_Add reaches the GPU as one consecutive write from 0x200: base, then the
			// two format words, then the per-buffer triples. Parameter 3 is buffer 0's offset,
			// which is the one every chunk draw actually uses.
			if (c.consec && c.nparams >= 4u) voff = PARAM(i, 3u);
			continue;
		}
		if (c.reg == 0x203u) { voff = PARAM(i, 0); continue; }

		if (c.reg == 0x228u) {
			nverts = PARAM(i, 0);
			if (nverts == 0u || nverts > 0x100000u)
				flag(3, frame, i, c.reg, nverts, "implausible vertex count");
			continue;
		}

		if (c.reg == 0x227u) {
			const u32 off = PARAM(i, 0) & 0x7FFFFFFFu;
			if (!addrReadable(base + off))
				flag(4, frame, i, c.reg, base + off, "index buffer is not VRAM or FCRAM");
			continue;
		}

		if (c.reg == 0x22Fu || c.reg == 0x22Eu) {
			if (!addrReadable(base + voff))
				flag(5, frame, i, c.reg, base + voff, "vertex buffer is not VRAM or FCRAM");
			continue;
		}
	}

	#undef PARAM

	if (s_vcode == before) { s_vframes_ok++; return true; }
	return false;
}

// A red-and-green pair for the validator, run once at pre-flight against two hand-built lists
// that differ in exactly one field.
//
// This is here because the validator's normal output is "nothing flagged", and a checker that has
// only ever been seen agreeing with healthy data has not been tested. If the exponent check were
// written against the wrong bits, or PARAM indexed the header instead of the value, the green run
// would look identical to a correct one and every clean frame it reported afterwards would be
// worthless.
//
// The validator's own state is saved and restored around this, so the self-check cannot leave its
// deliberate failure sitting in the report as though a real frame had produced it.
static size_t validatorSelfCheck(char* buf, size_t cap, size_t len)
{
	const int  save_code   = s_vcode;
	const u32  save_frame  = s_vframe, save_word = s_vword, save_reg = s_vreg;
	const u32  save_val    = s_vval,   save_hits = s_vhits, save_ok  = s_vframes_ok;
	char save_what[sizeof(s_vwhat)];
	memcpy(save_what, s_vwhat, sizeof(save_what));
	s_vcode = 0; s_vhits = 0; s_vframes_ok = 0;

	// Six words: a write to 0x2C0 selecting the uniform format, then a consecutive write to 0x2C1
	// (the vertex shader float uniform data register) carrying three parameter words.
	//   word0 = parameter 0
	//   word1 = header: register | extra params << 20 | consecutive << 31
	// The 0x2C0 write is not decoration. Without it the validator does not know the format and
	// skips the burst, so a self-check that omitted it would report PASS on a validator that never
	// looked at anything.
	u32 list[6];
	list[0] = 0x00000000u;                          // 0x2C0 param: bit 31 clear -> float24 mode
	list[1] = 0x2C0u;
	list[2] = 0x00000000u;                          // 0x2C1 param 0
	list[3] = 0x2C1u | (2u << 20) | (1u << 31);
	list[4] = 0x00000000u;                          // param 1
	list[5] = 0x00000000u;                          // param 2

	const bool green = gpuTestValidateList((const uint8_t*)list, sizeof(list), 0);

	// The same list with the first float24's exponent forced to all ones. The four 24-bit fields
	// are sliced out of the 96 bits as w0:w1:w2, so field 0 is the top 24 bits of word0 and an
	// exponent of 0x7F there means Inf or NaN.
	const int before = s_vcode;
	list[2] = 0x7F000000u;
	const bool red = gpuTestValidateList((const uint8_t*)list, sizeof(list), 0);
	const bool caught_infnan = (!red && s_vcode == 1 && s_vcode != before);

	// The float32 pair, which is the mode the hardware is actually in. This exists because the
	// float24 pair above passed for ten builds while the shipped validator was decoding real
	// float32 uploads as float24 and reporting hundreds of imaginary NaNs per run.
	//
	// The green word is chosen so it can only pass if the 0x2C0 mode bit is honoured:
	// 0x3F35047F is an ordinary float32 (about 0.70706, exponent 0x7E), but its low byte is 0x7F,
	// which is exactly where the float24 slicing puts field 1's exponent. A validator that ignored
	// the mode bit would flag this word as Inf or NaN. Passing it proves the bit changed the
	// answer, not merely that a healthy list looked healthy.
	s_vcode = 0;
	u32 flist[6];
	flist[0] = 0x80000000u;                         // 0x2C0 param: bit 31 SET -> float32 mode
	flist[1] = 0x2C0u;
	flist[2] = 0x3F35047Fu;                         // ~0.70706f, but float24-toxic
	flist[3] = 0x2C1u | (2u << 20) | (1u << 31);
	flist[4] = 0xBF800000u;                         // -1.0f
	flist[5] = 0x00000000u;                         // 0.0f
	const bool green32 = gpuTestValidateList((const uint8_t*)flist, sizeof(flist), 0);

	// And the same list with a real float32 NaN in it, to prove the f32 path can still go red.
	s_vcode = 0;
	flist[2] = 0x7FC00000u;                         // exponent 0xFF, mantissa set -> NaN
	const bool red32 = gpuTestValidateList((const uint8_t*)flist, sizeof(flist), 0);
	const bool caught_f32 = (!red32 && s_vcode == 1);

	// And once more for the address check: a draw whose vertex buffer offset puts it outside both
	// VRAM and FCRAM. base (register 0x200) is written first, then DRAWELEMENTS.
	s_vcode = 0;
	u32 dlist[8];
	dlist[0] = 0x03000000u;                              // 0x200 param: base >> 3 -> 0x18000000
	dlist[1] = 0x200u | (3u << 20) | (1u << 31);         // consecutive, 4 params total
	dlist[2] = 0x00000000u;                              // format low
	dlist[3] = 0x00000000u;                              // format high
	dlist[4] = 0x70000000u;                              // buffer 0 offset: lands nowhere real
	dlist[5] = 0x00000000u;                              // padding to an even word count
	dlist[6] = 0x00000000u;                              // 0x22F param
	dlist[7] = 0x22Fu;                                   // DRAWELEMENTS, one param
	const bool red2 = gpuTestValidateList((const uint8_t*)dlist, sizeof(dlist), 0);
	const bool caught_addr = (!red2 && s_vcode == 5);

	const bool pass = green && caught_infnan && caught_addr && green32 && caught_f32;
	len = app(buf, cap, len,
	          "%-34s %-8s %8s   %s\n",
	          "9  validator red/green pair", pass ? "PASS" : "FAILED", "-",
	          pass ? "flags Inf in both float formats, flags a bad address"
	               : "the per-frame list check is NOT trustworthy in this build");
	if (!pass)
		len = app(buf, cap, len,
		          "     f24 clean accepted: %s   f24 Inf caught: %s   bad address caught: %s\n"
		          "     f32 clean accepted: %s   f32 NaN caught: %s\n"
		          "     Treat every \"nothing flagged\" this build reports as meaningless.\n",
		          green ? "yes" : "NO", caught_infnan ? "yes" : "NO", caught_addr ? "yes" : "NO",
		          green32 ? "yes" : "NO", caught_f32 ? "yes" : "NO");

	s_vcode = save_code; s_vframe = save_frame; s_vword = save_word; s_vreg = save_reg;
	s_vval  = save_val;  s_vhits = save_hits;   s_vframes_ok = save_ok;
	memcpy(s_vwhat, save_what, sizeof(s_vwhat));
	return len;
}

size_t gpuTestValidatorReport(char* buf, size_t cap, size_t len)
{
	len = app(buf, cap, len, "\nlist check   : %lu clean frame(s)",
	          (unsigned long)s_vframes_ok);
	if (!s_vcode) {
		len = app(buf, cap, len, ", nothing flagged.\n"
		          "               Every float uniform was finite, every buffer address was in\n"
		          "               VRAM or FCRAM, every vertex count was plausible.\n");
		return len;
	}
	len = app(buf, cap, len, ", %lu FLAGGED.\n", (unsigned long)s_vhits);
	len = app(buf, cap, len,
	          "  FIRST BAD  : frame %lu  word %lu  reg %03lx  value %08lx\n"
	          "               %s\n",
	          (unsigned long)s_vframe, (unsigned long)s_vword,
	          (unsigned long)s_vreg, (unsigned long)s_vval, s_vwhat);
	return len;
}

// ---------------------------------------------------------------------------------------------
// Replaying a command list. Used by the pre-flight (on a list known to be good, to prove the
// machinery works before it is trusted on a list that is not) and by the post-mortem.

static u32* s_replay;    // linear, LIST_MAX

static bool replayInit(void)
{
	if (!s_replay) s_replay = (u32*)linearAlloc(LIST_MAX);
	return s_replay != NULL;
}

// Submits `nwords` words from s_replay. The caller has already filled and sized it.
static bool replaySubmit(u32 nwords, u32* us)
{
	GSPGPU_FlushDataCache(s_replay, nwords * 4u);
	qBegin();
	GX_ProcessCommandList(s_replay, nwords * 4u, GX_CMDLIST_FLUSH);
	return qEnd(us);
}

// Copies a prefix of `src` into the replay buffer and appends the real list's own tail, so the
// result is still a terminated command list. Truncating at an arbitrary word would produce
// something the GPU would be entitled to hang on for reasons that have nothing to do with the
// bug — which would make the bisection prove the opposite of what it looks like it proves.
//
// `prefix_words` is rounded DOWN to a command boundary. Returns the total word count, or 0 if no
// boundary at or below the request exists.
#define TAIL_WORDS 16u

static u32 buildTruncated(const u32* src, u32 nwords, u32 prefix_words, u32* actual_prefix)
{
	if (nwords <= TAIL_WORDS) return 0;
	const u32 tail_at = nwords - TAIL_WORDS;

	// Walk to the last command boundary at or below prefix_words.
	u32 last = 0;
	Cmd c;
	for (u32 i = 0; cmdAt(src, nwords, i, &c); i += c.words) {
		if (i > prefix_words) break;
		last = i;
	}
	if (!last) return 0;
	if (last + TAIL_WORDS > LIST_MAX / 4u) return 0;

	memcpy(s_replay, src, last * 4u);
	memcpy(s_replay + last, src + tail_at, TAIL_WORDS * 4u);
	*actual_prefix = last;
	return last + TAIL_WORDS;
}

// ---------------------------------------------------------------------------------------------
// A. Pre-flight.

static void selfFlush(size_t len)
{
	watchdogWriteFile(SELFTEST_REL, s_self, len);
}

void gpuTestPreflight(void)
{
	bool n3ds = false;
	APT_CheckNew3DS(&n3ds);

	size_t len = 0;
	len = app(s_self, sizeof(s_self), len,
	          "Blocksmith GPU pre-flight, v%s\n"
	          "================================================================\n"
	          "Every test below submits real work on a private GX queue and waits %lld ms for it.\n"
	          "A test that wedges is recorded as TIMEOUT and the battery carries on, so one boot\n"
	          "returns every answer -- including the answers after the first failure.\n\n"
	          "console      : %s\n"
	          "linear free  : %lu B\n"
	          "vram free    : %lu B\n\n",
	          BLOCKSMITH_VERSION, (long long)(TEST_TIMEOUT_NS / 1000000),
	          n3ds ? "New 3DS" : "Old 3DS",
	          (unsigned long)linearSpaceFree(), (unsigned long)vramSpaceFree());

	if (!scratchInit()) {
		len = app(s_self, sizeof(s_self), len,
		          "SKIPPED ALL: could not allocate %lu B of linear scratch.\n",
		          (unsigned long)SCRATCH_BYTES);
		selfFlush(len);
		return;
	}

	u32 us = 0;
	bool ok;

	#define REPORT(name, note) \
		len = app(s_self, sizeof(s_self), len, "%-34s %-8s %8lu us   %s\n", \
		          (name), ok ? "PASS" : "TIMEOUT", (unsigned long)us, (note)); \
		selfFlush(len)

	// 1. The simplest thing the GPU can be asked to do. If this times out, nothing else in the
	//    report matters and the fault is not in our command list at all.
	qBegin();
	GX_MemoryFill(s_src, 0x00000000u, s_src + (64u * 1024u / 4u), GX_FILL_TRIGGER | GX_FILL_32BIT_DEPTH,
	              NULL, 0, NULL, 0);
	ok = qEnd(&us);
	REPORT("1  memory fill, 64 KB", "the primitive both screen clears use");

	// 2. Two fills in one command, which is how citro3d clears colour and depth together, and
	//    exactly what the two completed entries in v1.1.6's queue were.
	qBegin();
	GX_MemoryFill(s_src, 0x00000000u, s_src + (64u * 1024u / 4u), GX_FILL_TRIGGER | GX_FILL_32BIT_DEPTH,
	              s_dst, 0x00000000u, s_dst + (64u * 1024u / 4u), GX_FILL_TRIGGER | GX_FILL_32BIT_DEPTH);
	ok = qEnd(&us);
	REPORT("2  memory fill, two buffers", "what a real frame's clear is");

	// 3. A big fill. Rules out a size-dependent fault in the fill unit.
	qBegin();
	GX_MemoryFill(s_src, 0x00000000u, s_src + (SCRATCH_BYTES / 4u), GX_FILL_TRIGGER | GX_FILL_32BIT_DEPTH,
	              NULL, 0, NULL, 0);
	ok = qEnd(&us);
	REPORT("3  memory fill, 256 KB", "size dependence");

	// 4. The display transfer, which is the command that never got to run on the frozen console
	//    because our draw list was still in front of it.
	qBegin();
	GX_DisplayTransfer(s_src, GX_BUFFER_DIM(64, 64), s_dst, GX_BUFFER_DIM(64, 64),
	                   GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
	                   GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8));
	ok = qEnd(&us);
	REPORT("4  display transfer 64x64", "the command queued behind the hang");

	// 5. A transfer at the real top-screen geometry, sideways as the hardware wants it.
	qBegin();
	GX_DisplayTransfer(s_src, GX_BUFFER_DIM(240, 400), s_dst, GX_BUFFER_DIM(240, 400),
	                   GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
	                   GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8));
	ok = qEnd(&us);
	REPORT("5  display transfer 240x400", "the real top screen geometry");

	// 6. Texture copy: a straight linear blit, a different engine again.
	qBegin();
	GX_TextureCopy(s_src, 0, s_dst, 0, 64u * 1024u, GX_TRANSFER_RAW_COPY(1));
	ok = qEnd(&us);
	REPORT("6  texture copy, 64 KB", "the third transfer engine");

	// 7. Two hundred fills back to back. The hardware freeze takes ~335 frames to appear, so
	//    anything that only goes wrong after repetition is worth provoking directly. Timed as a
	//    whole; a single timeout inside the run fails the test.
	{
		const u64 t0 = svcGetSystemTick();
		ok = true;
		for (int i = 0; i < 200 && ok; i++) {
			qBegin();
			GX_MemoryFill(s_src, (u32)i, s_src + (4u * 1024u / 4u),
			              GX_FILL_TRIGGER | GX_FILL_32BIT_DEPTH, NULL, 0, NULL, 0);
			u32 one = 0;
			ok = qEnd(&one);
		}
		us = tickUs(svcGetSystemTick() - t0);
	}
	REPORT("7  200 fills back to back", "repetition, the way the real bug arrives");

	// 8. The control, and the most important line in the file. Every test above reports PASS by
	//    the wait returning true, so a battery in which the wait CANNOT return false would report
	//    a clean sweep no matter what the hardware did.
	//
	//    It fills the queue with the largest fill in the battery and then allows the wait one
	//    nanosecond, so the work is real, the queue is as busy as one thread can make it, and the
	//    wait is asked for something it cannot deliver. It should report TIMEOUT.
	//
	//    Two earlier versions of this control reported PASS, and both times the control was the
	//    only line in the report that could have caught the mistake behind it.
	//
	//    v1 queued work on a queue that was never started. v2 started the queue but submitted a
	//    single fill. Rather than guess a third time, gxCmdQueueWait was disassembled out of this
	//    build's own ELF, and it does not do what its name suggests:
	//
	//      001bdaa4  ... deadline = svcGetSystemTick() + timeout
	//                   ldrb r2,[r4]      ; r4 = 0x002babc1, a single GLOBAL byte
	//                   cmp  r2,#0
	//                   beq  1bdb10       ; -> mov r0,#1 ; return TRUE, immediately
	//                   ... gspWaitForAnyEvent(), re-read the byte, and only once the deadline
	//                       has passed:  1bdb40  mov r0,#0 ; return FALSE
	//
	//    It never looks at the queue counters at all. The wait can only return false if that byte
	//    is still set at the moment the wait is entered — i.e. if GX work is genuinely in flight.
	//    An emulator that performs each GX command inline at submission time clears the byte before
	//    the wait is reached, so on a host like that the wait returns true having waited for
	//    nothing, and NO amount of queued work will change it.
	//
	//    That case is not a pass and it is not a failure; it is a control that could not have
	//    fired, so it is reported as INCONCL and never as PASS. The queue's own counters are
	//    sampled immediately before the wait so the report can say which of the two it was.
	{
		u16 queued = 0, submitted = 0, completed = 0;

		qBegin();
		for (int i = 0; i < (int)(sizeof(s_entries) / sizeof(s_entries[0])); i++)
			GX_MemoryFill(s_src, (u32)i, s_src + (SCRATCH_BYTES / 4u),
			              GX_FILL_TRIGGER | GX_FILL_32BIT_DEPTH, NULL, 0, NULL, 0);

		queued    = s_q.numEntries;
		submitted = s_q.curEntry;
		completed = s_q.lastEntry;
		ok = qEndT(&us, 1LL);

		len = app(s_self, sizeof(s_self), len, "%-34s %-8s %8lu us   %s\n",
		          "8  CONTROL: 1 ns timeout",
		          ok ? "INCONCL" : "TIMEOUT", (unsigned long)us,
		          ok ? "<- could not fire here, see note below"
		             : "<- correct: the timeout can fire");
		len = app(s_self, sizeof(s_self), len,
		          "     queue when the wait began : queued %u  submitted %u  completed %u  -> %s\n",
		          (unsigned)queued, (unsigned)submitted, (unsigned)completed,
		          (completed != queued) ? "work was still outstanding"
		                                : "the whole queue had already run");
		selfFlush(len);
	}

	// Let the fills above finish before anything else is submitted, so a deliberately abandoned
	// wait cannot leave work in flight and make test 9 or the game's first frame look slow.
	qDrain();

	// 9. The per-frame list validator, checked against data it must accept and data it must
	//    reject. Costs nothing and touches no hardware.
	len = validatorSelfCheck(s_self, sizeof(s_self), len);
	selfFlush(len);

	len = app(s_self, sizeof(s_self), len,
	          "\nTest 8 is the control, and it is the one that has to FAIL. It fills the queue with\n"
	          "eight 256 KB memory fills and then allows the wait one nanosecond.\n"
	          "  TIMEOUT -> the wait can report a failure, so every PASS above it means something,\n"
	          "             and so does the two-second wait the frame loop now uses.\n"
	          "  INCONCL -> the wait returned true without ever waiting. gxCmdQueueWait returns\n"
	          "             immediately when libctru's GX-busy byte is already clear, which is what\n"
	          "             happens on a host that runs each GX command inline at submission. It is\n"
	          "             expected on an emulator and it means this control proved nothing here.\n"
	          "             On a real console it should read TIMEOUT.\n"
	          "\nThe command-list tests are not here: they need a real list, so they run on the\n"
	          "first frame the world draws and are appended below.\n");
	selfFlush(len);

	#undef REPORT
}

// ---------------------------------------------------------------------------------------------
// The command-list half of the pre-flight, run once on the first real in-world list.
//
// This is what earns the right to believe the post-mortem. If replaying a list the GPU has
// ALREADY executed successfully were to wedge, the replay machinery would be the bug and every
// conclusion drawn from it downstream would be backwards.

static bool s_probe_done;

void gpuTestListProbe(const uint8_t* list, uint32_t len_bytes)
{
	if (s_probe_done) return;
	s_probe_done = true;

	// Resumes where gpuTestPreflight left off: s_self still holds the whole pre-flight report,
	// null-terminated by the last vsnprintf, and this appends to it and rewrites the file.
	size_t len = strlen(s_self);

	if (!replayInit()) {
		len = app(s_self, sizeof(s_self), len,
		          "\nlist replay  : SKIPPED, could not allocate %lu B linear.\n",
		          (unsigned long)LIST_MAX);
		selfFlush(len);
		return;
	}
	if (!list || len_bytes < 64u || len_bytes > LIST_MAX) {
		len = app(s_self, sizeof(s_self), len,
		          "\nlist replay  : SKIPPED, list is %lu B (need 64 .. %lu).\n",
		          (unsigned long)len_bytes, (unsigned long)LIST_MAX);
		selfFlush(len);
		return;
	}

	const u32 nwords = len_bytes / 4u;
	u32 us = 0;

	len = app(s_self, sizeof(s_self), len,
	          "\n\nCommand-list replay, on the first frame the world drew\n"
	          "================================================================\n"
	          "list         : %lu bytes / %lu words\n", (unsigned long)len_bytes,
	          (unsigned long)nwords);

	// R1. Replay it once, unchanged. This frame's list has already been executed by the GPU, so
	//     the only acceptable result is PASS. A TIMEOUT here means the replay is broken.
	memcpy(s_replay, list, len_bytes);
	bool ok = replaySubmit(nwords, &us);
	len = app(s_self, sizeof(s_self), len,
	          "R1 replay unchanged                %-8s %8lu us   %s\n",
	          ok ? "PASS" : "TIMEOUT", (unsigned long)us,
	          ok ? "replay machinery works" : "<- REPLAY IS BROKEN, ignore R2/R3 and the bisect");
	selfFlush(len);

	// R2. Replay it fifty times. Proves repetition of one list is not itself the trigger.
	{
		const u64 t0 = svcGetSystemTick();
		bool all = true;
		for (int i = 0; i < 50 && all; i++) {
			memcpy(s_replay, list, len_bytes);
			u32 one = 0;
			all = replaySubmit(nwords, &one);
		}
		us = tickUs(svcGetSystemTick() - t0);
		len = app(s_self, sizeof(s_self), len,
		          "R2 replay x50                      %-8s %8lu us   %s\n",
		          all ? "PASS" : "TIMEOUT", (unsigned long)us, "repetition of one good list");
		selfFlush(len);
	}

	// R3. Replay it truncated to half, rebuilt with its own tail. This is precisely the
	//     construction the bisector uses on the frozen list, so it has to be shown to produce a
	//     list the GPU accepts when the input is good. If a HALVED GOOD list wedges, then every
	//     TIMEOUT the bisector reports later would be an artefact of the truncation and not
	//     evidence about the bug at all.
	{
		u32 pre = 0;
		const u32 tot = buildTruncated((const u32*)(const void*)list, nwords, nwords / 2u, &pre);
		if (!tot) {
			len = app(s_self, sizeof(s_self), len,
			          "R3 replay half + tail              SKIPPED           no command boundary\n");
		} else {
			ok = replaySubmit(tot, &us);
			len = app(s_self, sizeof(s_self), len,
			          "R3 replay half + tail              %-8s %8lu us   prefix %lu w, total %lu w\n",
			          ok ? "PASS" : "TIMEOUT", (unsigned long)us,
			          (unsigned long)pre, (unsigned long)tot);
			if (!ok)
				len = app(s_self, sizeof(s_self), len,
				          "   ^ A GOOD list, halved, wedged. The bisector's truncation is unsound on\n"
				          "     this hardware and its verdict must be discarded.\n");
		}
		selfFlush(len);
	}
}

// ---------------------------------------------------------------------------------------------
// C. The post-mortem.

// The red arm for everything below, and nothing else uses it.
//
// The post-mortem only ever runs on a console that has frozen, which is not a state an emulator
// can be put into — that is the entire reason this bug has taken seven builds. So the machinery
// has to be provable some other way, or it ships never having been executed once: the queue read,
// the file writes, the three steps, the binary search and the command-boundary arithmetic would
// all be running for the first time on the one boot they have to get right.
//
//   -DBS_GPU_FAKE_WEDGE=<frame>   the timed wait reports a wedge on that frame, though the GPU
//                                 is healthy. Steps 1 and 2 then run for real against a working
//                                 GPU and must report ALIVE and replayed-cleanly.
//   -DBS_GPU_BISECT_FAKE=<word>   additionally forces step 2 to report a wedge and replaces the
//                                 bisector's submit with "wedges if the prefix reaches this
//                                 word", so the search itself can be checked against a known
//                                 answer. The reported command must be the one containing it.
#ifdef BS_GPU_FAKE_WEDGE
static u32 s_fake_frame;
#endif

bool gpuTestFrameWait(void)
{
	const bool ok = gxCmdQueueWait(C3D_QUEUE, BS_GPU_WEDGE_NS);
#ifdef BS_GPU_FAKE_WEDGE
	if (ok && ++s_fake_frame == (u32)(BS_GPU_FAKE_WEDGE)) return false;
#endif
	return ok;
}

static void postFlush(size_t len)
{
	watchdogWriteFile(POSTMORTEM_REL, s_post, len);
}

// Reads the queue the way watchdog.c's report does, but from the main thread and after the fact.
static size_t queueLines(size_t len, const char* label)
{
	const gxCmdQueue_s* q = C3D_QUEUE;
	if (!q->entries || q->maxEntries == 0 || q->maxEntries > 64) {
		len = app(s_post, sizeof(s_post), len, "%s: unreadable\n", label);
		return len;
	}
	len = app(s_post, sizeof(s_post), len,
	          "%s: cap %u  queued %u  submitted %u  completed %u\n",
	          label, q->maxEntries, q->numEntries, q->curEntry, q->lastEntry);
	for (unsigned i = 0; i < q->numEntries && i < q->maxEntries; i++) {
		const gxCmdEntry_s* e = &q->entries[i];
		const char* name = e->type == 1 ? "ProcessCommandList" :
		                   e->type == 2 ? "MemoryFill" :
		                   e->type == 3 ? "DisplayTransfer" :
		                   e->type == 4 ? "TextureCopy" : "?";
		len = app(s_post, sizeof(s_post), len,
		          "    [%u] %-18s %08lx %08lx %08lx %08lx%s\n", i, name,
		          (unsigned long)e->args[0], (unsigned long)e->args[1],
		          (unsigned long)e->args[2], (unsigned long)e->args[3],
		          i == q->lastEntry ? "   <== STUCK HERE" : "");
	}
	return len;
}

void gpuTestPostMortem(uint32_t frame)
{
	static bool done;
	if (done) return;
	done = true;

	size_t len = 0;
	len = app(s_post, sizeof(s_post), len,
	          "Blocksmith GPU post-mortem, v%s\n"
	          "================================================================\n"
	          "The frame loop waited %lld ms for the GX queue and it did not drain. The main\n"
	          "thread is alive and the GPU is still stuck in exactly the state that stuck it,\n"
	          "which is the only moment any of the questions below can be asked.\n\n"
	          "frame        : %lu\n"
	          "linear free  : %lu B\n"
	          "vram free    : %lu B\n\n",
	          BLOCKSMITH_VERSION, (long long)(BS_GPU_WEDGE_NS / 1000000),
	          (unsigned long)frame, (unsigned long)linearSpaceFree(),
	          (unsigned long)vramSpaceFree());
	len = queueLines(len, "queue at wedge");
	postFlush(len);

	// The frozen list itself, taken from the entry the queue is stuck on.
	const gxCmdQueue_s* q = C3D_QUEUE;
	const u8* hang_src = NULL;
	u32 hang_addr = 0, hang_size = 0;
	const char* hang_from = "the live GX queue entry";
	if (q->entries && q->maxEntries && q->maxEntries <= 64) {
		for (unsigned i = 0; i < q->numEntries && i < q->maxEntries; i++) {
			if (q->entries[i].type != 1) continue;
			hang_addr = q->entries[i].args[0];
			hang_size = q->entries[i].args[1];
			hang_src  = (const u8*)hang_addr;
			break;
		}
	}
#if BS_DRAW_PROBE
	// The queue held no draw command. On the console that froze it held five, so this is not the
	// expected path — but a post-mortem that gives up here would waste the boot, and the
	// watchdog's ring has a copy of the same frame's list.
	if (!hang_src) {
		u32 cap_len = 0, cap_addr = 0;
		const u8* cap = watchdogLastCmdList(&cap_len, &cap_addr);
		if (cap && cap_len) {
			hang_src  = cap;
			hang_addr = cap_addr;
			hang_size = cap_len;
			hang_from = "the watchdog's captured copy (the GX queue held no draw command)";
		}
	}
#endif
	len = app(s_post, sizeof(s_post), len, "\nlist source  : %s\n", hang_from);
	postFlush(len);

	// -- Step 1. Is the GPU wedged, or is it only this list? ------------------------------------
	//
	// A memory fill is the simplest work the hardware accepts and shares none of the vertex
	// pipeline with a draw. If this completes, the GPU is alive and the draw list is poison. If
	// it times out as well, the whole engine is stopped and the list may be entirely innocent.
	// Nothing else in this file changes the answer to that as much as this one line does.
	if (scratchInit()) {
		u32 us = 0;
		qBegin();
		GX_MemoryFill(s_src, 0x0u, s_src + (16u * 1024u / 4u),
		              GX_FILL_TRIGGER | GX_FILL_32BIT_DEPTH, NULL, 0, NULL, 0);
		const bool alive = qEnd(&us);
		len = app(s_post, sizeof(s_post), len,
		          "\n1 trivial fill while wedged        %-8s %8lu us\n"
		          "  %s\n",
		          alive ? "PASS" : "TIMEOUT", (unsigned long)us,
		          alive ? "GPU IS ALIVE. It still executes work. The stuck list is the problem."
		                : "GPU IS GLOBALLY WEDGED. Nothing executes. The list may be innocent.");
		postFlush(len);
		if (!alive) {
			len = app(s_post, sizeof(s_post), len,
			          "\nSteps 2 and 3 skipped: replaying a list into a stopped GPU cannot say\n"
			          "anything about the list.\n");
			postFlush(len);
			return;
		}
	}

	if (!hang_src || !hang_size || hang_size > LIST_MAX || !replayInit()) {
		len = app(s_post, sizeof(s_post), len,
		          "\nSteps 2 and 3 skipped: stuck list is %lu B at %08lx (need 1 .. %lu and a\n"
		          "linear replay buffer).\n",
		          (unsigned long)hang_size, (unsigned long)hang_addr, (unsigned long)LIST_MAX);
		postFlush(len);
		return;
	}

	// Take a private copy immediately: everything below submits work, and the original lives in
	// citro3d's own double-buffered command memory.
	static u32 s_hang[LIST_MAX / 4u];
	const u32 nwords = hang_size / 4u;
	memcpy(s_hang, hang_src, hang_size);
	watchdogWriteFile(BISECT_REL, s_hang, hang_size);

	// -- Step 2. Is it reproducible? ------------------------------------------------------------
	//
	// A list that wedges once and then replays cleanly is not a poisoned list; it is a timing or
	// ordering fault, and the bisection below would chase a value that is not there.
	{
		u32 us = 0;
		memcpy(s_replay, s_hang, hang_size);
		bool ok = replaySubmit(nwords, &us);
#ifdef BS_GPU_BISECT_FAKE
		ok = false;   // red arm: force the search below to run, see BS_GPU_FAKE_WEDGE above
#endif
		len = app(s_post, sizeof(s_post), len,
		          "\n2 replay the stuck list            %-8s %8lu us\n  %s\n",
		          ok ? "PASS" : "TIMEOUT", (unsigned long)us,
		          ok ? "IT REPLAYED CLEANLY. The list's contents are not sufficient on their own:\n"
		               "  the fault needs the state the GPU was in, or it is a race. Do not bisect."
		             : "IT WEDGED AGAIN. The list is reproducibly poison, so it can be bisected.");
		postFlush(len);
		if (ok) {
			len = app(s_post, sizeof(s_post), len,
			          "\nStep 3 skipped: nothing to bisect if the list runs.\n");
			postFlush(len);
			return;
		}
	}

	// -- Step 3. Which command? -----------------------------------------------------------------
	//
	// Binary search on the prefix length. Each candidate is a prefix of the real list with the
	// real list's own last 16 words appended, so it stays a properly terminated list -- R3 in the
	// pre-flight is what establishes that this construction is sound on this hardware.
	//
	// Invariant: `good` is a prefix length known to run, `bad` one known to wedge. The answer is
	// the command that begins at `good`, because adding exactly that one command is what turns a
	// list that runs into a list that does not.
	len = app(s_post, sizeof(s_post), len,
	          "\n3 bisecting %lu words\n"
	          "  each step is a prefix plus the real list's own last %lu words, so it stays a\n"
	          "  valid, terminated list.\n\n"
	          "    %-10s %-10s %-8s %s\n",
	          (unsigned long)nwords, (unsigned long)TAIL_WORDS,
	          "prefix w", "total w", "result", "us");
	postFlush(len);

	// A truncation is only a valid list if it ends on a command boundary, so the search runs over
	// the list's boundaries rather than over word counts.
	//
	// That distinction is not pedantry, it is a bug this code had. The first version bisected word
	// counts and rounded each midpoint down to a boundary, which sounds equivalent. It is not: once
	// the rounded prefix stopped advancing, the loop moved `good` forward WITHOUT SUBMITTING
	// ANYTHING, purely so the search would terminate. The emulator run that exposed it measured its
	// last PASS at 974 words and the report then named the command at word 1011 — a figure nothing
	// had ever tested. On hardware that would have sent the whole investigation after the wrong
	// command, in a report that looked exactly as confident as a correct one.
	//
	// Enumerating the boundaries up front makes every step of the search a real submission, and the
	// closing invariant exact: b[lo] ran, b[hi] wedged, and they are adjacent commands.
	static u32 s_bounds[LIST_MAX / 8u];   // a command is at least two words
	const u32 bounds_cap = (u32)(sizeof(s_bounds) / sizeof(s_bounds[0]));
	u32 nb = 0;
	{
		Cmd e;
		for (u32 i = 0; nb + 1u < bounds_cap && cmdAt(s_hang, nwords, i, &e); i += e.words)
			s_bounds[nb++] = i;
		s_bounds[nb++] = nwords;   // the whole list, already known to wedge from step 2
	}

	if (nb < 3u) {
		len = app(s_post, sizeof(s_post), len,
		          "\n  Not bisectable: the list walks as %lu commands.\n", (unsigned long)(nb - 1u));
		postFlush(len);
		return;
	}

	u32 lo = 0, hi = nb - 1u;   // b[lo] runs (b[0] is the empty prefix), b[hi] wedges
	for (int step = 0; step < 24 && hi > lo + 1u; step++) {
		const u32 mi = lo + (hi - lo) / 2u;
		u32 pre = 0;
		const u32 tot = buildTruncated(s_hang, nwords, s_bounds[mi], &pre);
		if (!tot) {
			// Only reachable on a list long enough that prefix + tail will not fit the replay
			// buffer. Stop rather than move an endpoint on no evidence; the bracket printed below
			// is then simply wider, and every line of it was measured.
			len = app(s_post, sizeof(s_post), len,
			          "    %-10lu %-10s %-8s  prefix + tail exceeds the replay buffer\n",
			          (unsigned long)s_bounds[mi], "-", "SKIP");
			postFlush(len);
			break;
		}

		u32 us = 0;
#ifdef BS_GPU_BISECT_FAKE
		// Red arm: a prefix wedges exactly when it reaches the nominated word. Substituting the
		// predicate rather than the search is the point — it leaves the binary search, the
		// boundary enumeration and the reporting running unmodified against an answer that is
		// known in advance.
		const bool ok = (pre <= (u32)(BS_GPU_BISECT_FAKE));
#else
		const bool ok = replaySubmit(tot, &us);
#endif
		len = app(s_post, sizeof(s_post), len, "    %-10lu %-10lu %-8s %lu\n",
		          (unsigned long)pre, (unsigned long)tot, ok ? "PASS" : "TIMEOUT",
		          (unsigned long)us);
		postFlush(len);
		if (ok) lo = mi; else hi = mi;
	}

	const u32 good = s_bounds[lo];
	const u32 bad  = s_bounds[hi];

	len = app(s_post, sizeof(s_post), len,
	          "\n  list walks as              : %lu commands\n"
	          "  shortest prefix that wedges : %lu words (command %lu)\n"
	          "  longest prefix that runs    : %lu words (command %lu)%s\n",
	          (unsigned long)(nb - 1u),
	          (unsigned long)bad, (unsigned long)hi,
	          (unsigned long)good, (unsigned long)lo,
	          lo == 0u ? "  <- the empty prefix; not submitted, assumed good" : "");

	// Name the command that sits on the boundary, and print it raw. Adding exactly this one command
	// is what turns a list that runs into a list that does not.
	Cmd c;
	if (hi == lo + 1u && cmdAt(s_hang, nwords, good, &c)) {
		len = app(s_post, sizeof(s_post), len,
		          "\n  THE COMMAND AT WORD %lu IS THE ONE THE GPU CANNOT EXECUTE:\n"
		          "    register  : %03lx\n"
		          "    params    : %lu%s\n"
		          "    raw       :",
		          (unsigned long)good, (unsigned long)c.reg,
		          (unsigned long)c.nparams, c.consec ? " (consecutive write)" : "");
		for (u32 k = 0; k < c.words && k < 12u; k++)
			len = app(s_post, sizeof(s_post), len, " %08lx", (unsigned long)s_hang[good + k]);
		len = app(s_post, sizeof(s_post), len, "\n");
	} else {
		len = app(s_post, sizeof(s_post), len,
		          "\n  The search did not close to a single command, so no command is named here.\n"
		          "  The culprit is inside words %lu .. %lu of bisect.bin.\n",
		          (unsigned long)good, (unsigned long)bad);
	}
	len = app(s_post, sizeof(s_post), len,
	          "\n  bisect.bin holds the whole stuck list as it was replayed.\n");
	postFlush(len);
}

#endif // BS_GPU_TESTS
