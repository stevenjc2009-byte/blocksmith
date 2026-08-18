// The world memory budget, asserted at runtime.
//
// The research page's derived budget for an Old 3DS: one chunk is 4 KB, a column
// of 8 chunks is 32 KB, and 17x17 columns of fully-populated block data is about
// 9 MB of the ~64 MB an app gets. That is comfortable — but only while it is
// checked. A silent over-allocation on this hardware surfaces later as a random
// crash in unrelated code, so every world allocation asks first and a refusal is
// counted and reported rather than swallowed.
//
// This is a module-level singleton on purpose: there is one world, and the
// bottom-screen report needs the figures without a pointer threaded through
// everything that allocates.
#pragma once

#include <stdbool.h>
#include <stddef.h>

// 12 MB. The 17x17-column worst case is ~9.25 MB; the rest is headroom for the
// column table and for a render distance experiment, without ever approaching the
// ~64 MB app limit.
#define WORLD_BUDGET_BYTES  (12u * 1024u * 1024u)

// True if the claim fits. On refusal nothing is reserved, the refusal is counted,
// and a line is printed — a budget that fails quietly is worse than no budget.
bool   budgetClaim(size_t bytes);
void   budgetRelease(size_t bytes);

size_t budgetUsed(void);
size_t budgetPeak(void);
size_t budgetCap(void);
int    budgetRefusals(void);

// Test hook: forget everything. Only for the self-test, which needs a clean slate.
void   budgetReset(void);
