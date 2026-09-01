#include "app/heapsplit.h"

// Page granularity. svcControlMemory works in 4 KB pages and libctru masks its own figures the
// same way; an unaligned request is not a rounding inconvenience, it is a failed allocation.
#define PAGE_MASK (~0xFFFu)

bool heapSplitChoose(unsigned remaining, unsigned* out_linear, unsigned* out_app)
{
	if (!out_linear || !out_app) return false;

	const unsigned linear = HEAPSPLIT_LINEAR_TARGET_BYTES & PAGE_MASK;

	// Refuse rather than clamp. A clamp here would quietly hand back a smaller linear heap on a
	// console that cannot afford the target, and the caller would have no way to tell that from
	// success — it would boot, the render distance would silently not be available, and the
	// only symptom would be a number nobody reads. Declining says so, and libctru's own split
	// then runs untouched, which is the behaviour every Old 3DS has shipped with.
	if (remaining < linear) return false;
	if (remaining - linear < HEAPSPLIT_APP_FLOOR_BYTES) return false;

	*out_linear = linear;
	*out_app    = (remaining - linear) & PAGE_MASK;
	return true;
}

// The libctru hook. Guarded because the policy above is host-testable and this is not: it is
// svc calls and process globals, and tests/heapsplit_test.c links the file for the function
// above alone.
#ifdef __3DS__

#include <3ds/svc.h>
#include <3ds/allocator/mappable.h>
#include <3ds/env.h>
#include <3ds/os.h>
#include <3ds/result.h>

extern char* fake_heap_start;
extern char* fake_heap_end;

extern u32 __ctru_heap;
extern u32 __ctru_linear_heap;
extern u32 __ctru_heap_size;
extern u32 __ctru_linear_heap_size;

// Overrides libctru's weak __system_allocateHeaps (nm reports it 'W'). The structure below is
// libctru's own function from libctru/source/system/allocateHeaps.c, deliberately kept
// line-for-line where it does the same job — the resource-limit query, the page mask, the two
// svcControlMemory calls, the newlib heap fixup — because this runs before main, before any
// service is up, and before the crash handler exists. The ONLY thing changed is the policy
// block that picks the two sizes.
//
// Setting __ctru_linear_heap_size from a constructor instead does not work and is not a style
// preference: allocateHeaps runs before .init_array, so a constructor is simply too late. And
// setting it as a strong global is what the Old 3DS red arm did — libctru's guard then panics.
void __system_allocateHeaps(void)
{
	Result rc;

	Handle reslimit = 0;
	rc = svcGetResourceLimit(&reslimit, CUR_PROCESS_HANDLE);
	if (R_FAILED(rc))
		svcBreak(USERBREAK_PANIC);

	s64 maxCommit = 0, currentCommit = 0;
	ResourceLimitType reslimitType = RESLIMIT_COMMIT;
	svcGetResourceLimitLimitValues(&maxCommit, reslimit, &reslimitType, 1);
	svcGetResourceLimitCurrentValues(&currentCommit, reslimit, &reslimitType, 1);
	svcCloseHandle(reslimit);

	// What the process was actually granted, page aligned. This is the number that tells the
	// two consoles apart without asking anything: ~126.8 MB where the ExHeader got 124 MB mode,
	// ~63.9 MB where it got the Old 3DS 64 MB. Nothing else is available this early.
	u32 remaining = (u32)(maxCommit - currentCommit) & ~0xFFFu;

	unsigned want_linear = 0, want_app = 0;
	if (heapSplitChoose(remaining, &want_linear, &want_app)) {
		__ctru_linear_heap_size = want_linear;
		__ctru_heap_size        = want_app;
	} else {
		// libctru's default policy, reproduced because falling through to it is not possible
		// once this function is overridden. Caps are its HEAP_SPLIT_SIZE_CAP (24 MB) and
		// LINEAR_HEAP_SIZE_CAP (32 MB); on an Old 3DS this lands on linear 33,554,432 and app
		// heap 30,355,456, which is byte for byte what the unmodified build measured.
		__ctru_linear_heap_size = (remaining / 2) & ~0xFFFu;
		__ctru_heap_size        = remaining - __ctru_linear_heap_size;
		if (__ctru_heap_size > (24u << 20)) {
			__ctru_heap_size        = 24u << 20;
			__ctru_linear_heap_size = remaining - __ctru_heap_size;
			if (__ctru_linear_heap_size > (32u << 20)) {
				__ctru_linear_heap_size = 32u << 20;
				__ctru_heap_size        = remaining - __ctru_linear_heap_size;
			}
		}
	}

	// The same guard libctru keeps, kept for the same reason: exceeding the grant is what makes
	// svcControlMemory fail, and failing here with a panic is far easier to diagnose than a
	// half-mapped heap.
	if (__ctru_heap_size + __ctru_linear_heap_size > remaining)
		svcBreak(USERBREAK_PANIC);

	rc = svcControlMemory(&__ctru_heap, OS_HEAP_AREA_BEGIN, 0x0, __ctru_heap_size,
	                      MEMOP_ALLOC, MEMPERM_READ | MEMPERM_WRITE);
	if (R_FAILED(rc))
		svcBreak(USERBREAK_PANIC);

	rc = svcControlMemory(&__ctru_linear_heap, 0x0, 0x0, __ctru_linear_heap_size,
	                      MEMOP_ALLOC_LINEAR, MEMPERM_READ | MEMPERM_WRITE);
	if (R_FAILED(rc))
		svcBreak(USERBREAK_PANIC);

	mappableInit(OS_MAP_AREA_BEGIN, OS_MAP_AREA_END);

	fake_heap_start = (char*)__ctru_heap;
	fake_heap_end   = fake_heap_start + __ctru_heap_size;
}

#endif // __3DS__
