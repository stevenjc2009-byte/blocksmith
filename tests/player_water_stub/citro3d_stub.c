// Implementation for tests/player_water_stub/citro3d.h — see that file's header comment for
// why this exists and what it does and does not promise. Plain row-major 4x4 homogeneous
// transforms, right-multiplied in place exactly the way camera.c's cameraView() calls them
// (Identity, then RotateX, then RotateY, then Translate, each mutating the same matrix).

#include "citro3d.h"

#include <math.h>
#include <string.h>

void Mtx_Identity(C3D_Mtx* out)
{
	memset(out->m, 0, sizeof(out->m));
	out->m[0][0] = out->m[1][1] = out->m[2][2] = out->m[3][3] = 1.0f;
}

static void mtxMultiplyInPlace(C3D_Mtx* mtx, const C3D_Mtx* rhs)
{
	C3D_Mtx result;
	for (int r = 0; r < 4; r++) {
		for (int c = 0; c < 4; c++) {
			float sum = 0.0f;
			for (int k = 0; k < 4; k++)
				sum += mtx->m[r][k] * rhs->m[k][c];
			result.m[r][c] = sum;
		}
	}
	*mtx = result;
}

void Mtx_RotateX(C3D_Mtx* mtx, float angle, bool bRightSide)
{
	(void)bRightSide;   // camera.c always passes true; one convention is all this stub needs
	C3D_Mtx rot;
	Mtx_Identity(&rot);
	const float s = sinf(angle), c = cosf(angle);
	rot.m[1][1] = c;  rot.m[1][2] = -s;
	rot.m[2][1] = s;  rot.m[2][2] = c;
	mtxMultiplyInPlace(mtx, &rot);
}

void Mtx_RotateY(C3D_Mtx* mtx, float angle, bool bRightSide)
{
	(void)bRightSide;
	C3D_Mtx rot;
	Mtx_Identity(&rot);
	const float s = sinf(angle), c = cosf(angle);
	rot.m[0][0] = c;   rot.m[0][2] = s;
	rot.m[2][0] = -s;  rot.m[2][2] = c;
	mtxMultiplyInPlace(mtx, &rot);
}

void Mtx_Translate(C3D_Mtx* mtx, float x, float y, float z, bool bRightSide)
{
	(void)bRightSide;
	C3D_Mtx t;
	Mtx_Identity(&t);
	t.m[0][3] = x;
	t.m[1][3] = y;
	t.m[2][3] = z;
	mtxMultiplyInPlace(mtx, &t);
}
