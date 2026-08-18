// View-frustum culling for chunk meshes (step 7.1).
//
// Six planes pulled straight out of the combined projection x view matrix, rather than
// built by hand from a field of view and a camera direction. That is not cleverness for
// its own sake — this console needs it. `Mtx_PerspTilt` bakes a 90-degree roll into the
// projection because the top screen's framebuffer is stored sideways, and step 7.6 will
// bake an eye offset into it as well for stereoscopic 3D. A hand-built frustum would have
// to know about both and would silently cull the wrong half of the screen the day either
// changes. Planes extracted from the matrix inherit whatever the matrix says, always.
//
// The extraction is Gribb & Hartmann: a point is inside the frustum when its clip-space
// coordinates satisfy -w <= x <= w, -w <= y <= w and **-w <= z <= 0**, and each of those
// six inequalities is a row of the matrix added to or subtracted from the w row.
//
// That z range is the trap. It is -w..0, not OpenGL's -w..w — citro3d's own comment on
// Mtx_Persp calls it "a fixed depth range of [-1,0] (required by PICA)" — so the near plane
// is w + z >= 0 as usual but the **far plane is -z >= 0**, the negated z row on its own,
// with no w in it. It is also why the depth test in chunk_render.c is GPU_GREATER rather
// than GPU_LESS. Assuming the OpenGL convention here culls the entire world: measured, on
// the first build of this file, as `draws 0 tris 0`.
#pragma once

#include <3ds.h>
#include <citro3d.h>

#include <stdbool.h>

// Plane order is fixed only so that a failing cull can be attributed to a named plane
// while debugging; nothing depends on it.
typedef enum {
	FRUSTUM_LEFT,
	FRUSTUM_RIGHT,
	FRUSTUM_BOTTOM,
	FRUSTUM_TOP,
	FRUSTUM_NEAR,
	FRUSTUM_FAR,
	FRUSTUM_PLANES
} FrustumPlane;

typedef struct {
	// Each plane as (a, b, c, d) with a*x + b*y + c*z + d >= 0 meaning "inside".
	// Deliberately NOT normalised: culling only ever compares against zero, and the
	// sign of a dot product does not care about the length of the normal. Normalising
	// would cost six square roots per frame to make no difference to any answer.
	float p[FRUSTUM_PLANES][4];
} Frustum;

// Builds the six planes from a combined projection x view matrix. Pass the same matrices
// the draw uses, multiplied in the same order, or the culling will disagree with the
// rasteriser at the edges of the screen.
void frustumFromMatrix(Frustum* f, const C3D_Mtx* view_projection);

// True if any part of the axis-aligned box could be visible. This is the standard
// conservative test: a box is rejected only when it is entirely outside a single plane,
// so a box straddling two planes' outside regions without being fully outside either one
// is kept. That false positive costs one draw call; the opposite error would erase
// geometry the player can see, so the test is deliberately biased this way.
bool frustumTestAABB(const Frustum* f, float min_x, float min_y, float min_z,
                     float max_x, float max_y, float max_z);
