#include "world/visgraph.h"

#include <math.h>
#include <string.h>

#include "world/block.h"
#include "world/registry.h"

// The fill decomposes a linear chunk index with masks and shifts instead of / and %, because
// the ARM11 has no divide instruction and this runs up to 4,096 times per chunk build. That
// is only equivalent while the chunk is 16 on a side, so say so here rather than leave a
// silent wrong answer if CHUNK_DIM ever moves.
_Static_assert(CHUNK_DIM == 16, "visgraph.c decomposes chunk indices assuming CHUNK_DIM 16");

// See-through for the purposes of sight, which is not the same question the mesher asks.
// The mesher wants "does this hide the face behind it"; this wants "can I see past it".
//
// Built as a 256-entry table rather than called per cell, and that is not
// micro-optimisation for its own sake — it is the difference the step was measured at. The
// fill asks this question about a cell and about each of its six neighbours, so a 16^3 chunk
// makes up to 28,672 of them; blockInfo() lives in another translation unit and there is no
// link-time optimisation in this build, so every one of those was a real function call. With
// the table the first measurement of 749 us per chunk fell to the number in the log.
//
// REGISTRY_MAX entries, which is the whole BlockId space (256), so an out-of-range id needs no
// branch — blockInfo() answers air for an id with no row, and air is open.
//
// v1.6.0 task 10: the FILL loop used to stop at BLOCK_COUNT (8), even though the array was
// already the full 256 wide. That was correct only while 8 was every block there is. With the
// master registry a server can define dynamic rows from REG_ID_DYN_LO (0x80) up, and every one
// of them read back the air_open prefill instead of its own def — so a solid dynamic block was
// see-through to the cave cull, and the chunks behind a wall built out of one were culled away
// while the wall itself still drew. Walking the whole table asks registryView() the same
// question for every id it can actually be asked about; ids with no row still answer air,
// which is exactly what the prefill said, so nothing about the core rows changes.
// The air_open prefill that used to cover ids BLOCK_COUNT..255 is gone with the short loop: the
// walk below now writes every entry itself, and blockInfo() already answers air for an id with
// no row, so the prefill and the loop said the same thing about the same slots.
//
// v1.7.1 task 49: built ONCE and kept, instead of once per chunk. The table above was a
// local rebuilt at the top of every visChunkConnectivity() call — 256 blockInfo() calls per
// chunk meshed, measured on the host probe at 256.00 calls per call, for a table whose
// contents cannot change while a session is running. That is the same waste world/mesher.c
// removed from its own per-id tables (s_solid/s_deferred/s_occludes/s_rect, mesher.c:120-134),
// and this follows mesher.c's shape deliberately rather than inventing a second one:
//
//   * static storage, not a caller's local;
//   * built LAZILY on first use, never at load time or from an init hook. That timing is the
//     part that matters: world/registry.c's table is populated at runtime by registryInitCore()
//     and then added to by the server's DEFS batch at join, so a table built any earlier than
//     first use would be a table built before the blocks exist;
//   * an exported invalidate, visgraphInvalidateTables(), that is the exact counterpart of
//     mesherInvalidateTables().
//
// The one addition on top of mesher's shape is s_open_stamp, and it is here because visgraph
// has no owned call site to invalidate it from. mesher.c is invalidated by net/networld.c
// after every DEFS batch; nothing calls into visgraph at that moment, and a cave-culling table
// that goes stale does not produce a wrong-looking block, it produces a *hole in the world* —
// a solid dynamic block read back as see-through culls everything behind it. So the cache
// carries a key that says which registry it was built from, and rebuilds when that key moves,
// which makes a missed invalidate call cost a rebuild instead of a hole.
//
// What the key has to cover, and why it is cheap. Every entry of this table is one bit,
// `!(solid && !transparent)`, and only two things can change it:
//   * the core rows (0x00..0x7F) — they come from registry.c's compiled-in kCoreDefs and are
//     reinstalled byte-identically by every registryInitCore(), so within one build they can
//     never differ. Nothing to hash.
//   * the dynamic rows (0x80 up) — registered lowest-free-first and never removed, and both
//     paths that create them (registryRegister and registryRemoteApply, the latter refusing
//     any batch whose first id is not the next free slot) fill them contiguously from
//     REG_ID_DYN_LO. So walking up from REG_ID_DYN_LO until an undefined id is the whole of
//     the mutable half, and in single player, where there are no dynamic rows at all, that is
//     one registryIsDefined() call per chunk instead of 256 blockInfo() calls.
// registryCount() is folded in as well so that adding a row is detected even in the case where
// its openness bit happens to match, and so that leaving a session (registryInitCore, which
// drops the count back to the core rows) is detected without walking anything.
static uint8_t  s_open[REGISTRY_MAX];
static bool     s_open_ready;
static uint32_t s_open_stamp;

// FNV-1a over registryCount() and the openness of every defined dynamic row. FNV rather than
// anything cleverer because this is a change detector, not a security fingerprint, and it has
// to be cheaper than the 256 calls it replaces or the whole exercise is pointless.
static uint32_t openStamp(void)
{
	uint32_t h = 2166136261u;
	h = (h ^ registryCount()) * 16777619u;

	for (int id = REG_ID_DYN_LO; id <= REG_ID_DYN_HI; id++) {
		if (!registryIsDefined((BlockId)id)) break;   // contiguous, so the first gap is the end
		const BlockInfo* info = blockInfo((BlockId)id);
		const uint32_t   bit  = (uint32_t)!(info->solid && !info->transparent);
		h = (h ^ ((uint32_t)id | (bit << 8))) * 16777619u;
	}
	return h;
}

void visgraphInvalidateTables(void)
{
	s_open_ready = false;
}

// Returns the cached table, building it if the registry it was built from has moved.
//
// Main thread only, like mesher.c's planBuild(): the sole caller is
// scene/chunk_render.c's mesh step, and app/worker.c never reaches this file.
static const uint8_t* openTable(void)
{
	const uint32_t stamp = openStamp();
	if (s_open_ready && stamp == s_open_stamp) return s_open;

	for (int i = 0; i < REGISTRY_MAX; i++) {
		const BlockInfo* info = blockInfo((BlockId)i);
		s_open[i] = (uint8_t)!(info->solid && !info->transparent);
	}
	s_open_stamp = stamp;
	s_open_ready = true;
	return s_open;
}

// Which of the six faces this local coordinate sits on, as a bit mask. A corner cell is on
// three of them at once, which is the whole reason this returns a mask and not a face.
static inline uint8_t faceBitsAt(int x, int y, int z)
{
	uint8_t b = 0;
	if (x == CHUNK_DIM - 1) b |= 1u << FACE_EAST;
	if (x == 0)             b |= 1u << FACE_WEST;
	if (y == CHUNK_DIM - 1) b |= 1u << FACE_TOP;
	if (y == 0)             b |= 1u << FACE_BOTTOM;
	if (z == CHUNK_DIM - 1) b |= 1u << FACE_SOUTH;
	if (z == 0)             b |= 1u << FACE_NORTH;
	return b;
}

// Every pair of distinct faces present in `bits`, as a connectivity mask.
static uint16_t pairsOf(uint8_t bits)
{
	uint16_t m = 0;
	for (int a = 0; a < BLOCK_FACES; a++) {
		if (!(bits & (1u << a))) continue;
		for (int b = a + 1; b < BLOCK_FACES; b++) {
			if (!(bits & (1u << b))) continue;
			m |= (uint16_t)(1u << visPairIndex(a, b));
		}
	}
	return m;
}

uint16_t visChunkConnectivity(const Chunk* c, VisScratch* sc)
{
	const uint8_t* open = openTable();

	// Step 9.1b. A UNIFORM chunk's answer does not need a flood fill, or even sc, to
	// compute: every one of its 4,096 cells reads the same open[] answer, so either none of
	// them are see-through (no pair of faces can possibly connect — mask 0) or all of them
	// are (one component touching every face — every pair connects, all fifteen real bits
	// set). That is NOT the same value as VIS_ALL_CONNECTED (0xFFFF): that macro is the
	// per-frame walk's "unknown chunk" sentinel (see visgraph.h:45-49), which deliberately
	// also sets the unused 16th bit so the whole walk table can be initialised with one
	// memset(0xFF). A real flood-fill result — which is exactly what this shortcut must
	// reproduce, bit for bit, since callers compare it against pairsOf()'s output — never
	// sets that 16th bit, because visPairIndex() never returns 15. Using VIS_ALL_CONNECTED
	// here instead of the fifteen-bit full mask was caught by testVisConnectivity's
	// `open == 0x7FFF` assertion (tools/run_host_tests.sh: "FAIL L2900 open == 0x7FFF"),
	// which is exactly the kind of bit-for-bit regression that check exists to catch.
	// An all-air chunk is the common case this shortcuts and is already skipped before
	// meshing by chunkIsAllAir in main.c/scene/chunk_render.c, so this mainly pays off for
	// an all-stone chunk deep underground, which still reaches this function on its way
	// into the cave-culling graph.
	if (chunkGetForm(c) == CHUNK_FORM_UNIFORM)
		return open[chunkGet(c, 0)] ? (uint16_t)((1u << VIS_PAIRS) - 1) : 0;

	// Non-uniform (PALETTE4 or RAW): unpack once, here, rather than let the flood fill below
	// call chunkGet per cell. See visgraph.c:17-22 above (now restated at the header, chunk.h)
	// for why the fill itself must stay a raw array walk — the measured 749us-to-current-number
	// win was earned by removing a per-cell function call from a loop that runs up to 28,672
	// times, and a per-cell chunkGet would put one straight back.
	chunkDecompressAll(c, sc->blocks);

	memset(sc->label, 0, sizeof(sc->label));

	// Component 0 is "unvisited", so real components start at 1 and comp_faces is indexed
	// by the component number directly.
	uint16_t components = 0;
	memset(sc->comp_faces, 0, sizeof(sc->comp_faces));

	// chunkIndex is (y * 16 + z) * 16 + x, so a neighbour is a fixed offset on the linear
	// index and the local coordinates come back out with a mask and two shifts. Decomposing
	// with % and / and re-composing with chunkIndex, which is how this was first written,
	// costs three divisions per cell on an ARM11 that has no divide instruction at all.
	enum { STEP_X = 1, STEP_Z = CHUNK_DIM, STEP_Y = CHUNK_DIM * CHUNK_DIM };

	// Every cell is a candidate seed. Seeding only from the six face planes was tried — it
	// visits 1,536 candidates instead of 4,096 and never floods a pocket of air sealed inside
	// the chunk, which can contribute no pair to the mask — and measured 637 us against 630,
	// i.e. no change. The cost is the flood, not the search for somewhere to start it, so the
	// simpler loop is the one that stays. Do not re-try this without a measurement.
	for (int seed = 0; seed < CHUNK_BLOCKS; seed++) {
		if (sc->label[seed] || !open[sc->blocks[seed]]) continue;

		// An explicit stack, not recursion. A 16^3 open chunk is 4,096 cells deep in the
		// worst case and the 3DS main thread's stack is not there to be spent on a flood
		// fill.
		components++;
		uint8_t faces = 0;
		int top = 0;
		sc->stack[top++] = (uint16_t)seed;
		sc->label[seed]  = components;

		while (top > 0) {
			const int i  = sc->stack[--top];
			const int lx = i & (CHUNK_DIM - 1);
			const int lz = (i >> 4) & (CHUNK_DIM - 1);
			const int ly = i >> 8;
			faces |= faceBitsAt(lx, ly, lz);

			// Six-connected. Diagonal-only gaps are not sight lines, and treating them as
			// connections is how a cull starts keeping chunks behind a checkerboard wall.
			int nbr[6];
			int n = 0;
			if (lx < CHUNK_DIM - 1) nbr[n++] = i + STEP_X;
			if (lx > 0)             nbr[n++] = i - STEP_X;
			if (ly < CHUNK_DIM - 1) nbr[n++] = i + STEP_Y;
			if (ly > 0)             nbr[n++] = i - STEP_Y;
			if (lz < CHUNK_DIM - 1) nbr[n++] = i + STEP_Z;
			if (lz > 0)             nbr[n++] = i - STEP_Z;

			for (int d = 0; d < n; d++) {
				const int j = nbr[d];
				if (sc->label[j] || !open[sc->blocks[j]]) continue;

				sc->label[j] = components;
				sc->stack[top++] = (uint16_t)j;
			}
		}

		sc->comp_faces[components] = faces;
	}

	uint16_t mask = 0;
	for (uint16_t k = 1; k <= components; k++)
		mask |= pairsOf(sc->comp_faces[k]);
	return mask;
}

// ---------------------------------------------------------------------------------------

static inline int cellIndex(const VisWalk* w, int lx, int ly, int lz)
{
	return (ly * w->nz + lz) * w->nx + lx;
}

bool visWalkBegin(VisWalk* w, int ox, int oy, int oz, int nx, int ny, int nz)
{
	if (nx <= 0 || ny <= 0 || nz <= 0) return false;
	if (nx > VIS_BOX_MAX_XZ || nz > VIS_BOX_MAX_XZ || ny > VIS_BOX_MAX_Y) return false;

	w->ox = ox; w->oy = oy; w->oz = oz;
	w->nx = nx; w->ny = ny; w->nz = nz;

	const size_t cells = (size_t)nx * (size_t)ny * (size_t)nz;

	// 0xFF, not 0: an empty coordinate has to read as fully connected. See the header —
	// the sky above the terrain is a hole in the mesh list, and a hole that reads as a wall
	// culls the world.
	memset(w->mask, 0xFF, cells * sizeof(w->mask[0]));
	memset(w->drawable, 0, cells * sizeof(w->drawable[0]));
	memset(w->visible,  0, cells * sizeof(w->visible[0]));
	memset(w->entered,  0, cells * sizeof(w->entered[0]));
	memset(w->pending,  0, cells * sizeof(w->pending[0]));
	return true;
}

void visWalkSet(VisWalk* w, int cx, int cy, int cz, uint16_t mask, bool drawable)
{
	const int lx = cx - w->ox, ly = cy - w->oy, lz = cz - w->oz;
	if (lx < 0 || lx >= w->nx || ly < 0 || ly >= w->ny || lz < 0 || lz >= w->nz) return;

	const int i = cellIndex(w, lx, ly, lz);
	w->mask[i]     = mask;
	w->drawable[i] = drawable;
}

bool visWalkVisible(const VisWalk* w, int cx, int cy, int cz)
{
	const int lx = cx - w->ox, ly = cy - w->oy, lz = cz - w->oz;
	if (lx < 0 || lx >= w->nx || ly < 0 || ly >= w->ny || lz < 0 || lz >= w->nz) return true;
	return w->visible[cellIndex(w, lx, ly, lz)];
}

static inline int clampi(int v, int lo, int hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

// Chunk coordinate of a world block position. floorf first and *then* the arithmetic shift,
// which is world.h's convention — a cast to int truncates toward zero, so at x = -1.5 it
// would answer chunk 0 for a block that is in chunk -1.
static inline int chunkOfBlock(float block)
{
	return (int)floorf(block) >> 4;
}

// The N.V < 0 test. `face` is stepped through only when the camera is on its inner side, so
// the walk can never move back toward the camera. Distances are in world blocks.
static bool faceLeadsAway(int face, int cx, int cy, int cz, float cam_x, float cam_y, float cam_z)
{
	const float lo_x = (float)(cx * CHUNK_DIM), hi_x = lo_x + (float)CHUNK_DIM;
	const float lo_y = (float)(cy * CHUNK_DIM), hi_y = lo_y + (float)CHUNK_DIM;
	const float lo_z = (float)(cz * CHUNK_DIM), hi_z = lo_z + (float)CHUNK_DIM;

	switch (face) {
	case FACE_EAST:   return cam_x <= hi_x;
	case FACE_WEST:   return cam_x >= lo_x;
	case FACE_TOP:    return cam_y <= hi_y;
	case FACE_BOTTOM: return cam_y >= lo_y;
	case FACE_SOUTH:  return cam_z <= hi_z;
	case FACE_NORTH:  return cam_z >= lo_z;
	default:          return false;
	}
}

static inline int oppositeFace(int face)
{
	// The enum is written in +/- pairs, so the opposite is the low bit flipped. Asserted by
	// the host test rather than trusted, because block.h's face order is a contract other
	// files also depend on and this would fail silently if it changed.
	return face ^ 1;
}

// Bit 6 marks the chunk the camera is in: it was not entered through any face, so every
// face is a legal exit.
#define ENTRY_ORIGIN  0x40u

// A queue entry carries the cell's local x/y/z, four bits per axis, instead of its linear
// index. Both fit in the uint16_t the queue was already made of, so this costs no memory and
// no extra per-frame setup — but it is the difference between the pop loop doing three
// integer divisions and doing none.
//
// Why not just divide. The pop loop used to recover the coordinates from the linear index
// with `i % nx`, `(i / nx) % nz` and `i / (nx * nz)`. nx/ny/nz are the *runtime* extent of
// the loaded box — they follow the render distance and the clamping in scene/chunk_render.c,
// so they are ordinary values like 9 or 7, not powers of two, and no shift is equivalent to
// them. The ARM11 has no divide instruction at all, so each of those became a call to a
// division helper: compiling this file with the real devkitARM flags (-O3 -march=armv6k)
// emitted three of them, two `bl __aeabi_idivmod` and one `bl __aeabi_idiv`. After this
// change the same compile emits none — that count is checked by compiling to assembly, not
// inferred from the C.
//
// How many that is per frame, measured rather than assumed: over a 9x8x9 box (render
// distance 4, the full world column) the walk pops 1,222 entries a frame, of which 561 get
// past the duplicate-push early-out and reach the arithmetic. Three helper calls each is
// **1,684 division calls per frame** gone, replaced by two multiplies on all 1,222 pops —
// cellIndex() is now on the near side of that early-out, which is the one thing this costs.
// On a host x86 that has a hardware divider the trade is roughly a wash (an interleaved A/B
// of both versions in one process measured 17.2 us/frame against 15.4); on the console it is
// not close, because there the divisor side of that trade is a function call into a software
// division loop and the multiply side is one instruction.
//
// Why not a precomputed reciprocal. That is the usual answer when a divisor is fixed for the
// duration, and it would have worked — but it still costs a multiply-high plus a correction
// per component, and it would be arithmetic nobody can read. The coordinates are already in
// hand at every push site (the seed has lx/ly/lz, and a neighbour step has jx/jy/jz), so
// carrying them costs literally nothing and the loop gets them back with three masks. The
// linear index the rest of the loop needs comes back from cellIndex(), which is two
// multiplies — an instruction the ARM11 does have.
//
// Four bits per axis is enough for every box VisWalk can hold: VIS_BOX_MAX_XZ is 16 and
// VIS_BOX_MAX_Y is COLUMN_CHUNKS. Asserted rather than assumed, because widening either of
// those is a one-line edit in the header and would otherwise silently alias two cells onto
// one queue entry — which would not crash, it would quietly mark the wrong chunk visible.
#define VIS_Q_SHIFT  4
#define VIS_Q_MASK   ((1u << VIS_Q_SHIFT) - 1u)
#define VIS_Q_PACK(x, y, z) \
	((uint16_t)((unsigned)(x) | ((unsigned)(y) << VIS_Q_SHIFT) | \
	            ((unsigned)(z) << (2 * VIS_Q_SHIFT))))

_Static_assert(VIS_BOX_MAX_XZ <= (1 << VIS_Q_SHIFT) && VIS_BOX_MAX_Y <= (1 << VIS_Q_SHIFT),
               "visgraph.c packs a walk queue entry as 4 bits per axis; a box axis has "
               "outgrown that and would alias two cells onto one entry");

int visWalkRun(VisWalk* w, float cam_x, float cam_y, float cam_z)
{
	const size_t cells = (size_t)w->nx * (size_t)w->ny * (size_t)w->nz;
	memset(w->visible, 0, cells * sizeof(w->visible[0]));
	memset(w->entered, 0, cells * sizeof(w->entered[0]));
	memset(w->pending, 0, cells * sizeof(w->pending[0]));

	const int cam_cx = chunkOfBlock(cam_x);
	const int cam_cy = chunkOfBlock(cam_y);
	const int cam_cz = chunkOfBlock(cam_z);

	// Clamped rather than rejected. A camera outside the loaded box means the player is at
	// its edge, and the nearest cell is the right place for sight to enter from.
	const int lx = clampi(cam_cx - w->ox, 0, w->nx - 1);
	const int ly = clampi(cam_cy - w->oy, 0, w->ny - 1);
	const int lz = clampi(cam_cz - w->oz, 0, w->nz - 1);

	int head = 0, tail = 0;
	const int start = cellIndex(w, lx, ly, lz);
	w->entered[start] = (uint8_t)ENTRY_ORIGIN;
	w->pending[start] = (uint8_t)ENTRY_ORIGIN;
	w->queue[tail++]  = VIS_Q_PACK(lx, ly, lz);

	int reached = 0;

	while (head < tail) {
		// Coordinates out of the packed entry, index back from them. See VIS_Q_PACK above
		// for why this is not `i % nx` and friends any more.
		const unsigned q  = w->queue[head++];
		const int      ix = (int)(q & VIS_Q_MASK);
		const int      iy = (int)((q >> VIS_Q_SHIFT) & VIS_Q_MASK);
		const int      iz = (int)((q >> (2 * VIS_Q_SHIFT)) & VIS_Q_MASK);
		const int      i  = cellIndex(w, ix, iy, iz);

		const uint8_t fin = w->pending[i];
		w->pending[i] = 0;
		if (!fin) continue;   // a duplicate push whose faces were already expanded

		if (w->drawable[i] && !w->visible[i]) {
			w->visible[i] = true;
			reached++;
		}

		// Every face reachable from any of the entry faces still waiting on this cell.
		uint8_t exits = 0;
		if (fin & ENTRY_ORIGIN) {
			exits = 0x3F;
		} else {
			for (int a = 0; a < BLOCK_FACES; a++) {
				if (!(fin & (1u << a))) continue;
				for (int b = 0; b < BLOCK_FACES; b++)
					if (visConnected(w->mask[i], a, b)) exits |= (uint8_t)(1u << b);
			}
		}

		for (int f = 0; f < BLOCK_FACES; f++) {
			if (!(exits & (1u << f))) continue;

			const int cx = w->ox + ix, cy = w->oy + iy, cz = w->oz + iz;
			if (!faceLeadsAway(f, cx, cy, cz, cam_x, cam_y, cam_z)) continue;

			int jx = ix, jy = iy, jz = iz;
			switch (f) {
			case FACE_EAST:   jx++; break;
			case FACE_WEST:   jx--; break;
			case FACE_TOP:    jy++; break;
			case FACE_BOTTOM: jy--; break;
			case FACE_SOUTH:  jz++; break;
			default:          jz--; break;
			}
			if (jx < 0 || jx >= w->nx || jy < 0 || jy >= w->ny || jz < 0 || jz >= w->nz)
				continue;

			const int     j   = cellIndex(w, jx, jy, jz);
			const uint8_t opp = (uint8_t)(1u << oppositeFace(f));
			if (w->entered[j] & opp) continue;

			w->entered[j] |= opp;
			w->pending[j] |= opp;
			w->queue[tail++] = VIS_Q_PACK(jx, jy, jz);
		}
	}

	return reached;
}
