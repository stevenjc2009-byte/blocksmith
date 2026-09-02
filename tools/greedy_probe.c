// Step 7.4's measurement: how much would greedy meshing actually save?
//
// The plan phrases 7.4 as "re-measure, THEN decide", and the deciding number was supposed
// to be GPU milliseconds. That number cannot be obtained on this machine — Azahar's
// C3D_GetDrawingTime returns a constant 0.249 ms — so the decision has to rest on the only
// evidence that CAN be gathered here: the size of the prize. This probe generates the real
// spawn world, meshes it with the real mesher, and counts how many quads a greedy merge
// would emit instead.
//
// It counts the merge two ways, because they are two different products:
//
//   LOSSLESS  merge only quads whose four AO corners are all equal to each other and to
//             the neighbour's. This is the only merge that leaves the picture unchanged.
//             A 2x1 quad has four corners, so any AO variation across the merged span is
//             gone — which is precisely the "greedy meshing breaks unit-quad AO" trade the
//             plan warns about.
//
//   AO-DROPPED  merge on tile alone. This is the upper bound: what greedy meshing would
//             give if AO were abandoned entirely. It is not a shippable option, it is the
//             ceiling the lossless number should be compared against.
//
// Build and run with tools/run_greedy_probe.sh. Not part of the test suite: it asserts
// nothing, it measures.
#include <stdio.h>
#include <string.h>

#include "world/block.h"
#include "world/budget.h"
#include "world/mesher.h"
#include "world/scratch.h"
#include "world/world.h"
#include "world/worldgen.h"
#include "world/worldgen_scratch.h"

#define PROBE_SEED    1337u
#define PROBE_RADIUS  3           // 7x7 columns, the ring main.c loads around the player

// A reconstructed quad. The mesher emits four consecutive vertices per quad, all sharing a
// face index and a plane coordinate, so the quad is recoverable from the vertex stream
// without the mesher having to hand out anything extra.
typedef struct {
	uint8_t tile_u, tile_v;   // atlas cell, taken as the min corner
	uint8_t ao;               // the shared AO value, valid only when ao_uniform
	bool    ao_uniform;       // all four corners equal
} Quad;

#define SLICE_MAX  (CHUNK_DIM + 1)   // a face plane can sit at 0..16

static Quad   s_quads[MESH_MAX_FACES];
static int    s_grid[BLOCK_FACES][SLICE_MAX][CHUNK_DIM][CHUNK_DIM];
static MeshVertex   s_verts[MESH_MAX_VERTS];
static uint16_t     s_indices[MESH_MAX_INDICES];
static MeshScratch  s_scratch;

// Which axis a face's plane lies on, and which two axes the quad spans.
static void faceAxes(int face, int* plane, int* a, int* b)
{
	switch (face) {
	case FACE_EAST: case FACE_WEST:   *plane = 0; *a = 1; *b = 2; break;  // x const, spans y,z
	case FACE_TOP:  case FACE_BOTTOM: *plane = 1; *a = 0; *b = 2; break;  // y const, spans x,z
	default:                          *plane = 2; *a = 0; *b = 1; break;  // z const, spans x,y
	}
}

static int vertAxis(const MeshVertex* v, int axis)
{
	return axis == 0 ? v->x : (axis == 1 ? v->y : v->z);
}

// Standard two-pass greedy: grow a run along `a`, then grow that run along `b` while whole
// rows match. `same` decides whether two quads may be merged.
typedef bool (*SameFn)(const Quad*, const Quad*);

static bool sameLossless(const Quad* p, const Quad* q)
{
	return p->tile_u == q->tile_u && p->tile_v == q->tile_v &&
	       p->ao_uniform && q->ao_uniform && p->ao == q->ao;
}

static bool sameTileOnly(const Quad* p, const Quad* q)
{
	return p->tile_u == q->tile_u && p->tile_v == q->tile_v;
}

// A merge rule that never merges. Counting with it must reproduce the mesher's own quad
// total exactly — if it does not, the grid this probe rebuilds from the vertex stream has
// lost or doubled quads and every other number here is worthless. A measurement whose
// method cannot be shown wrong is not a measurement.
static bool sameNever(const Quad* p, const Quad* q)
{
	(void)p;
	(void)q;
	return false;
}

static int greedyCount(SameFn same)
{
	static bool used[BLOCK_FACES][SLICE_MAX][CHUNK_DIM][CHUNK_DIM];
	memset(used, 0, sizeof(used));

	int merged = 0;
	for (int f = 0; f < BLOCK_FACES; f++) {
		for (int p = 0; p < SLICE_MAX; p++) {
			for (int a = 0; a < CHUNK_DIM; a++) {
				for (int b = 0; b < CHUNK_DIM; b++) {
					const int q0 = s_grid[f][p][a][b];
					if (q0 < 0 || used[f][p][a][b]) continue;

					const Quad* base = &s_quads[q0];

					int wa = 1;
					while (a + wa < CHUNK_DIM) {
						const int qn = s_grid[f][p][a + wa][b];
						if (qn < 0 || used[f][p][a + wa][b] || !same(base, &s_quads[qn])) break;
						wa++;
					}

					int hb = 1;
					while (b + hb < CHUNK_DIM) {
						bool row_ok = true;
						for (int i = 0; i < wa; i++) {
							const int qn = s_grid[f][p][a + i][b + hb];
							if (qn < 0 || used[f][p][a + i][b + hb] || !same(base, &s_quads[qn])) {
								row_ok = false;
								break;
							}
						}
						if (!row_ok) break;
						hb++;
					}

					for (int i = 0; i < wa; i++)
						for (int j = 0; j < hb; j++)
							used[f][p][a + i][b + j] = true;
					merged++;
				}
			}
		}
	}
	return merged;
}

int main(void)
{
	static World world;
	static WorldGenScratch wgs;   // this probe's one lane; too big for main's frame
	WorldGen gen;

	worldInit(&world);
	// GEN_VERSION_LEGACY on purpose, and it must stay that way to keep this probe's recorded
	// numbers meaningful: they were taken on the 2D-heightmap terrain, and v1.6.0's greedy
	// meshing decision rests on them. Re-running this against GEN_VERSION_DENSITY would
	// measure a different world and produce a figure that looks comparable and is not - the
	// density field's caves change both the quad count and the AO variation this counts.
	// If the prize needs re-measuring on Beta terrain, that is a NEW probe, not an edit here.
	worldgenInit(&gen, PROBE_SEED, GEN_VERSION_LEGACY);
	const int failed = worldgenArea(&gen, &wgs, &world, 0, 0, PROBE_RADIUS);

	// worldgenColumn already ends by calling worldgenDecorate, so the trees are in. Counting
	// the leaves says so out loud, because trees carry the most AO variation in the world and
	// measuring a treeless one would flatter the lossless merge badly. A zero here would mean
	// the whole measurement is of the wrong world.
	long leaves = 0;
	for (int32_t x = -PROBE_RADIUS * CHUNK_DIM; x < (PROBE_RADIUS + 1) * CHUNK_DIM; x++)
		for (int32_t z = -PROBE_RADIUS * CHUNK_DIM; z < (PROBE_RADIUS + 1) * CHUNK_DIM; z++)
			for (int y = 0; y < COLUMN_CHUNKS * CHUNK_DIM; y++)
				if (worldGet(&world, x, y, z) == BLOCK_LEAVES)
					leaves++;

	long total_quads = 0, total_lossless = 0, total_dropped = 0, total_identity = 0;
	long uniform_ao = 0;
	int  chunks = 0;

	for (int cx = -PROBE_RADIUS; cx <= PROBE_RADIUS; cx++) {
	for (int cz = -PROBE_RADIUS; cz <= PROBE_RADIUS; cz++) {
	for (int cy = 0; cy < COLUMN_CHUNKS; cy++) {
		if (!worldChunk(&world, cx, cy, cz)) continue;

		scratchFill(&s_scratch, &world, cx, cy, cz);

		MeshOut out = {
			.verts = s_verts, .indices = s_indices,
			.vert_cap = MESH_MAX_VERTS, .index_cap = MESH_MAX_INDICES,
		};
		meshChunk(&out, &s_scratch);
		if (out.faces == 0) continue;

		chunks++;
		total_quads += out.faces;

		memset(s_grid, -1, sizeof(s_grid));

		for (uint32_t q = 0; q < out.faces; q++) {
			const MeshVertex* v = &s_verts[q * 4];
			const int face = v[0].nrm;

			int plane_ax, ax_a, ax_b;
			faceAxes(face, &plane_ax, &ax_a, &ax_b);

			// The plane coordinate is shared by all four corners; the span coordinates
			// are taken as the minimum, which is the cell the quad sits on.
			const int plane = vertAxis(&v[0], plane_ax);
			int ca = vertAxis(&v[0], ax_a), cb = vertAxis(&v[0], ax_b);
			uint8_t tu = v[0].u, tv = v[0].v;
			bool uniform = true;
			for (int k = 1; k < 4; k++) {
				const int ka = vertAxis(&v[k], ax_a), kb = vertAxis(&v[k], ax_b);
				if (ka < ca) ca = ka;
				if (kb < cb) cb = kb;
				if (v[k].u < tu) tu = v[k].u;
				if (v[k].v < tv) tv = v[k].v;
				if (v[k].ao != v[0].ao) uniform = false;
			}

			if (plane < 0 || plane >= SLICE_MAX ||
			    ca < 0 || ca >= CHUNK_DIM || cb < 0 || cb >= CHUNK_DIM) {
				printf("UNEXPECTED quad face %d plane %d cell %d,%d - probe assumption wrong\n",
				       face, plane, ca, cb);
				return 1;
			}

			s_quads[q].tile_u     = tu;
			s_quads[q].tile_v     = tv;
			s_quads[q].ao         = v[0].ao;
			s_quads[q].ao_uniform = uniform;
			if (uniform) uniform_ao++;

			s_grid[face][plane][ca][cb] = (int)q;
		}

		total_lossless += greedyCount(sameLossless);
		total_dropped  += greedyCount(sameTileOnly);
		total_identity += greedyCount(sameNever);
	}
	}
	}

	printf("GREEDY PROBE  seed %u  radius %d  columns refused %d  leaf blocks %ld  meshed chunks %d\n",
	       PROBE_SEED, PROBE_RADIUS, failed, leaves, chunks);
	printf("  quads now              %8ld\n", total_quads);
	printf("  quads greedy, AO kept  %8ld  (%.1f%% of now)\n",
	       total_lossless, total_quads ? 100.0 * (double)total_lossless / (double)total_quads : 0.0);
	printf("  quads greedy, AO gone  %8ld  (%.1f%% of now)  <- ceiling, not shippable\n",
	       total_dropped, total_quads ? 100.0 * (double)total_dropped / (double)total_quads : 0.0);
	printf("  quads with flat AO     %8ld  (%.1f%%)  <- what limits the lossless merge\n",
	       uniform_ao, total_quads ? 100.0 * (double)uniform_ao / (double)total_quads : 0.0);
	printf("  self-check, never merge %8ld  %s  <- must equal \"quads now\"\n",
	       total_identity, total_identity == total_quads ? "OK" : "BROKEN");

	worldExit(&world);
	return 0;
}
