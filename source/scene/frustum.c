#include "scene/frustum.h"

// Row i of a C3D_Mtx, in the only ordering that matters here: Mtx_MultiplyFVec4 computes
// clip.x as dot(r[0], v), clip.y as dot(r[1], v), and so on, with the dot taken over the
// .x/.y/.z/.w accessors. So r[i].x is the coefficient of world x in clip component i, and
// r[i].w is the constant term. (C3D_FVec stores its floats as {w, z, y, x} in memory —
// reading the union through .c[] instead of the named fields would silently reverse every
// plane, which is exactly the kind of bug that shows up as "half the world vanishes when
// you turn left".)
static void planeFromRows(float out[4], const C3D_FVec* w_row, const C3D_FVec* row, float sign)
{
	out[0] = w_row->x + sign * row->x;
	out[1] = w_row->y + sign * row->y;
	out[2] = w_row->z + sign * row->z;
	out[3] = w_row->w + sign * row->w;
}

void frustumFromMatrix(Frustum* f, const C3D_Mtx* vp)
{
	const C3D_FVec* x = &vp->r[0];
	const C3D_FVec* y = &vp->r[1];
	const C3D_FVec* z = &vp->r[2];
	const C3D_FVec* w = &vp->r[3];

	planeFromRows(f->p[FRUSTUM_LEFT],   w, x, +1.0f);   //  w + x >= 0
	planeFromRows(f->p[FRUSTUM_RIGHT],  w, x, -1.0f);   //  w - x >= 0
	planeFromRows(f->p[FRUSTUM_BOTTOM], w, y, +1.0f);   //  w + y >= 0
	planeFromRows(f->p[FRUSTUM_TOP],    w, y, -1.0f);   //  w - y >= 0
	planeFromRows(f->p[FRUSTUM_NEAR],   w, z, +1.0f);   //  w + z >= 0

	// The far plane is the NEGATED z row, not the w row minus it, because the PICA's clip
	// range is -w <= z <= 0 rather than OpenGL's -w <= z <= w: the far plane is the
	// constant z = 0, which as an inequality is simply -z >= 0. Getting this pair backwards
	// culls the entire world except an infinitely thin sheet at the far plane — measured,
	// on the first build of this file, as `draws 0 tris 0`.
	f->p[FRUSTUM_FAR][0] = -z->x;
	f->p[FRUSTUM_FAR][1] = -z->y;
	f->p[FRUSTUM_FAR][2] = -z->z;
	f->p[FRUSTUM_FAR][3] = -z->w;
}

bool frustumTestAABB(const Frustum* f, float min_x, float min_y, float min_z,
                     float max_x, float max_y, float max_z)
{
	for (int i = 0; i < FRUSTUM_PLANES; i++) {
		const float* p = f->p[i];

		// The "positive vertex": the one corner of the box furthest along this plane's
		// normal. If even that corner is on the outside, every other corner is further
		// out still and the whole box can be rejected — one dot product per plane instead
		// of eight. Picking the corner per axis by the sign of the normal is what makes
		// that a branchy select rather than a loop over corners.
		const float px = (p[0] >= 0.0f) ? max_x : min_x;
		const float py = (p[1] >= 0.0f) ? max_y : min_y;
		const float pz = (p[2] >= 0.0f) ? max_z : min_z;

		if (p[0] * px + p[1] * py + p[2] * pz + p[3] < 0.0f)
			return false;
	}
	return true;
}
