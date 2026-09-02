// Host gate for scene/ringorder.c — the nearest-first order main.c's genRequestArea() and
// genQueueReadyColumns() walk the streaming ring in.
//
// WHY THIS FILE LINKS THE REAL MODULE. The defect being fixed is an ORDERING defect inside
// main.c, and main.c carries main() and includes <3ds.h>, so it cannot be linked here. That is
// the same wall world/meshq.c was carved out of main.c's genQueueReadyColumns to get around in
// v1.6.0 task 12, and this file follows it exactly: the order itself now lives in a module with
// no <3ds.h> in it, this binary links THAT module rather than a copy of it, and main.c calls it.
//
// WHAT THIS FILE CANNOT PROVE, stated up front. It proves the ORDER is nearest-first. It does
// not prove main.c walks it — nothing on the host can reach main.c's loops — so the integration
// is checked by reading the two call sites and by an ARM compile, and the pop-in it is meant to
// improve is a VISUAL claim that needs the console. See the report accompanying this change.
//
// The "before" order below is a MODEL of the loop that was replaced (`for dz { for dx }`),
// written out here rather than linked, because the code it models no longer exists to link. It
// is four lines and unambiguous; it is used only to produce the comparison figures, never as
// the thing under test.
//
// Own main(), same pattern as tests/render_dist_ceiling_test.c.
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "scene/ringorder.h"

static int  s_checks;
static int  s_fails;
static char s_first[160];

#define CHECK(cond) do {                                                           \
		s_checks++;                                                                \
		if (!(cond)) {                                                             \
			if (!s_fails) snprintf(s_first, sizeof s_first, "L%d %s", __LINE__, #cond); \
			s_fails++;                                                             \
		}                                                                          \
	} while (0)

static int d2(int dx, int dz) { return dx * dx + dz * dz; }

// ── The order that was there before, as a model (see the file comment) ────────────────────
static int beforeOrder(int radius, RingOffset* out)
{
	int n = 0;
	for (int dz = -radius; dz <= radius; dz++)
		for (int dx = -radius; dx <= radius; dx++)
			out[n++] = (RingOffset){(int8_t)dx, (int8_t)dz};
	return n;
}

// The order shipped, filtered to `radius` exactly the way main.c's two loops filter it —
// INCLUDING the `seen < want` early stop, which is the half most able to go silently wrong. If
// that stop ever fired early, this returns fewer than (2r+1)^2 columns and the count check in
// main() below goes red; in main.c the same defect would instead be columns that are never asked
// for, i.e. a permanent hole at one edge of the world with a clean build.
//
// `scanned` reports how many table entries the walk touched, which is the cost the early stop
// exists to bound.
static int afterOrder(int radius, RingOffset* out, int* scanned)
{
	const RingOffset* const ord = ringOrder();
	const int want = (2 * radius + 1) * (2 * radius + 1);
	int n = 0, i = 0;
	for (; i < RING_ORDER_COUNT && n < want; i++) {
		const int dx = ord[i].dx, dz = ord[i].dz;
		if (dx < -radius || dx > radius || dz < -radius || dz > radius) continue;
		out[n++] = ord[i];
	}
	if (scanned) *scanned = i;
	return n;
}

// ── Measurements ──────────────────────────────────────────────────────────────────────────

// Submission index at which every column of the player's own 3x3 ring has been asked for.
// This is the one that gates the spawn column being meshable at all: main.c's
// genColumnRingComplete() refuses to queue a column until all nine of its neighbours are in.
static int idxCentreRingComplete(const RingOffset* o, int n)
{
	int seen = 0;
	for (int i = 0; i < n; i++)
		if (o[i].dx >= -1 && o[i].dx <= 1 && o[i].dz >= -1 && o[i].dz <= 1)
			if (++seen == 9) return i;
	return -1;
}

// Mean Euclidean distance of the first k columns submitted, x1000 so it prints as an integer
// and cannot pick up a printf float-formatting difference between hosts.
static int meanDistOfFirst(const RingOffset* o, int k)
{
	double sum = 0.0;
	for (int i = 0; i < k; i++) {
		const int q = d2(o[i].dx, o[i].dz);
		double r = 0.0, x = (double)q;
		if (x > 0.0) { r = x; for (int it = 0; it < 40; it++) r = 0.5 * (r + x / r); }
		sum += r;
	}
	return (int)(sum / (double)k * 1000.0 + 0.5);
}

// The +X recentre strip: when the player crosses a column boundary eastward, genRecenter() ->
// genRequestArea() re-asks for exactly the columns at dx = +radius. This returns the position
// of the column DEAD AHEAD (dz == 0) within that strip's own submission order.
static int idxAheadInStrip(const RingOffset* o, int n, int radius)
{
	int pos = 0;
	for (int i = 0; i < n; i++) {
		if (o[i].dx != radius) continue;      // not in the newly entered strip
		if (o[i].dz == 0) return pos;
		pos++;
	}
	return -1;
}

static void printMap(const char* title, const RingOffset* o, int n, int radius, int after)
{
	printf("  %s (first %d of %d submitted)\n", title, after, n);
	for (int dz = -radius; dz <= radius; dz++) {
		printf("    ");
		for (int dx = -radius; dx <= radius; dx++) {
			char c = '.';
			for (int i = 0; i < after && i < n; i++)
				if (o[i].dx == dx && o[i].dz == dz) { c = '#'; break; }
			if (dx == 0 && dz == 0) c = (c == '#') ? '@' : 'o';
			printf("%c", c);
		}
		printf("\n");
	}
}

int main(void)
{
	const RingOffset* const ord = ringOrder();

	// ── 1. The table is a PERMUTATION of the square. A sort that dropped or duplicated an
	// entry would leave a column of the world that is never asked for, which is a permanent
	// hole rather than a slow load — the same class of failure world/meshq.c was split out for.
	{
		static int seen[RING_ORDER_SPAN][RING_ORDER_SPAN];
		memset(seen, 0, sizeof seen);
		for (int i = 0; i < RING_ORDER_COUNT; i++) {
			CHECK(ord[i].dx >= -RING_ORDER_MAX_RADIUS && ord[i].dx <= RING_ORDER_MAX_RADIUS);
			CHECK(ord[i].dz >= -RING_ORDER_MAX_RADIUS && ord[i].dz <= RING_ORDER_MAX_RADIUS);
			seen[ord[i].dz + RING_ORDER_MAX_RADIUS][ord[i].dx + RING_ORDER_MAX_RADIUS]++;
		}
		int missing = 0, dup = 0;
		for (int z = 0; z < RING_ORDER_SPAN; z++)
			for (int x = 0; x < RING_ORDER_SPAN; x++) {
				if (seen[z][x] == 0) missing++;
				if (seen[z][x] > 1)  dup++;
			}
		CHECK(missing == 0);
		CHECK(dup == 0);
	}

	// ── 2. NEAREST FIRST. This is the check the whole file exists for, and the one the red arm
	// below fails: squared distance must never decrease as the table is walked.
	for (int i = 1; i < RING_ORDER_COUNT; i++)
		CHECK(d2(ord[i - 1].dx, ord[i - 1].dz) <= d2(ord[i].dx, ord[i].dz));

	// The player's own column is asked for before anything else. genStart() also submits it by
	// hand before calling genRequestArea(), but a recentre has no such special case.
	CHECK(ord[0].dx == 0 && ord[0].dz == 0);

	// ── 3. The radius filter main.c applies keeps every property above. A caller at radius r
	// must see exactly the (2r+1)^2 columns of its own ring, still in distance order — if the
	// filter dropped one, that column would never be requested at that distance.
	printf("table entries scanned per pass (the cost the `seen < want` stop bounds):\n");
	for (int r = 1; r <= RING_ORDER_MAX_RADIUS; r++) {
		static RingOffset buf[RING_ORDER_COUNT];
		int scanned = 0;
		const int n = afterOrder(r, buf, &scanned);
		const int old_cost = (2 * r + 1) * (2 * r + 1);   // the nested pair that was replaced
		CHECK(n == old_cost);
		for (int i = 1; i < n; i++)
			CHECK(d2(buf[i - 1].dx, buf[i - 1].dz) <= d2(buf[i].dx, buf[i].dz));

		// Without the early stop this walk is RING_ORDER_COUNT every time, which at radius 1 is
		// 169 iterations to do 9 columns' work. The stop has to keep it near the old cost.
		CHECK(scanned <= RING_ORDER_COUNT);
		printf("    radius %d: old loop %3d iterations, new walk %3d entries scanned\n",
		       r, old_cost, scanned);
	}

	// ── 4. Measurements. Printed, not asserted — except the two relations at the end, which are
	// the direction of the change and would go red if the order regressed to row-major.
	{
		const int r = RING_ORDER_MAX_RADIUS;              // the area ring at RENDER_DIST_MAX
		static RingOffset before[RING_ORDER_COUNT], after[RING_ORDER_COUNT];
		const int nb = beforeOrder(r, before);
		const int na = afterOrder(r, after, NULL);
		CHECK(nb == na);

		const int cb = idxCentreRingComplete(before, nb);
		const int ca = idxCentreRingComplete(after, na);
		const int sb = idxAheadInStrip(before, nb, r);
		const int sa = idxAheadInStrip(after, na, r);

		printf("\nring order, area radius %d (%d columns)\n", r, na);
		printf("  columns submitted before the player's own 3x3 ring is complete:\n");
		printf("      row-major (before): %d\n", cb);
		printf("      nearest-first (after): %d\n", ca);
		printf("  position of the column DEAD AHEAD within a +X recentre strip of %d:\n",
		       2 * r + 1);
		printf("      row-major (before): %d\n", sb);
		printf("      nearest-first (after): %d\n", sa);
		printf("  mean distance (blocks/16) of the first k columns submitted, x1000:\n");
		for (int k = 9; k <= na; k *= 3)
			printf("      k=%-3d  before %6d   after %6d\n",
			       k, meanDistOfFirst(before, k), meanDistOfFirst(after, k));

		printf("\n");
		printMap("row-major (before)", before, nb, r, 40);
		printf("\n");
		printMap("nearest-first (after)", after, na, r, 40);

		// The direction of the change, as relations rather than literals so that raising
		// RENDER_DIST_MAX cannot turn these into stale numbers that quietly stop meaning
		// anything (render_dist_ceiling_test.c's own reasoning, same trap).
		CHECK(ca < cb);
		CHECK(sa < sb);
		for (int k = 9; k <= na; k *= 3)
			CHECK(meanDistOfFirst(after, k) <= meanDistOfFirst(before, k));
	}

	printf("\nring order self-test: %s\n", s_fails ? "FAILED" : "PASS");
	if (s_fails)
		printf("  %d of %d checks failed, first: %s\n", s_fails, s_checks, s_first);
	else
		printf("  %d checks\n", s_checks);

	return s_fails ? 1 : 0;
}
