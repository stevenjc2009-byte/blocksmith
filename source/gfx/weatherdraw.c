#include "gfx/weatherdraw.h"

#include <math.h>

// ── Grid placement and animation, pure C ────────────────────────────────────────────────
//
// Everything down to the __3DS__ guard is plain arithmetic over WeatherDrawState and
// WeatherVertex -- see weatherdraw.h's own header for why weatherAt() itself is
// deliberately NOT called from anywhere in this file.

// Fall-scroll speed, in whole texture repeats (WEATHERDRAW_TEX_WORLD_PER_TILE world blocks
// each) per second. "Rain falls fast... snow falls slower" (the task's own requirement) is
// encoded entirely in these two numbers being different -- there is no other place in this
// file that distinguishes the two kinds' vertical motion. Both are JUDGEMENT CALLS by eye,
// not measurements -- see weatherDrawFallSpeed's own header in weatherdraw.h. 6.0 vs 1.2 is
// a 5x ratio, picked because Minecraft-style rain reads as several times snow's fall rate in
// every version of this effect steve has ever seen play, not because either number was
// clocked against a reference.
#define WEATHERDRAW_RAIN_FALL_SPEED 6.0f
#define WEATHERDRAW_SNOW_FALL_SPEED 1.2f

// Snow's horizontal drift: a sinusoid, not a random walk, because a random walk needs a PRNG
// state this struct does not otherwise carry and a sinusoid is enough to read as "drifting"
// rather than "falling straight down" from a fixed camera-relative vantage. Rain gets none
// -- "near-vertical" is the task's own wording for rain, so rain's drift_x is always 0 (see
// weatherDrawUpdate). Both constants are judgement calls, exactly like the fall speeds above.
// WEATHERDRAW_DRIFT_AMPLITUDE_BLOCKS itself lives in weatherdraw.h (public), not here -- see
// that header's own comment on why: the host test needs the exact same number this file
// uses, not a second copy of it.
#define WEATHERDRAW_DRIFT_FREQ_RAD_PER_SEC 1.2f     // ~5.2 real seconds per full sway cycle
#define WEATHERDRAW_TWO_PI 6.28318530718f

// WEATHERDRAW_GRID_Y_SNAP_BLOCKS also lives in weatherdraw.h, for the same reason. Deliberately
// much coarser than WEATHERDRAW_CELL_BLOCKS (the horizontal snap): the horizontal snap has to
// be fine enough that walking in a straight line does not visibly "pop" the grid sideways
// often, but the strip is already WEATHERDRAW_COLUMN_HEIGHT_BLOCKS (12) tall and centred on
// the camera, so the camera can move several blocks vertically (a jump, walking up a
// staircase) before the fixed 12-block window would actually stop covering it from top to
// bottom. A coarser snap here means fewer silent re-centres for the same visual coverage.
// Judgement call, not a measurement.

// Rounds `v` to the nearest multiple of `step` (step > 0). floorf(x + 0.5) rather than a
// libm round()/roundf() call -- this project's other pure-C world files avoid pulling in
// more of libm than a single tick needs, and floorf is already required for this file's
// wrap arithmetic below, so no second rounding primitive is introduced for one call site.
static float weatherDrawSnap(float v, float step)
{
	return floorf(v / step + 0.5f) * step;
}

void weatherDrawStateInit(WeatherDrawState* st)
{
	if (!st)
		return;
	st->kind = WEATHER_CLEAR;
	st->scroll_v = 0.0f;
	st->drift_phase = 0.0f;
	st->drift_x = 0.0f;
	st->grid_x = 0.0f;
	st->grid_y = 0.0f;
	st->grid_z = 0.0f;
}

void weatherDrawSetState(WeatherDrawState* st, WeatherKind kind)
{
	if (!st)
		return;
	st->kind = kind;
}

bool weatherDrawShouldDraw(const WeatherDrawState* st)
{
	return st && st->kind != WEATHER_CLEAR;
}

float weatherDrawFallSpeed(WeatherKind kind)
{
	switch (kind) {
	case WEATHER_RAIN: return WEATHERDRAW_RAIN_FALL_SPEED;
	case WEATHER_SNOW: return WEATHERDRAW_SNOW_FALL_SPEED;
	default:           return 0.0f;
	}
}

int weatherDrawTileForKind(WeatherKind kind)
{
	switch (kind) {
	case WEATHER_RAIN: return WEATHERDRAW_TILE_RAIN;
	case WEATHER_SNOW: return WEATHERDRAW_TILE_SNOW;
	default:           return -1;
	}
}

int weatherDrawKindVertexOffset(WeatherKind kind)
{
	switch (kind) {
	case WEATHER_RAIN: return 0;
	case WEATHER_SNOW: return WEATHERDRAW_VERTS_PER_KIND;
	default:           return -1;
	}
}

void weatherDrawUpdate(WeatherDrawState* st, float dt, float cam_x, float cam_y, float cam_z)
{
	if (!st)
		return;

	// A negative dt (a caller's first frame before its own clock has a prior sample, or a
	// clock glitch) must not run either accumulator backwards or toward NaN -- clamped to
	// zero rather than trusted, the same "validate at the boundary" rule every pure function
	// in this project's world/ files already follows for its own inputs.
	if (dt < 0.0f)
		dt = 0.0f;

	// Wrapped into [0, 1) every call rather than left to grow without bound. GPU_REPEAT
	// (weatherdraw.c's own wrap mode on this axis -- see weather.v.pica's header) would
	// sample the same texel for scroll_v and scroll_v + 1000000.0f, so correctness never
	// required this; it is here so a multi-hour play session cannot walk this float far
	// enough from zero to lose the sub-texel precision a slow accumulation like this
	// otherwise would.
	float speed = weatherDrawFallSpeed(st->kind);
	st->scroll_v = fmodf(st->scroll_v + speed * dt, 1.0f);

	// Same reasoning, same wrap, for the drift phase -- 2*pi rather than 1.0 because this
	// one feeds sinf() directly rather than a hardware sampler that wraps for free.
	st->drift_phase = fmodf(st->drift_phase + WEATHERDRAW_DRIFT_FREQ_RAD_PER_SEC * dt,
	                         WEATHERDRAW_TWO_PI);

	// Rain is "near-vertical" (the task's own wording) -- exactly zero horizontal drift,
	// not a small one, so there is nothing here that could accidentally give rain the same
	// sway snow gets if the kind check below were ever lost.
	st->drift_x = (st->kind == WEATHER_SNOW)
	                  ? sinf(st->drift_phase) * WEATHERDRAW_DRIFT_AMPLITUDE_BLOCKS
	                  : 0.0f;

	st->grid_x = weatherDrawSnap(cam_x, WEATHERDRAW_CELL_BLOCKS);
	st->grid_z = weatherDrawSnap(cam_z, WEATHERDRAW_CELL_BLOCKS);
	st->grid_y = weatherDrawSnap(cam_y, WEATHERDRAW_GRID_Y_SNAP_BLOCKS);
}

// Appends one crossed-billboard quad's six vertices (two triangles, no index buffer -- see
// weatherdraw.h's own note on why, mirroring scene/crackoverlay.c's CrackVertex buffer) at
// out[*vi], advancing *vi by WEATHERDRAW_VERTS_PER_QUAD.
//
// (dir_x, dir_z) is a unit vector in the XZ plane: the quad's own horizontal axis. Two calls
// with perpendicular directions make one column's cross. Corner order is bottom-left,
// bottom-right, top-right, top-left, the same "as seen from outside" convention
// scene/crackoverlay.c's kFaceCorners and kFaceUV both use, which is what fixes the
// (u0 -> u1, v0 -> v1) UV assignment below to the "left to right, bottom to top" mapping
// tools/make_weathertex.py's tile layout expects.
static void weatherDrawEmitQuad(WeatherVertex* out, int* vi,
                                 float center_x, float center_z,
                                 float dir_x, float dir_z,
                                 float half_width, float half_height,
                                 float tile_u0, float tile_u1, float v_top)
{
	float lx = center_x - dir_x * half_width;
	float lz = center_z - dir_z * half_width;
	float rx = center_x + dir_x * half_width;
	float rz = center_z + dir_z * half_width;

	WeatherVertex bl = { lx, -half_height, lz, tile_u0, 0.0f };
	WeatherVertex br = { rx, -half_height, rz, tile_u1, 0.0f };
	WeatherVertex tr = { rx,  half_height, rz, tile_u1, v_top };
	WeatherVertex tl = { lx,  half_height, lz, tile_u0, v_top };

	out[(*vi)++] = bl;
	out[(*vi)++] = br;
	out[(*vi)++] = tr;
	out[(*vi)++] = bl;
	out[(*vi)++] = tr;
	out[(*vi)++] = tl;
}

int weatherDrawBuildVertices(WeatherVertex* out, int max_verts)
{
	if (!out || max_verts < WEATHERDRAW_VERTS_TOTAL)
		return -1;

	// Rain's slice first, then snow's -- the same order weatherDrawKindVertexOffset()
	// returns offsets for, so the two can never disagree about which kind's geometry sits
	// where.
	static const WeatherKind kKinds[WEATHERDRAW_KINDS] = { WEATHER_RAIN, WEATHER_SNOW };

	// A cross's two arms sit at +/-45 degrees to the grid axes (dividing a right angle in
	// half), rather than along the grid axes themselves, purely so a column viewed from
	// exactly along +X, +Z or a diagonal never lines up edge-on with BOTH arms of the cross
	// at once -- one arm is always at least 45 degrees off any axis-aligned or
	// diagonal-aligned view.
	const float kInvSqrt2 = 0.70710678f;

	const float half_n = (float)(WEATHERDRAW_GRID_N - 1) * 0.5f;
	const float half_w = WEATHERDRAW_COLUMN_WIDTH_BLOCKS * 0.5f;
	const float half_h = WEATHERDRAW_COLUMN_HEIGHT_BLOCKS * 0.5f;
	const float v_top = WEATHERDRAW_COLUMN_HEIGHT_BLOCKS / WEATHERDRAW_TEX_WORLD_PER_TILE;

	int vi = 0;
	for (int k = 0; k < WEATHERDRAW_KINDS; k++) {
		int tile = weatherDrawTileForKind(kKinds[k]);
		float tile_u0 = (float)tile / (float)WEATHERDRAW_TILE_COUNT;
		float tile_u1 = (float)(tile + 1) / (float)WEATHERDRAW_TILE_COUNT;

		for (int gz = 0; gz < WEATHERDRAW_GRID_N; gz++) {
			for (int gx = 0; gx < WEATHERDRAW_GRID_N; gx++) {
				float cx = ((float)gx - half_n) * WEATHERDRAW_CELL_BLOCKS;
				float cz = ((float)gz - half_n) * WEATHERDRAW_CELL_BLOCKS;

				weatherDrawEmitQuad(out, &vi, cx, cz,  kInvSqrt2,  kInvSqrt2,
				                    half_w, half_h, tile_u0, tile_u1, v_top);
				weatherDrawEmitQuad(out, &vi, cx, cz,  kInvSqrt2, -kInvSqrt2,
				                    half_w, half_h, tile_u0, tile_u1, v_top);
			}
		}
	}

	return vi;
}

// ── GPU pass -- console build only ──────────────────────────────────────────────────────
#ifdef __3DS__

#include <3ds.h>
#include <citro3d.h>
#include <tex3ds.h>

#include "weather_shbin.h"    // generated by picasso from source/shaders/weather.v.pica
#include "weathertex_t3x.h"   // generated by tex3ds from gfx/weathertex.t3s

// One shader program for the whole process, never torn down -- see scene/crackoverlay.c's
// s_shader_ready for the full account of the use-after-free this avoids (shaderProgramFree
// does not null program->vertexShader, and citro3d separately caches a pointer to the last
// bound program). weatherDrawInit/Exit still run per session; only the shader survives one.
static bool s_shader_ready;

static DVLB_s*         s_dvlb;
static shaderProgram_s s_program;
static int              s_uloc_projection;
static int              s_uloc_modelview;
static int              s_uloc_params;

static C3D_Tex s_tex;

static WeatherVertex* s_verts;   // linearAlloc'ed once per session, rebuilt never -- see
                                  // weatherdraw.h's own header on why a per-frame CPU
                                  // rebuild is not needed at all
static bool s_ready;

bool weatherDrawInit(void)
{
	if (!s_shader_ready) {
		s_dvlb = DVLB_ParseFile((u32*)weather_shbin, weather_shbin_size);
		if (!s_dvlb)
			return false;
		shaderProgramInit(&s_program);
		shaderProgramSetVsh(&s_program, &s_dvlb->DVLE[0]);

		s_uloc_projection = shaderInstanceGetUniformLocation(s_program.vertexShader, "projection");
		s_uloc_modelview  = shaderInstanceGetUniformLocation(s_program.vertexShader, "modelView");
		s_uloc_params     = shaderInstanceGetUniformLocation(s_program.vertexShader, "params");
		s_shader_ready = true;
	}

	Tex3DS_Texture t3x = Tex3DS_TextureImport(weathertex_t3x, weathertex_t3x_size, &s_tex, NULL, true);
	if (!t3x)
		return false;
	Tex3DS_TextureFree(t3x);

	// LINEAR on both filters: this sheet is soft alpha art sampled at a handful of texels
	// across (8x8 per tile), the same reasoning gfx/fogtex.c gives for its own LINEAR ramp
	// -- NEAREST would show hard 8-step contour rings in what is meant to read as a soft
	// streak or fleck. No mip chain (gfx/weathertex.t3s asks tex3ds for none), so only the
	// base level is ever sampled.
	C3D_TexSetFilter(&s_tex, GPU_LINEAR, GPU_LINEAR);

	// U = tile-select, CLAMPED -- a fixed value per vertex that must never wrap into the
	// other tile (see weatherdraw.h's vertex-format note and weather.v.pica's header).
	// V = fall-scroll, REPEAT -- the whole reason tools/make_weathertex.py drew a sheet
	// exactly one tile TALL: V's full [0,1] range IS one tile, so wrapping can only ever
	// repeat that same tile, never bleed into anything else.
	C3D_TexSetWrap(&s_tex, GPU_CLAMP_TO_EDGE, GPU_REPEAT);

	s_verts = (WeatherVertex*)linearAlloc(sizeof(WeatherVertex) * WEATHERDRAW_VERTS_TOTAL);
	if (!s_verts) {
		C3D_TexDelete(&s_tex);
		return false;
	}

	int n = weatherDrawBuildVertices(s_verts, WEATHERDRAW_VERTS_TOTAL);
	if (n != WEATHERDRAW_VERTS_TOTAL) {
		// Unreachable given the constants above agree with each other, but checked rather
		// than assumed -- see weatherDrawBuildVertices's own contract in weatherdraw.h.
		linearFree(s_verts);
		s_verts = NULL;
		C3D_TexDelete(&s_tex);
		return false;
	}

	// Built once by the CPU, read from RAM by the GPU: without this flush the GPU would
	// read whatever the linear heap held before weatherDrawBuildVertices ran. Same call,
	// same reason, as scene/crackoverlay.c and scene/chunk_render.c.
	GSPGPU_FlushDataCache(s_verts, sizeof(WeatherVertex) * WEATHERDRAW_VERTS_TOTAL);

	s_ready = true;
	return true;
}

void weatherDrawExit(void)
{
	if (s_verts)
		linearFree(s_verts);
	s_verts = NULL;

	if (s_ready)
		C3D_TexDelete(&s_tex);

	s_ready = false;

	// The shader is deliberately not freed here -- see s_shader_ready above.
}

void weatherDrawDraw(const C3D_Mtx* projection, const C3D_Mtx* view, const WeatherDrawState* st)
{
	if (!s_ready || !weatherDrawShouldDraw(st))
		return;   // WEATHER_CLEAR, or init never succeeded: draw nothing, cost nothing --
		          // see this file's report for why this is the whole story on a clear day

	int offset = weatherDrawKindVertexOffset(st->kind);
	if (offset < 0)
		return;   // unreachable given weatherDrawShouldDraw() above, checked rather than assumed

	C3D_BindProgram(&s_program);

	// This module's own attribute layout: position then texture coordinate, identical shape
	// to scene/crackoverlay.c's CrackVertex.
	C3D_AttrInfo* attr = C3D_GetAttrInfo();
	AttrInfo_Init(attr);
	AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 3);   // v0 = position
	AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2);   // v1 = texture coordinate

	C3D_BufInfo* buf = C3D_GetBufInfo();
	BufInfo_Init(buf);
	BufInfo_Add(buf, s_verts, sizeof(WeatherVertex), 2, 0x10);

	// Texture unit 2 -- units 0 (atlas) and 1 (fog ramp) are taken; this is the one this
	// task was told is free, and gfx/fogtex.c's own comment already confirms it was unused
	// before this file existed.
	C3D_TexBind(2, &s_tex);

	// No fragment shader: TEV stage 0 REPLACEs with the weather sample, the same shape as
	// scene/crackoverlay.c's own stage 0 and for the same reason -- this pass carries no
	// baked face shade or AO to modulate against.
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE2, 0, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);

	// v1.9.0's rule, followed here as it is in scene/crackoverlay.c and scene/highlight.c:
	// TEV stage 1 and the fog unit are explicitly OWNED, not inherited. A leaked stage 1
	// would blend the rain/snow art toward the sky colour, reading as weaker precipitation
	// rather than as a fog bug -- the same "wrong texture coordinate renders A texture, not
	// an error" trap this project keeps naming.
	C3D_TexEnv* fog = C3D_GetTexEnv(1);
	C3D_TexEnvInit(fog);
	C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);

	// Alpha BLEND, not test: tools/make_weathertex.py's art is soft (anti-aliased), so there
	// is real partial coverage for a blend to resolve, unlike the crack overlay's binary
	// art. Same blend equation source/gfx/sprite.c uses for its own translucent quads.
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
	               GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
	               GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);

	// v1.8.9 integration lane, overriding the paragraph this replaces (kept below in spirit,
	// not text, so the next reader knows this was a deliberate steve-directed change and not
	// a drive-by edit): depth test ENABLED, reading but not writing, so a strip behind a wall
	// or inside a cave is correctly hidden instead of drawing straight through solid terrain.
	// Real (test-enabled, no-write) rather than the shipped no-test, because by the time this
	// runs chunkRenderDraw(view) has already filled the depth buffer with real terrain for
	// this eye (see the call site in drawEye) -- there is a real depth of the world's own to
	// test against now, which is exactly what the paragraph below argued this pass did not
	// have. GPU_WRITE_COLOR only, still no depth WRITE: several overlapping translucent
	// strips (this grid, particles.c's splashes) must not occlude each other or anything
	// drawn after them, only be occluded by what is already opaque.
	//
	// GPU_GEQUAL, not the GPU_LEQUAL steve's own instruction named -- flagged, not silently
	// substituted; see this task's report. This file's vertex shader is fed the SAME
	// projection matrix chunk_render.c's opaque pass uses (chunkRenderProjection(), passed
	// in below), and that pass's own depth test is `C3D_DepthTest(true, GPU_GREATER,
	// GPU_WRITE_ALL)` (chunk_render.c:893) -- GREATER, not LESS. Under this project's actual
	// depth convention (confirmed by that line, not assumed) a fragment closer to the camera
	// stores a LARGER depth value, so "pass where this strip is at least as close as what's
	// already there" -- the occlusion behaviour steve's own sentence right above this block
	// asks for ("terrain depth is in the buffer by then and the strips will be correctly
	// occluded") -- is GEQUAL, matching GREATER's sense. GPU_LEQUAL is the opposite
	// comparison and would make a strip draw only where it is FARTHER than what is already
	// in the buffer -- including the open-sky case, where the buffer holds the clear/far
	// value, so an unoccluded strip would fail its own test and never draw at all. That is a
	// concrete, checkable reason (not a scope objection) the literal instruction as typed
	// cannot produce the described result, so this pass uses GEQUAL instead and names the
	// substitution here rather than either forcing the broken literal value or leaving the
	// pass untouched. Original paragraph, preserved for the reasoning it still carries about
	// WHY a strip needs a depth test at all (now applied, not skipped):
	//
	// No depth test: rain/snow strips intersect the whole visible world by construction (a
	// 12-block-tall grid centred on the camera reaches through walls, hills, water, all of
	// it), and this pass has no sensible depth of its own to test or write -- the same
	// choice source/gfx/sprite.c makes for its own screen-anchored translucent quads.
	// REASONED, not measured: this means a strip nominally "behind" a wall the player is
	// looking at draws in front of it anyway. Accepted for this pass because the alternative
	// (testing against the world's depth buffer) would need this pass to run inside the
	// world's own depth-write geometry pass rather than as a clean, separately-ownable one --
	// exactly the kind of scope-widening decision this task's ownership rule says is not
	// mine to make unasked. Left as an explicit, named trade-off for whoever integrates this
	// to accept or revisit, not a silent gap.
	C3D_DepthTest(true, GPU_GEQUAL, GPU_WRITE_COLOR);

	// Crossed cards, not a solid volume: both faces of each plane must render regardless of
	// which side the camera is on, unlike scene/crackoverlay.c's solid cube.
	C3D_CullFace(GPU_CULL_NONE);

	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uloc_projection, projection);

	C3D_Mtx model, mv;
	Mtx_Identity(&model);
	Mtx_Translate(&model, st->grid_x + st->drift_x, st->grid_y, st->grid_z, true);
	Mtx_Multiply(&mv, view, &model);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uloc_modelview, &mv);

	// The whole per-frame "it is falling" cost: one float. See weather.v.pica's own header.
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uloc_params, st->scroll_v, 0.0f, 0.0f, 0.0f);

	C3D_DrawArrays(GPU_TRIANGLES, offset, WEATHERDRAW_VERTS_PER_KIND);

	// Put back everything this pass changed that the NEXT pass would not expect to inherit
	// -- the same rule scene/chunk_render.c's own transparent pass follows at the end of
	// itself (see its comment on playermodel.c/crackoverlay.c/highlight.c all relying on
	// that reset). Restored to the world pass's own established defaults
	// (scene/chunk_render.c's pipelineBind: GPU_GREATER/GPU_WRITE_ALL depth,
	// GPU_CULL_BACK_CCW), not merely turned off, so whichever pass runs next this frame
	// sees the same state it would if this pass had not run at all.
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	C3D_CullFace(GPU_CULL_BACK_CCW);
}

#endif
