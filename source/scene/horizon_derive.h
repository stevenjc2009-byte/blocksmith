// The horizon cull's blocker column, and the per-column values derived from the camera.
//
// v1.8.7. This exists as its own header for one reason: it is the SINGLE definition of the
// derivation, shared by the code that ships and the test that checks it. Before this, the
// per-blocker quantities below were recomputed inside horizonHidden()'s inner loop, once per
// (candidate x blocker) pair, including a sqrtf that depends only on the blocker and the
// camera - neither of which changes while that loop runs. They are computed once per blocker
// in horizonBuild() now, which runs immediately after cameraFromView() in cullFrame() (see the
// ordering note on hznDerive below).
//
// source/scene/chunk_render.c cannot be compiled on the host - it includes <3ds.h> and
// <citro3d.h> - so tools/run_host_tests.sh lifts horizonHidden()'s source text out of it with
// awk and tests/horizon_test.c #includes the result. That machinery covers the function; it
// cannot cover a second function, because the awk range runs from one signature to one closing
// brace. Putting the derivation in a header instead means the test compiles the actual
// definition rather than a lifted copy OR a hand-copy - a strictly stronger guarantee than
// extraction, and it needs no change to run_host_tests.sh, whose horizon stanza already passes
// -I source. tests/horizon_test.c already reaches into source/ this way for CHUNK_DIM.
//
// The consequence to keep in mind: break the derivation here and tests/horizon_test.c goes red,
// because there is no second copy of it anywhere. That is the property this file is for.
#ifndef BS_SCENE_HORIZON_DERIVE_H
#define BS_SCENE_HORIZON_DERIVE_H

#include <math.h>

#ifndef CHUNK_DIM
#error "include world/chunk.h before scene/horizon_derive.h - the derivation is in chunk units"
#endif

typedef struct {
	int   cx, cz;
	float top_y;   // world-space y of the highest meshed chunk's ceiling in this column

	// Derived by hznDerive() from top_y and the camera. Not inputs - horizonBuild() owns them
	// and horizonHidden() only reads them. Filling cx/cz/top_y without calling hznDerive()
	// leaves these stale, which is why every write to top_y in either file is followed by the
	// derive rather than open-coding it.
	float ndx, ndz;   // horizontal vector camera -> column centre
	float ndist;      // its length; 0.0f means the column centre is exactly under the camera
	float blk_h;      // the column's silhouette height above the eye
} HznCol;

// Fills the derived half of one column. Depends on the camera, so it must run AFTER the camera
// position for this frame is known: in the renderer that is cameraFromView() at the top of
// cullFrame(), and horizonBuild() is called later in the same function with nothing in between
// that writes s_cam_x/y/z - verified by reading cullFrame() rather than assumed.
//
// The arithmetic is character for character what horizonHidden()'s inner loop used to do, so
// this is a hoist and not a rewrite: same operations, same order, same float precision, and
// therefore the same values. The cull's answers do not move.
static inline void hznDerive(HznCol* col, float cam_x, float cam_y, float cam_z)
{
	const float ncx = (float)(col->cx * CHUNK_DIM) + (float)CHUNK_DIM * 0.5f;
	const float ncz = (float)(col->cz * CHUNK_DIM) + (float)CHUNK_DIM * 0.5f;
	col->ndx   = ncx - cam_x;
	col->ndz   = ncz - cam_z;
	col->ndist = sqrtf(col->ndx * col->ndx + col->ndz * col->ndz);
	col->blk_h = col->top_y - cam_y;
}

#endif
