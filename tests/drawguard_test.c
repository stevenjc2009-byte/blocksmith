// Host self-test for the chunk draw guard — source/scene/chunk_render.c's boundVertCap(),
// tierTablesGolden() and drawSane().
//
// v1.8.17 shipped this guard defaulting on for the first time, and the 2026-09-03 freeze
// evidence brief (scratchpad FREEZE-EVIDENCE-1817.md) shows steve's console froze again at the
// exact same point regardless — with no BAD DRAW line and no clean line in hang.txt at all, so
// whether the guard is even reaching the report is still unresolved on his console. Five
// hypothesis lanes converged on the drawn vertex/index values and the draw parameters as the
// remaining candidate, and this guard is the one piece of code that inspects exactly that. A
// separate audit of drawSane() found four gaps it structurally could not catch: a run starting
// mid-triangle (only the index COUNT was ever checked for a multiple of 3, never the start
// offset), a run whose first/count belong to a slot other than the one actually bound on the
// GPU, and the two ground-truth tables boundVertCap() derives its answer from being trusted
// with no check that they still match their boot-time values. v1.8.18 closes all three
// (drawSane's codes 5, 6 and 7) — this file is the proof they can actually fail, not just
// reasoning about the diff.
//
// chunk_render.c includes <3ds.h> and <citro3d.h> and cannot be compiled on the host at all —
// see tools/run_host_tests.sh's 49h comment block for the reason this project lifts real
// source text out of that file with awk instead of hand-copying it into a test ("a test that
// links nothing tests nothing", already paid for twice in this codebase). Everything under
// test below — boundVertCap, tierTablesGolden, drawSane, and the MeshSlot layout and tier-face
// cut points they depend on — is extracted the same way by
// scratchpad/guardh_drawguard.sh (GUARD-HARDEN lane, 2026-09-04) into build-host/.../*.inc,
// which this file #includes. Sabotage chunk_render.c and this binary goes red; there is no
// second copy of the checked logic anywhere to drift out of step with the shipped one.
//
// NOT extracted: MeshVertex (world/mesh_vertex.h), MESH_SLOT_FACES/MESH_SLOT_INDICES
// (scene/mesh_pool_sizing.h) and BLOCK_FACES (world/block.h, MeshSlot.face_start's size) are
// included for real below — all three are already host-clean headers built for exactly this,
// so there is no reason to lift them out of a .c file as well.
#ifndef __3DS__

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "world/mesh_vertex.h"      // the real MeshVertex — see header note above
#include "scene/mesh_pool_sizing.h" // the real MESH_SLOT_FACES / MESH_SLOT_INDICES
#include "world/block.h"            // the real BLOCK_FACES — MeshSlot.face_start needs it

static int  s_checks;
static int  s_fails;
static char s_first[200];

// Same reporting convention as tests/horizon_test.c's CHECK: every failure printed, not just
// the first, because a red arm that prints one line out of several cannot tell "this check
// failed" apart from "this check was quietly neutralised".
#define CHECK(cond) do {                                                          \
		s_checks++;                                                                \
		if (!(cond)) {                                                             \
			s_fails++;                                                             \
			printf("  FAIL L%d  %s\n", __LINE__, #cond);                           \
			if (!s_first[0])                                                       \
				snprintf(s_first, sizeof(s_first), "L%d %.170s", __LINE__, #cond); \
		}                                                                          \
	} while (0)

// ── State the extracted functions read, by the same names chunk_render.c gives them ──────
//
// Sized far smaller than the console pool (which runs to hundreds of slots per tier) — the
// functions under test only ever look at three things: whether a pointer falls inside
// [arena, arena + cap*count), whether it lands on a cap-aligned boundary, and whether the two
// ground-truth tables still match their golden copy. None of that depends on how many slots a
// tier actually holds, so two per tier is enough to prove "the right one" from "the wrong one"
// without allocating what the real console would.
#include "tierfaces_extract.inc"     // TIER_S/M/L_FACES, kTierFaces[3]

#define TEST_SLOTS_PER_TIER 2
#define TEST_TOTAL_SLOTS    (TEST_SLOTS_PER_TIER * 3)

#include "meshslot_extract.inc"      // the real MeshSlot layout

static const MeshSlot* s_bound;

static MeshVertex* s_tier_arena[3];
static int         s_tier_count[3];
static MeshVertex* s_tier_arena_golden[3];
static int         s_tier_count_golden[3];
static bool        s_tier_golden_set;

static uint16_t s_shared_indices[MESH_SLOT_INDICES];
static MeshSlot  s_slots[TEST_TOTAL_SLOTS];

static int      s_guard_code;
static uint32_t s_guard_first, s_guard_count, s_guard_maxidx, s_guard_cap;
static int      s_guard_slot, s_guard_hits;
static uintptr_t s_guard_verts;

// Backing storage for the three tier arenas. Sized off the real kTierFaces cut points
// (TIER_S/M/L_FACES, extracted above) so a slot's cap in this file is byte-for-byte what it is
// on the console — TEST_SLOTS_PER_TIER is the only thing this test shrinks.
static MeshVertex s_arena_s[TEST_SLOTS_PER_TIER * TIER_S_FACES * 4];
static MeshVertex s_arena_m[TEST_SLOTS_PER_TIER * TIER_M_FACES * 4];
static MeshVertex s_arena_l[TEST_SLOTS_PER_TIER * TIER_L_FACES * 4];

#include "boundvertcap_extract.inc"
#include "tiergolden_extract.inc"
#include "drawsane_extract.inc"

static void resetGuard(void)
{
	s_guard_code = s_guard_hits = s_guard_slot = 0;
	s_guard_first = s_guard_count = s_guard_maxidx = s_guard_cap = 0;
	s_guard_verts = 0;
}

// Builds a pool exactly like chunkRenderInit's tier loop, at TEST_SLOTS_PER_TIER slots per
// tier, and fills the shared index buffer with the SAME documented pattern chunkRenderInit
// uses (chunk_render.c: "Every quad's pattern is {v, v+1, v+2, v, v+2, v+3} relative to its
// own base vertex v = 4 * (quad's position in the slot)") — real setup data, not logic under
// test, the same way horizon_test.c fills its own HznCol fixtures directly.
static void poolSetup(void)
{
	s_tier_arena[0] = s_arena_s; s_tier_count[0] = TEST_SLOTS_PER_TIER;
	s_tier_arena[1] = s_arena_m; s_tier_count[1] = TEST_SLOTS_PER_TIER;
	s_tier_arena[2] = s_arena_l; s_tier_count[2] = TEST_SLOTS_PER_TIER;

	int next = 0;
	for (int t = 0; t < 3; t++) {
		const uint32_t cap = (uint32_t)kTierFaces[t] * 4;
		for (int i = 0; i < TEST_SLOTS_PER_TIER; i++) {
			s_slots[next].verts    = s_tier_arena[t] + (size_t)i * cap;
			s_slots[next].vert_cap = cap;
			next++;
		}
	}

	const int quads = MESH_SLOT_INDICES / 6;
	for (int q = 0; q < quads; q++) {
		const uint16_t v = (uint16_t)(4 * q);
		uint16_t* idx = &s_shared_indices[q * 6];
		idx[0] = v; idx[1] = v + 1; idx[2] = v + 2;
		idx[3] = v; idx[4] = v + 2; idx[5] = v + 3;
	}

	memcpy(s_tier_arena_golden, s_tier_arena, sizeof(s_tier_arena_golden));
	memcpy(s_tier_count_golden, s_tier_count, sizeof(s_tier_count_golden));
	s_tier_golden_set = true;
}

int main(void)
{
	poolSetup();

	// ── GREEN: a clean draw on the S tier must pass with no report ─────────────────────
	{
		resetGuard();
		s_bound = &s_slots[0];   // S tier, slot 0
		const bool ok = drawSane(s_bound, 0, 6);
		CHECK(ok);
		CHECK(s_guard_code == 0);
		CHECK(s_guard_hits == 0);
	}

	// ── GREEN: the full run on the WIDEST (L) tier slot must also pass — this is the run
	// that would overrun anything smaller, so it is the real headroom case, not a token one.
	{
		resetGuard();
		s_bound = &s_slots[4];   // L tier, slot 0 (index 4 = 2 S + 2 M)
		const bool ok = drawSane(s_bound, 0, MESH_SLOT_INDICES);
		CHECK(ok);
		CHECK(s_guard_code == 0);
	}

	// ── CODE 1: verts NULL / not a slot start ───────────────────────────────────────────
	{
		resetGuard();
		MeshSlot bogus = {0};
		bogus.verts = NULL;
		s_bound = &bogus;
		const bool ok = drawSane(s_bound, 0, 6);
		CHECK(!ok);
		CHECK(s_guard_code == 1);
	}

	// ── CODE 2: index count not a multiple of 3 (pre-existing check, still alive) ───────
	{
		resetGuard();
		s_bound = &s_slots[0];
		const bool ok = drawSane(s_bound, 0, 4);
		CHECK(!ok);
		CHECK(s_guard_code == 2);
	}

	// ── CODE 3: run ends past the end of the shared index buffer (matches the real
	// BS_DRAW_GUARD_REDTEST==3 arm exactly: first = MESH_SLOT_INDICES-6, count = 12).
	// Both were (MESH_SLOT_INDICES-3, 6) until code 5 tightened from `first % 3` to
	// `first % 6`. That first is not 6-aligned, so the guard refused it as code 5 and never
	// reached the code 3 test — the arm and this test both silently stopped proving the
	// thing they are named after. They moved together; keep them that way. ───────────────
	{
		resetGuard();
		s_bound = &s_slots[0];
		const bool ok = drawSane(s_bound, (uint32_t)MESH_SLOT_INDICES - 6u, 12u);
		CHECK(!ok);
		CHECK(s_guard_code == 3);
	}

	// ── CODE 4: indices reach past the bound (S-tier, smallest) slot's own vertex cap —
	// matches BS_DRAW_GUARD_REDTEST==4 (first=0, count=MESH_SLOT_INDICES): the shared index
	// buffer's last quad addresses vertices up to the L tier's cap, which is bigger than the
	// S tier slot actually bound here. This is the gap the guard's own comment calls "the one
	// worth the trouble" — proving it still fires after this session's refactor matters more
	// than any of the three new codes.
	{
		resetGuard();
		s_bound = &s_slots[0];   // S tier: cap = TIER_S_FACES*4 = 2048
		const bool ok = drawSane(s_bound, 0u, (uint32_t)MESH_SLOT_INDICES);
		CHECK(!ok);
		CHECK(s_guard_code == 4);
		CHECK(s_guard_cap == (uint32_t)TIER_S_FACES * 4u);
	}

	// ── CODE 5 (NEW, gap 1): run starts mid-quad. count alone was always checked for a
	// multiple of 3; first never was, so a run beginning at first=1 passed every check that
	// existed before this session. The bound is 6, not 3, because every mesher emit path
	// (emitFace, emitCross in world/mesher.c) writes exactly 4 verts and 6 indices under a
	// single overflow gate, so every legitimate first is a multiple of 6 — measured across
	// 51,740 real draw runs. A 3-aligned first starts mid-QUAD, which silently breaks code
	// 4's assumption that the run's last index is also its largest. ─────────────────────
	{
		resetGuard();
		s_bound = &s_slots[0];
		const bool ok = drawSane(s_bound, 1u, 6u);
		CHECK(!ok);
		CHECK(s_guard_code == 5);
	}
	// The 3-aligned-but-not-6-aligned case, which is the whole reason the bound tightened.
	// Under the old `first % 3` rule this passed; it must now refuse, or the tightening did
	// not actually happen.
	{
		resetGuard();
		s_bound = &s_slots[0];
		const bool ok = drawSane(s_bound, 3u, 6u);
		CHECK(!ok);
		CHECK(s_guard_code == 5);
	}
	// And the green control right next to it: the same count, properly quad-aligned, must
	// still pass — otherwise code 5 would just be code 2 wearing a new number.
	{
		resetGuard();
		s_bound = &s_slots[0];
		const bool ok = drawSane(s_bound, 6u, 6u);
		CHECK(ok);
		CHECK(s_guard_code == 0);
	}

	// ── CODE 6 (NEW, gap 6): first/count belong to a different slot than the one actually
	// bound on the GPU. slotBind bound slot 0; this drives drawSane as if the caller meant
	// slot 1's geometry instead — individually valid parameters, wrong pairing. ───────────
	{
		resetGuard();
		s_bound = &s_slots[0];               // what slotBind actually bound
		const bool ok = drawSane(&s_slots[1], 0u, 6u);  // what the caller claims it meant
		CHECK(!ok);
		CHECK(s_guard_code == 6);
		// The report must describe what the GPU actually has, not the caller's mistaken
		// belief — s_guard_slot is derived from s_bound, never from the mismatched argument.
		CHECK(s_guard_slot == 0);
	}

	// ── CODE 7 (NEW, gap 4): a tier ground-truth table drifts from its boot-time value.
	// boundVertCap() has no way to see this on its own — it would just compute a different,
	// self-consistent cap. tierTablesGolden() is what catches the drift itself. ────────────
	{
		resetGuard();
		s_bound = &s_slots[0];
		s_tier_count[0] += 1;    // corrupt the live table, as if something else in the
		                          // process had scribbled over it
		const bool ok = drawSane(s_bound, 0u, 6u);
		CHECK(!ok);
		CHECK(s_guard_code == 7);
		s_tier_count[0] -= 1;    // put it back — this file's later checks must see a clean
		                          // pool again, the same discipline the real REDTEST==7 arm
		                          // in drawIndices() follows
	}
	// Green control: with the table restored, the exact same draw must pass again — proves
	// code 7 really is watching the table and not permanently latching on the first call.
	{
		resetGuard();
		s_bound = &s_slots[0];
		const bool ok = drawSane(s_bound, 0u, 6u);
		CHECK(ok);
		CHECK(s_guard_code == 0);
	}

	printf("drawguard_test: %d checks, %d fail%s\n", s_checks, s_fails, s_fails == 1 ? "" : "s");
	if (s_fails) {
		printf("first failure: %s\n", s_first);
		return 1;
	}
	return 0;
}

#else
int main(void) { return 0; }
#endif
