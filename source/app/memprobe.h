// What the boot actually costs, measured on the console it is running on.
//
// Every memory figure in this project is arithmetic on paper. The mesh pool is 9,199,616
// bytes because scene/render_dist.h multiplies four constants together; the world store is
// 12 MB because world/budget.h says so; the total, 24,348,874 bytes, is a sum of fourteen
// such derivations. Not one of them has been read back off a console since before v1.7.0,
// and the reading that exists (25.83 MB of linear free) was taken when the pool was 4.24 MB.
//
// That matters right now because v1.8.5 is being asked to raise the render distance on a New
// 3DS, and the honest answer to "how far can it go" is a subtraction from a number nobody has
// measured. So: mark the free heap at each boot milestone, subtract adjacent marks, and the
// difference is what that milestone cost — not what it was calculated to cost.
//
// Deliberately free of <3ds.h>, which is the whole reason the readings are passed IN rather
// than taken here. linearSpaceFree() and vramSpaceFree() are libctru; the arithmetic, the
// overflow behaviour and the formatting are not, and the host suite tests all three. main.c
// is the only caller and the only place the two worlds meet.
#pragma once

#include <stdbool.h>
#include <stddef.h>

// Boot milestones worth a mark. Nine is three more than main.c currently uses; the cap
// exists so the struct is a fixed size and marking can never allocate, because a probe that
// allocates is measuring itself.
#define MEMPROBE_MAX_STAGES 9

// One milestone's three readings.
//
// Note the mixed polarity, which is not an oversight and is why the cost accessors exist
// instead of callers subtracting fields themselves: `linear` and `vram` are FREE space and go
// DOWN as memory is consumed, while `heap` is bytes IN USE and goes UP. memProbeLinearCost,
// memProbeVramCost and memProbeHeapCost all normalise to the same convention — positive means
// this milestone consumed memory — so nothing downstream has to remember which is which.
typedef struct {
	const char* name;    // static string; the milestone that had just finished
	unsigned    linear;  // linearSpaceFree() immediately after it — FREE bytes
	unsigned    vram;    // vramSpaceFree() immediately after it — FREE bytes
	unsigned    heap;    // mallinfo().uordblks immediately after it — bytes IN USE
} MemProbeStage;

typedef struct {
	MemProbeStage stage[MEMPROBE_MAX_STAGES];
	int           count;
	bool          overflowed;  // a mark was dropped; the totals below are still honest
} MemProbe;

void memProbeReset(MemProbe* p);

// Records one milestone. False if the table was full, in which case nothing is stored and
// `overflowed` is set — a dropped mark must not silently shift the costs of the marks around
// it, so callers get told and the report says so out loud.
bool memProbeMark(MemProbe* p, const char* name,
                  unsigned linear, unsigned vram, unsigned heap);

// Bytes stage `i` consumed: the drop in free memory between stage i-1 and stage i. Stage 0 is
// the baseline and by definition costs nothing. Out of range returns 0.
//
// Signed, and that is not defensive padding: a stage can FREE memory (gpuTestPreflight
// releases its scratch, screenInit's second eye is conditional), and an unsigned subtraction
// would turn a 512 KB release into 4,294,443,776 bytes of cost and read as a catastrophe.
long memProbeLinearCost(const MemProbe* p, int i);
long memProbeVramCost(const MemProbe* p, int i);

// Same convention — positive means consumed — off a reading that counts the other way. This
// is the one that can see the allocations linearSpaceFree() is blind to: net/blockdiff.h's
// 1.02 MB pending-edit store and everything else that reaches for malloc rather than
// linearAlloc, which is most of the game and all of the two claims v1.8.5 is reclaiming.
long memProbeHeapCost(const MemProbe* p, int i);

// Consumed between the first mark and the last. Zero if fewer than two marks were taken.
long memProbeLinearTotal(const MemProbe* p);
long memProbeVramTotal(const MemProbe* p);
long memProbeHeapTotal(const MemProbe* p);

// The whole report as text, newline separated, NUL terminated. Returns the number of
// characters written excluding the NUL, or -1 if `cap` was too small to hold all of it — a
// truncated memory report is worse than none, because the missing line is the one that would
// have explained the total.
int memProbeFormat(const MemProbe* p, char* out, size_t cap);

// Cap for a `memProbeFormat` buffer that can never truncate.
//
// The widest line the formatter can emit is the per-stage one: two spaces, a name padded to
// MEMPROBE_NAME_MAX, then three labelled ten-digit readings, then "cost" and three signed
// longs of up to eleven characters each with separators, and a newline. The host suite
// measures it rather than trusting this paragraph — see testNoLineExceedsTheLineBudget, which
// prints the number it measured so the constant below can be set from it.
//
// 80 was the first value here. Set back to 80 the suite still PASSED, which is the useful part
// of this note: MEMPROBE_REPORT_MAX carries four spare lines of slack, and that slack silently
// absorbed a per-line budget that was 14 characters short of the real worst line. The total
// fitting was luck, not design. So the host suite now asserts the invariant the arithmetic
// actually rests on -- that no single emitted line exceeds MEMPROBE_LINE_MAX -- and that check
// does go red at 80. See tests/memprobe_test.c, testNoLineExceedsTheLineBudget.
#define MEMPROBE_NAME_MAX   24
#define MEMPROBE_LINE_MAX   192
#define MEMPROBE_REPORT_MAX ((MEMPROBE_MAX_STAGES + 4) * MEMPROBE_LINE_MAX)
