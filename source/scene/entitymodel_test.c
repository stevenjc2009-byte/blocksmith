// Host self-test for the animal models — the geometry and the UV unwrap in
// source/scene/entitymodel.h.
//
// #ifndef __3DS__ around the WHOLE file, and it is load bearing rather than tidiness. The
// Makefile's SOURCES list includes source/scene and globs *.c out of it, so without this
// guard the console build compiles a second main() into the link. Files under tests/ are
// exempt because that directory is not in SOURCES; a _test.c that lives beside the module it
// tests is not, and that is the trap. It lives here anyway, beside entitymodel.h, because
// what it tests is a static inline in that header and nothing links.
#ifndef __3DS__

// The header's console half needs <citro3d.h> and entity/entity.h; its data half — the box
// table, the unwrap, the vertex builder — needs neither, which is the entire reason the split
// exists. gfx/fogramp.c and scene/render_dist.c are <3ds.h>-free for the same reason.
#define EM_DATA_ONLY 1
#include "scene/entitymodel.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_checks;
static int s_failed;

#define CHECK(cond, ...)                                                      \
	do {                                                                      \
		s_checks++;                                                           \
		if (!(cond)) {                                                        \
			s_failed++;                                                       \
			printf("L%d " #cond "  ", __LINE__);                              \
			printf(__VA_ARGS__);                                              \
			printf("\n");                                                     \
		}                                                                     \
	} while (0)

static const char* kKindName[EM_KINDS] = {
	"pig", "cow", "chicken", "sheep", "zombie", "skeleton"
};
static const char* kFaceName[6] = { "top", "bottom", "right", "front", "left", "back" };

// ── the unwrap is actually a NET ─────────────────────────────────────────────────────────
//
// This is the check that is not a restatement of the code. A cardboard box flattened by
// cutting four edges has a property the code never says out loud: two faces whose rectangles
// touch on the SHEET must touch along the same edge in 3D. If emBoxFaceQuad's axis table has
// a wrong `inv` anywhere, the rectangles still tile perfectly and the sheet still looks fine,
// but the shared edge comes apart in 3D — that face is mirrored, and on nearly-symmetric art
// nobody notices until the day something asymmetric is painted on it.
//
// So: for each adjacent pair, take the two corners each face contributes to the shared edge
// and require the 3D positions to be equal.
//
// Corner order out of emBoxFaceQuad is rect top-left, top-right, bottom-right, bottom-left.
typedef struct {
	int face_a, a0, a1;    // face and the two corner indices on its shared edge
	int face_b, b0, b1;    // the neighbour, corners listed to match a0/a1 in order
	const char* what;
} NetSeam;

static const NetSeam kSeams[] = {
	// TOP sits directly above FRONT: TOP's bottom edge (3,2) is FRONT's top edge (0,1).
	{ EM_FACE_TOP, 3, 2,  EM_FACE_FRONT, 0, 1, "top/front" },
	// BOTTOM sits directly above... nothing. It is folded DOWN from FRONT but placed in row
	// A beside TOP, so on the sheet it does not touch FRONT at all. The 3D seam is still
	// real and still checkable: BOTTOM's top edge (0,1) is FRONT's bottom edge (3,2).
	{ EM_FACE_BOTTOM, 0, 1,  EM_FACE_FRONT, 3, 2, "bottom/front" },
	// RIGHT is immediately left of FRONT in row B: RIGHT's right edge (1,2) is FRONT's left
	// edge (0,3).
	{ EM_FACE_RIGHT, 1, 2,  EM_FACE_FRONT, 0, 3, "right/front" },
	// LEFT is immediately right of FRONT: FRONT's right edge (1,2) is LEFT's left edge (0,3).
	{ EM_FACE_FRONT, 1, 2,  EM_FACE_LEFT, 0, 3, "front/left" },
	// BACK is immediately right of LEFT: LEFT's right edge (1,2) is BACK's left edge (0,3).
	{ EM_FACE_LEFT, 1, 2,  EM_FACE_BACK, 0, 3, "left/back" },
};

static int samePoint(const EmVertex* a, const EmVertex* b)
{
	return fabsf(a->x - b->x) < 1e-6f && fabsf(a->y - b->y) < 1e-6f
	    && fabsf(a->z - b->z) < 1e-6f;
}

static void testUnwrapIsANet(void)
{
	for (int k = 0; k < EM_KINDS; k++) {
		for (int bi = 0; bi < EM_BOXES_PER_KIND; bi++) {
			const EmBox* b = &kEmBoxes[k][bi];
			EmVertex q[6][4];
			for (int f = 0; f < 6; f++)
				emBoxFaceQuad(b, f, q[f]);

			for (size_t i = 0; i < sizeof(kSeams) / sizeof(kSeams[0]); i++) {
				const NetSeam* s = &kSeams[i];
				CHECK(samePoint(&q[s->face_a][s->a0], &q[s->face_b][s->b0]),
				      "%s box %d seam %s first corner: (%.4f,%.4f,%.4f) vs (%.4f,%.4f,%.4f)",
				      kKindName[k], bi, s->what,
				      (double)q[s->face_a][s->a0].x, (double)q[s->face_a][s->a0].y,
				      (double)q[s->face_a][s->a0].z,
				      (double)q[s->face_b][s->b0].x, (double)q[s->face_b][s->b0].y,
				      (double)q[s->face_b][s->b0].z);
				CHECK(samePoint(&q[s->face_a][s->a1], &q[s->face_b][s->b1]),
				      "%s box %d seam %s second corner", kKindName[k], bi, s->what);
			}
		}
	}
}

// ── the faces are the faces they claim to be ─────────────────────────────────────────────
//
// A face named TOP whose four corners are not all at the box's maximum y is not a top. This
// is what catches a fixed_axis/fixed_hi transposition, which the seam test above cannot: a
// consistently-wrong pair of faces still shares its edges.
static void testFaceOrientation(void)
{
	for (int k = 0; k < EM_KINDS; k++) {
		for (int bi = 0; bi < EM_BOXES_PER_KIND; bi++) {
			const EmBox* b = &kEmBoxes[k][bi];
			const float lo[3] = { b->px / 16.0f, b->py / 16.0f, b->pz / 16.0f };
			const float hi[3] = { (b->px + b->sx) / 16.0f, (b->py + b->sy) / 16.0f,
			                      (b->pz + b->sz) / 16.0f };
			const int axis[6] = { 1, 1, 0, 2, 0, 2 };
			const int side[6] = { 1, 0, 1, 0, 0, 1 };

			for (int f = 0; f < 6; f++) {
				EmVertex q[4];
				emBoxFaceQuad(b, f, q);
				const float want = side[f] ? hi[axis[f]] : lo[axis[f]];
				for (int c = 0; c < 4; c++) {
					const float got = axis[f] == 0 ? q[c].x : (axis[f] == 1 ? q[c].y : q[c].z);
					CHECK(fabsf(got - want) < 1e-6f,
					      "%s box %d face %s corner %d: axis %d is %.4f, want %.4f",
					      kKindName[k], bi, kFaceName[f], c, axis[f],
					      (double)got, (double)want);
				}
			}
		}
	}
}

// ── the texture is not stretched ─────────────────────────────────────────────────────────
//
// One sheet texel must be one model texel on every face. A face that mapped a 4-wide
// rectangle onto a 16-wide side would still tile, still share its edges and still be
// oriented correctly; it would simply be smeared, which on this GPU renders as *a* texture
// and never as an error. Checked as: the 3D length of each rect edge, in texels, equals the
// rect's own size in sheet pixels.
static void testUniformTexelScale(void)
{
	for (int k = 0; k < EM_KINDS; k++) {
		for (int bi = 0; bi < EM_BOXES_PER_KIND; bi++) {
			const EmBox* b = &kEmBoxes[k][bi];
			for (int f = 0; f < 6; f++) {
				EmVertex q[4];
				emBoxFaceQuad(b, f, q);

				// Top edge of the rect, corners 0 -> 1, in sheet pixels and in model texels.
				const float du = (q[1].u - q[0].u) * EM_SHEET_W;
				const float dv = (q[0].v - q[3].v) * EM_SHEET_H;
				const float d3s = fabsf(q[1].x - q[0].x) + fabsf(q[1].y - q[0].y)
				                + fabsf(q[1].z - q[0].z);
				const float d3t = fabsf(q[3].x - q[0].x) + fabsf(q[3].y - q[0].y)
				                + fabsf(q[3].z - q[0].z);

				CHECK(fabsf(du - d3s * EM_TEXELS_PER_BLOCK) < 1e-3f,
				      "%s box %d face %s: %.2f sheet px across %.2f model texels",
				      kKindName[k], bi, kFaceName[f], (double)du,
				      (double)(d3s * EM_TEXELS_PER_BLOCK));
				CHECK(fabsf(dv - d3t * EM_TEXELS_PER_BLOCK) < 1e-3f,
				      "%s box %d face %s: %.2f sheet px down %.2f model texels",
				      kKindName[k], bi, kFaceName[f], (double)dv,
				      (double)(d3t * EM_TEXELS_PER_BLOCK));
			}
		}
	}
}

// ── the V flip points the way the sheet is painted ───────────────────────────────────────
//
// tools/make_animals.py paints in PNG rows, top-left origin — the same coordinates as the
// table's toy. Texture v grows UPWARDS. So a face nearer the TOP of the sheet must come out
// with a LARGER v, and every box has a pair to compare: row A (top/bottom faces, at toy) is
// above row B (the four sides, at toy + d).
//
// If the flip were dropped, or applied twice, every one of these reverses — and the visible
// result is not an error, it is an animal wearing its belly on its back.
static void testVFlipDirection(void)
{
	for (int k = 0; k < EM_KINDS; k++) {
		for (int bi = 0; bi < EM_BOXES_PER_KIND; bi++) {
			const EmBox* b = &kEmBoxes[k][bi];
			EmVertex top[4], front[4];
			emBoxFaceQuad(b, EM_FACE_TOP, top);
			emBoxFaceQuad(b, EM_FACE_FRONT, front);
			CHECK(top[0].v > front[0].v,
			      "%s box %d: top rect v %.4f is not above front rect v %.4f",
			      kKindName[k], bi, (double)top[0].v, (double)front[0].v);
			// And the flip's own arithmetic, at the one row it can be pinned at: the top of
			// row A is sheet row toy.
			CHECK(fabsf(top[0].v - (1.0f - (float)b->toy / EM_SHEET_H)) < 1e-6f,
			      "%s box %d: top rect v %.6f, expected 1 - %d/%d",
			      kKindName[k], bi, (double)top[0].v, b->toy, EM_SHEET_H);
		}
	}
}

// ── every UV is on the sheet, and inside its own animal's quadrant ───────────────────────
//
// GPU_CLAMP_TO_EDGE means a UV past the edge smears the last texel row instead of erroring,
// and a UV that strayed into a neighbouring quadrant would paint a pig with cow. Both render
// as art, never as a fault.
static void testUVsInQuadrant(void)
{
	for (int k = 0; k < EM_KINDS; k++) {
		const float qx0 = (float)((k % EM_QUADRANT_COLS) * EM_QUADRANT_W) / EM_SHEET_W;
		const float qx1 = qx0 + (float)EM_QUADRANT_W / EM_SHEET_W;
		// Row of quadrants k/EM_QUADRANT_COLS counted from the TOP of the sheet, so in v it
		// is measured from 1 downwards.
		const float qv1 = 1.0f - (float)((k / EM_QUADRANT_COLS) * EM_QUADRANT_H) / EM_SHEET_H;
		const float qv0 = qv1 - (float)EM_QUADRANT_H / EM_SHEET_H;

		EmVertex* v = (EmVertex*)malloc(sizeof(EmVertex) * EM_VERTS_PER_KIND);
		emBuildKind(k, v);
		for (int i = 0; i < EM_VERTS_PER_KIND; i++) {
			CHECK(v[i].u >= 0.0f && v[i].u <= 1.0f && v[i].v >= 0.0f && v[i].v <= 1.0f,
			      "%s vertex %d uv (%.5f,%.5f) is off the sheet", kKindName[k], i,
			      (double)v[i].u, (double)v[i].v);
			CHECK(v[i].u >= qx0 - 1e-6f && v[i].u <= qx1 + 1e-6f
			      && v[i].v >= qv0 - 1e-6f && v[i].v <= qv1 + 1e-6f,
			      "%s vertex %d uv (%.5f,%.5f) is outside its quadrant "
			      "u[%.4f,%.4f] v[%.4f,%.4f]",
			      kKindName[k], i, (double)v[i].u, (double)v[i].v,
			      (double)qx0, (double)qx1, (double)qv0, (double)qv1);
		}
		free(v);
	}
}

// ── nets do not collide ──────────────────────────────────────────────────────────────────
//
// Two boxes may share a net deliberately — the four legs of a quadruped do, and that is what
// keeps the sheet small. Two boxes with DIFFERENT net origins overlapping is a packing bug
// that would put one animal's leg on another's face. tools/make_animals.py checks the same
// thing against the finished PNG; this checks it against the table, which is where it can be
// fixed.
static void testNetsDoNotCollide(void)
{
	static uint8_t owner_k[EM_SHEET_W][EM_SHEET_H];
	static uint8_t owner_x[EM_SHEET_W][EM_SHEET_H];
	static uint8_t owner_y[EM_SHEET_W][EM_SHEET_H];
	static uint8_t used[EM_SHEET_W][EM_SHEET_H];
	memset(used, 0, sizeof(used));

	int collisions = 0;
	for (int k = 0; k < EM_KINDS; k++) {
		for (int bi = 0; bi < EM_BOXES_PER_KIND; bi++) {
			const EmBox* b = &kEmBoxes[k][bi];
			int nw, nh;
			emBoxNetSize(b, &nw, &nh);
			CHECK(b->tox + nw <= EM_SHEET_W && b->toy + nh <= EM_SHEET_H,
			      "%s box %d net %dx%d at (%d,%d) runs off the sheet",
			      kKindName[k], bi, nw, nh, b->tox, b->toy);

			for (int y = b->toy; y < b->toy + nh && y < EM_SHEET_H; y++) {
				for (int x = b->tox; x < b->tox + nw && x < EM_SHEET_W; x++) {
					if (used[x][y]
					    && !(owner_k[x][y] == k && owner_x[x][y] == b->tox
					         && owner_y[x][y] == b->toy))
						collisions++;
					used[x][y] = 1;
					owner_k[x][y] = (uint8_t)k;
					owner_x[x][y] = b->tox;
					owner_y[x][y] = b->toy;
				}
			}
		}
	}
	CHECK(collisions == 0, "%d sheet texels are claimed by two different nets", collisions);
}

// ── the numbers the report quotes ────────────────────────────────────────────────────────
static void reportCost(void)
{
	printf("entitymodel: %d kinds x %d boxes x %d verts = %d verts in the shared VBO, "
	       "%d bytes (%d-byte EmVertex)\n",
	       EM_KINDS, EM_BOXES_PER_KIND, EM_VERTS_PER_BOX, EM_VERTS,
	       (int)(EM_VERTS * sizeof(EmVertex)), (int)sizeof(EmVertex));
	printf("entitymodel: %d verts per animal per draw call\n", EM_VERTS_PER_KIND);
	printf("entitymodel: EmVertex offsets  pos 0, uv %d, stride %d "
	       "(both attributes 4-byte aligned)\n",
	       (int)offsetof(EmVertex, u), (int)sizeof(EmVertex));
	for (int k = 0; k < EM_KINDS; k++) {
		float hh, ht;
		emKindExtent(k, &hh, &ht);
		printf("entitymodel: %-8s half-extent %.4f blocks, height %.4f blocks\n",
		       kKindName[k], (double)hh, (double)ht);
	}
}

// Dumps the whole vertex buffer, one vertex per line, for the offline renderer that turns it
// back into a picture. The point of dumping the REAL buffer rather than re-deriving it in the
// renderer is the difference between an instrument that shares the code path and one that
// agrees with it by hand — this project has been bitten by five agreeing checks that were all
// blind to the same wrong mesh.
static void dumpVerts(void)
{
	for (int k = 0; k < EM_KINDS; k++) {
		EmVertex* v = (EmVertex*)malloc(sizeof(EmVertex) * EM_VERTS_PER_KIND);
		emBuildKind(k, v);
		for (int i = 0; i < EM_VERTS_PER_KIND; i++)
			printf("V %d %d %.6f %.6f %.6f %.8f %.8f\n",
			       k, i, (double)v[i].x, (double)v[i].y, (double)v[i].z,
			       (double)v[i].u, (double)v[i].v);
		free(v);
	}
}

int main(int argc, char** argv)
{
	if (argc > 1 && strcmp(argv[1], "--dump") == 0) {
		dumpVerts();
		return 0;
	}

	testUnwrapIsANet();
	testFaceOrientation();
	testUniformTexelScale();
	testVFlipDirection();
	testUVsInQuadrant();
	testNetsDoNotCollide();
	reportCost();

	if (s_failed) {
		printf("entitymodel self-test: FAILED - %d of %d checks\n", s_failed, s_checks);
		return 1;
	}
	printf("entitymodel self-test: PASS %d checks\n", s_checks);
	return 0;
}

#else

// Console build: nothing here. An empty translation unit is not valid ISO C, so give the
// compiler one declaration to chew on rather than let -Wpedantic complain about the guard.
// Same shape and same reason as scene/title_nav_test.c and scene/ui_layout_test.c.
typedef int entitymodel_test_host_only_t;

#endif   // !__3DS__
