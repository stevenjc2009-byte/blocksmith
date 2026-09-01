#include "app/memprobe.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void memProbeReset(MemProbe* p)
{
	if (!p) return;
	memset(p, 0, sizeof(*p));
}

bool memProbeMark(MemProbe* p, const char* name,
                  unsigned linear, unsigned vram, unsigned heap)
{
	if (!p) return false;
	if (p->count >= MEMPROBE_MAX_STAGES) {
		p->overflowed = true;
		return false;
	}
	p->stage[p->count].name   = name ? name : "?";
	p->stage[p->count].linear = linear;
	p->stage[p->count].vram   = vram;
	p->stage[p->count].heap   = heap;
	p->count++;
	return true;
}

// Both costs are the same subtraction on a different field, and both have to widen to a
// signed type BEFORE subtracting rather than after: `(long)(a - b)` on unsigned operands has
// already wrapped by the time the cast runs, and on a 32-bit long it stays wrapped.
static long dropped(unsigned before, unsigned after)
{
	return (long)before - (long)after;
}

long memProbeLinearCost(const MemProbe* p, int i)
{
	if (!p || i <= 0 || i >= p->count) return 0;
	return dropped(p->stage[i - 1].linear, p->stage[i].linear);
}

long memProbeVramCost(const MemProbe* p, int i)
{
	if (!p || i <= 0 || i >= p->count) return 0;
	return dropped(p->stage[i - 1].vram, p->stage[i].vram);
}

// The operands are the other way round on purpose: `heap` is bytes IN USE, so consumption is
// a RISE, where linear and vram consumption is a FALL. Both end up positive-means-consumed,
// which is the only thing a reader of the report should have to know.
long memProbeHeapCost(const MemProbe* p, int i)
{
	if (!p || i <= 0 || i >= p->count) return 0;
	return dropped(p->stage[i].heap, p->stage[i - 1].heap);
}

long memProbeLinearTotal(const MemProbe* p)
{
	if (!p || p->count < 2) return 0;
	return dropped(p->stage[0].linear, p->stage[p->count - 1].linear);
}

long memProbeVramTotal(const MemProbe* p)
{
	if (!p || p->count < 2) return 0;
	return dropped(p->stage[0].vram, p->stage[p->count - 1].vram);
}

long memProbeHeapTotal(const MemProbe* p)
{
	if (!p || p->count < 2) return 0;
	return dropped(p->stage[p->count - 1].heap, p->stage[0].heap);
}

// snprintf into a shrinking window, checking every return. The alternative -- formatting into
// a scratch line and strcat-ing -- is what the truncation contract exists to avoid, because
// strcat cannot tell the caller it ran out of room.
static bool appendf(char** out, size_t* left, const char* fmt, ...)
	__attribute__((format(printf, 3, 4)));

static bool appendf(char** out, size_t* left, const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	const int n = vsnprintf(*out, *left, fmt, ap);
	va_end(ap);

	// vsnprintf returns what it WOULD have written, so n == *left means the NUL was dropped.
	if (n < 0 || (size_t)n >= *left) return false;
	*out  += n;
	*left -= (size_t)n;
	return true;
}

int memProbeFormat(const MemProbe* p, char* out, size_t cap)
{
	if (!p || !out || cap == 0) return -1;

	char*  w    = out;
	size_t left = cap;
	*w = '\0';

	if (!appendf(&w, &left, "boot memory probe: %d marks%s\n",
	             p->count, p->overflowed ? " (TABLE FULL, marks dropped)" : "")) {
		return -1;
	}

	for (int i = 0; i < p->count; i++) {
		// Stage 0 has no predecessor, so it prints its readings and no cost. Printing a
		// cost of 0 there would read as "this stage was free", which is a different claim.
		if (i == 0) {
			if (!appendf(&w, &left,
			             "  %-*s free lin %10u vram %8u  used heap %10u  (baseline)\n",
			             MEMPROBE_NAME_MAX, p->stage[i].name,
			             p->stage[i].linear, p->stage[i].vram, p->stage[i].heap)) {
				return -1;
			}
			continue;
		}
		if (!appendf(&w, &left,
		             "  %-*s free lin %10u vram %8u  used heap %10u"
		             "  cost lin %+ld vram %+ld heap %+ld\n",
		             MEMPROBE_NAME_MAX, p->stage[i].name,
		             p->stage[i].linear, p->stage[i].vram, p->stage[i].heap,
		             memProbeLinearCost(p, i), memProbeVramCost(p, i),
		             memProbeHeapCost(p, i))) {
			return -1;
		}
	}

	if (!appendf(&w, &left, "  TOTAL cost lin %+ld vram %+ld heap %+ld\n",
	             memProbeLinearTotal(p), memProbeVramTotal(p), memProbeHeapTotal(p))) {
		return -1;
	}

	return (int)(w - out);
}
