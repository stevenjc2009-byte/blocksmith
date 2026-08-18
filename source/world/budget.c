#include "world/budget.h"

#include <stdio.h>

static size_t s_used;
static size_t s_peak;
static int    s_refusals;

bool budgetClaim(size_t bytes)
{
	if (bytes > WORLD_BUDGET_BYTES - s_used) {
		s_refusals++;
		printf("BUDGET REFUSED %lu B (used %lu of %lu)\n",
		       (unsigned long)bytes, (unsigned long)s_used,
		       (unsigned long)WORLD_BUDGET_BYTES);
		return false;
	}
	s_used += bytes;
	if (s_used > s_peak) s_peak = s_used;
	return true;
}

void budgetRelease(size_t bytes)
{
	// Clamped rather than wrapped: an unsigned underflow here would read as a
	// 4 GB world and hide the real bug.
	s_used = (bytes > s_used) ? 0 : s_used - bytes;
}

size_t budgetUsed(void)   { return s_used; }
size_t budgetPeak(void)   { return s_peak; }
size_t budgetCap(void)    { return WORLD_BUDGET_BYTES; }
int    budgetRefusals(void) { return s_refusals; }

void budgetReset(void)
{
	s_used = 0;
	s_peak = 0;
	s_refusals = 0;
}
