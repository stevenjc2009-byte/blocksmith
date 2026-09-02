// Blocksmith v1.8.9 — the VISIBLE half of weather: camera-following rain/snow billboard
// strips. world/weather.c decides WHAT is falling; this file is the only thing that draws
// it, and it draws nothing yet — see the bottom of this header for exactly what "yet" means.
//
// ── What this is, and the one thing it deliberately does NOT do ────────────────────────
//
// docs/plan-1.8.9-weather.md §6 scoped the render pass out of the weather-model task and
// pointed at vanilla Minecraft's own approach as the cheap path on this hardware: a small
// grid of camera-following vertical quads with a texture that scrolls to look like it is
// falling, not a per-particle system. That is what this file builds. There is still no
// particle system anywhere in this project after this file — these are STATIC quads, built
// once and never re-uploaded; the only things that change per frame are two float uniforms
// (a scroll offset and, for snow, a drift offset) and the model matrix that recentres the
// whole static grid near the camera. See weatherdraw.c's own header for the full cost
// account of why that split (static geometry, dynamic uniforms) is what makes this cheap.
//
// This file does NOT call weatherAt(). world/weather.h's own header says plainly what the
// render pass's query surface should be: weatherAt(g, tick, x, z), asked "at the player's
// own position... a few times a second... not per particle, not per frame" — that sentence
// describes an EXTERNAL caller deciding a cadence, not something this file should decide for
// itself. Pulling WorldGen/World into a GPU-drawing file would also mean this file's host
// test would need a real generated world just to prove a vertex layout, which is exactly the
// kind of test-doesn't-match-the-claim mismatch this project avoids elsewhere. Instead:
//
//   * This header includes world/weather.h for exactly one thing — the WeatherKind enum
//     itself (WEATHER_CLEAR / WEATHER_RAIN / WEATHER_SNOW) — so this file's notion of "what
//     is falling" is never a second, redeclared copy of that enum that could drift out of
//     sync with the real one. That satisfies "call it, do not re-derive it" at the type
//     level: this file consumes the exact classification weatherAt() produces, verbatim, and
//     contains no biome logic of its own to re-derive it with.
//   * The actual weatherAt() CALL is left to the integration lane, in main.c, at whatever
//     cadence it chooses (a natural choice: once per weatherTickColumn-style visit, or on a
//     fixed timer of a few times a second, exactly as weather.h recommends) — see
//     docs/plan-1.8.9-weather-integration.md for the exact call this file expects to receive
//     the result of, through weatherDrawSetState() below.
//
// ── The API a later integration lane calls ──────────────────────────────────────────────
//
// Three GPU-facing calls, deliberately shaped like every other pass in source/gfx and
// source/scene (crackoverlayInit/Exit/Draw, spriteBegin/.../End):
//
//   weatherDrawInit()                                    — once, at boot or world join
//   weatherDrawSetState(kind)                             — whenever the caller's own
//                                                            weatherAt() poll changes
//   weatherDrawUpdate(dt, camX, camY, camZ)                — once a frame, before drawing
//   weatherDrawDraw(projection, view)                      — once a frame, in the draw loop
//   weatherDrawExit()                                     — once, at world leave/shutdown
//
// Below that GPU surface, the logic that decides WHERE the quads go and WHAT their UVs are
// is plain C with no citro3d in sight (weatherDrawStateInit/SetState/Update/BuildVertices
// and the small helpers below them), for the same reason source/app/battery.c splits the bar
// arithmetic from the PTMU polling: tools/run_host_tests.sh links THIS half directly, so the
// grid placement and scroll arithmetic under test is the exact copy that ships, not a
// hand-written twin of it.
//
// ── Nothing calls this yet ───────────────────────────────────────────────────────────────
//
// No file in this tree calls weatherDrawInit, weatherDrawSetState, weatherDrawUpdate or
// weatherDrawDraw. That is by design, not an oversight: this task's hand-off is
// docs/plan-1.8.9-weather-integration.md, which states the exact lines for main.c to call
// these at, because this file is not permitted to edit main.c itself (two other lanes are
// live on it). Until that integration lands, weatherDrawInit() existing and being callable
// is the whole of what "done" means for this file.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "world/weather.h"   // for WeatherKind only -- see the header above

// ── Grid layout constants ────────────────────────────────────────────────────────────────
//
// A GRID_N x GRID_N array of "columns" in world XZ, centred under the camera and snapped to
// CELL_BLOCKS so the grid shifts in whole-cell jumps as the player moves rather than sliding
// continuously (the same reason vanilla Minecraft's own rain grid snaps rather than tracking
// the player's exact fractional position — a continuously-translating backdrop of falling
// streaks reads as the WORLD sliding, not as rain falling in a static world). GRID_N is
// odd so a column sits exactly centred under the camera rather than straddling it.
//
// All four of these are JUDGEMENT CALLS, by eye against no existing ruling in this project
// (there is no prior "how much rain coverage looks right" measurement to place them against,
// the same honesty world/weather.h's own WEATHER_CHANCE_PRECIP comment applies to itself) —
// not measurements, and the first knobs to retune once steve can actually look at this.
#define WEATHERDRAW_GRID_N            5      // 5x5 = 25 columns
#define WEATHERDRAW_CELL_BLOCKS       3.0f   // spacing between column centres
#define WEATHERDRAW_COLUMN_WIDTH_BLOCKS  1.25f
#define WEATHERDRAW_COLUMN_HEIGHT_BLOCKS 12.0f   // vertical extent of one strip, centred on the camera

#define WEATHERDRAW_COLUMNS (WEATHERDRAW_GRID_N * WEATHERDRAW_GRID_N)

// Snow's horizontal drift amplitude, and how coarsely the grid's vertical origin snaps to
// the camera's height. Public (rather than kept private to weatherdraw.c) so the host test
// can check weatherDrawUpdate's output against the SAME numbers the implementation uses,
// instead of a second, hand-copied pair of magic numbers that could silently drift out of
// sync with the real ones -- see weatherdraw.c's own header on why each value is what it is.
#define WEATHERDRAW_DRIFT_AMPLITUDE_BLOCKS 0.6f
#define WEATHERDRAW_GRID_Y_SNAP_BLOCKS     4.0f

// Two crossed quads per column (the same double-plane cross a billboarded grass clump uses:
// two quads 90 degrees apart in the horizontal plane so the strip reads as "there" from any
// direction the camera happens to be facing, without the per-frame cost of actually turning
// the quads to face the camera every frame -- these are fixed-orientation, not true
// billboards, which is what keeps the per-frame cost to "a matrix and two floats" rather
// than "rebuild the buffer"). Two triangles per quad, six vertices per triangle pair, no
// index buffer -- the same idiom scene/crackoverlay.c's CrackVertex buffer uses and for the
// same stated reason: baking the fan split at generation time means there is no index
// arithmetic left in the draw path to get wrong.
#define WEATHERDRAW_QUADS_PER_COLUMN   2
#define WEATHERDRAW_VERTS_PER_QUAD     6
#define WEATHERDRAW_VERTS_PER_COLUMN  (WEATHERDRAW_QUADS_PER_COLUMN * WEATHERDRAW_VERTS_PER_QUAD)   // 12

// One kind's worth of geometry -- the full grid, baked once.
#define WEATHERDRAW_VERTS_PER_KIND (WEATHERDRAW_COLUMNS * WEATHERDRAW_VERTS_PER_COLUMN)   // 300

// Rain and snow are baked back to back in the SAME buffer, exactly as crackoverlay.c bakes
// all eight crack stages into one buffer and selects between them with a first-vertex
// offset at draw time rather than rebuilding anything when the stage (here: the kind)
// changes. WEATHER_CLEAR draws nothing and has no slot.
#define WEATHERDRAW_KINDS 2   // rain, snow
#define WEATHERDRAW_VERTS_TOTAL (WEATHERDRAW_KINDS * WEATHERDRAW_VERTS_PER_KIND)   // 600

// Tile indices into the two-tile weathertex.png sheet (see tools/make_weathertex.py). Fixed
// by the texture layout, not by WeatherKind's own numbering -- WEATHER_CLEAR=0 in that enum
// would otherwise look like it means "tile 0" and it does not, so the mapping is made
// explicit here via weatherDrawTileForKind() rather than left as an implicit cast.
#define WEATHERDRAW_TILE_RAIN 0
#define WEATHERDRAW_TILE_SNOW 1
#define WEATHERDRAW_TILE_COUNT 2

// ── Vertex format ────────────────────────────────────────────────────────────────────────
//
// Position is local to the grid's own origin (the snapped camera-following point
// weatherDrawUpdate computes) -- the model matrix uploaded at draw time translates (and, for
// snow, adds the drift offset to) this local geometry into world space, the same
// model-matrix-carries-the-placement idiom scene/crackoverlay.c uses for its own per-block
// cube. Float, not the world's packed 8-bit MeshVertex, for the same reason crackoverlay's
// CrackVertex and scene/highlight.c's HlVertex are both float: this geometry does not live on
// a block-grid attribute scale, it lives on a "several blocks around a floating camera" scale.
//
// v is the FALL-SCROLL axis (tools/make_weathertex.py's texture is exactly one tile TALL on
// this axis for exactly this reason: GPU_REPEAT must never wrap into a neighbouring, unrelated
// tile). v runs from 0.0 at the bottom of a column to
// WEATHERDRAW_COLUMN_HEIGHT_BLOCKS / WEATHERDRAW_TEX_WORLD_PER_TILE at the top, so the texture
// repeats once per WEATHERDRAW_TEX_WORLD_PER_TILE world blocks of height; weather.v.pica adds
// a per-frame scroll uniform to this coordinate before sampling, so the CPU never touches this
// buffer again after it is built once. (An earlier draft of this header had u and v the other
// way around -- see tools/make_weathertex.py's own note on that mistake and why it is worth
// naming rather than quietly fixing.)
//
// u is the TILE-SELECT axis (rain vs snow) and is NOT animated -- it spans exactly the range
// [WEATHERDRAW_TILE_RAIN/2, (WEATHERDRAW_TILE_RAIN+1)/2] or the snow equivalent per vertex
// (0..0.5 for the rain tile, 0.5..1.0 for the snow tile, matching tools/make_weathertex.py's
// side-by-side placement), varying only across the quad's own WIDTH so the tile's full 2D
// shape still shows rather than being sampled along a single fixed line -- the same "vary
// across the geometry, not just at one corner" requirement any 2D texture sample needs.
#define WEATHERDRAW_TEX_WORLD_PER_TILE 1.0f   // one texture repeat per this many world blocks of height

typedef struct {
	float x, y, z;
	float u, v;
} WeatherVertex;

// ── Per-frame state, advanced by weatherDrawUpdate ──────────────────────────────────────
//
// Everything here is plain floats and one enum -- no pointer, no allocation -- so a caller
// (and the host test) can put one on the stack.
typedef struct {
	WeatherKind kind;      // WEATHER_CLEAR means "draw nothing", set by weatherDrawSetState

	float scroll_v;         // current fall-scroll offset, wrapped into [0, 1) -- see below
	float drift_phase;      // radians, wrapped into [0, 2*pi) -- see below
	float drift_x;           // current horizontal drift offset in blocks, snow only

	// The snapped grid origin: where the static geometry's local (0,0,0) currently sits in
	// world space. Recomputed every weatherDrawUpdate call from the camera position, snapped
	// to WEATHERDRAW_CELL_BLOCKS in X/Z (see the grid-layout comment above) and to a coarser
	// step in Y (a strip this tall does not need to re-centre vertically nearly as often as
	// the player's feet move horizontally -- see weatherdraw.c's weatherDrawUpdate for the
	// exact step and why).
	float grid_x, grid_y, grid_z;
} WeatherDrawState;

// Zeroes `st` to "clear, no scroll, grid centred at the origin". Call once before the first
// weatherDrawSetState/weatherDrawUpdate.
void weatherDrawStateInit(WeatherDrawState* st);

// Records what is currently falling. Idempotent and cheap enough to call every frame if a
// caller wants to (it does nothing but store an enum), but the intended cadence is whatever
// the caller's own weatherAt() poll runs at -- see the header above.
void weatherDrawSetState(WeatherDrawState* st, WeatherKind kind);

// Advances the scroll/drift accumulators by `dt` seconds and recomputes the snapped grid
// origin from the camera position. Pure -- no citro3d, safe to call from the host test with
// a hand-picked dt and camera path. Call once a frame, before weatherDrawDraw.
void weatherDrawUpdate(WeatherDrawState* st, float dt, float cam_x, float cam_y, float cam_z);

// True while `st->kind` is anything other than WEATHER_CLEAR -- i.e. while there is
// something for weatherDrawDraw to actually draw. Exposed so a caller (or the host test)
// can check the same condition weatherDrawDraw itself uses without needing a citro3d
// context to do it.
bool weatherDrawShouldDraw(const WeatherDrawState* st);

// The fall-scroll speed for `kind`, in scroll-units (whole texture repeats) per second.
// Rain is deliberately several times snow's speed -- "rain falls fast... snow falls slower"
// is the task's own requirement, and this is the one number that encodes it. Both are
// JUDGEMENT CALLS by eye, not measurements -- there is no reference frame-capture of real
// Minecraft rain to clock a speed against, and this project does not use assets from that
// game to begin with (see tools/make_weathertex.py's own header). Returns 0.0f for
// WEATHER_CLEAR (never read -- weatherDrawDraw skips the draw entirely first).
float weatherDrawFallSpeed(WeatherKind kind);

// Maps a WeatherKind to its tile index in the two-tile sheet (WEATHERDRAW_TILE_RAIN /
// WEATHERDRAW_TILE_SNOW), or -1 for WEATHER_CLEAR. See the tile-index comment above for why
// this indirection exists instead of casting the enum directly.
int weatherDrawTileForKind(WeatherKind kind);

// Maps a WeatherKind to its first-vertex offset into the buffer weatherDrawBuildVertices
// produces (a multiple of WEATHERDRAW_VERTS_PER_KIND), or -1 for WEATHER_CLEAR. Every
// kind's slice is exactly WEATHERDRAW_VERTS_PER_KIND vertices long.
int weatherDrawKindVertexOffset(WeatherKind kind);

// Fills `out` (which must hold at least WEATHERDRAW_VERTS_TOTAL entries) with the full
// static grid for BOTH kinds back to back -- rain's WEATHERDRAW_VERTS_PER_KIND vertices
// first, at weatherDrawKindVertexOffset(WEATHER_RAIN) (0), then snow's at
// weatherDrawKindVertexOffset(WEATHER_SNOW) (WEATHERDRAW_VERTS_PER_KIND). Returns
// WEATHERDRAW_VERTS_TOTAL on success, or -1 if `max_verts` is too small to hold that many
// (the same "tell the caller rather than overrun" contract crackoverlay's fixed-size
// CRACK_VERTS buffer enforces by construction at compile time -- this function enforces it
// at the call site instead, since it is also meant to be driven from a host test with a
// deliberately undersized buffer).
//
// Pure and deterministic: called once by weatherDrawInit() below the __3DS__ guard, and
// directly by the host test with no citro3d in sight.
int weatherDrawBuildVertices(WeatherVertex* out, int max_verts);

// ── GPU pass -- console build only ──────────────────────────────────────────────────────
#ifdef __3DS__

#include <citro3d.h>

// Builds the shader program (permanent for the process -- see weatherdraw.c's own note on
// why, the same shaderProgramFree use-after-free scene/crackoverlay.c and scene/highlight.c
// both already carry a note about), loads gfx/weathertex.t3x onto texture unit 2, and
// linearAlloc's + fills the static WEATHERDRAW_VERTS_TOTAL-vertex buffer via
// weatherDrawBuildVertices(). Call once, at boot or world join. Returns false (and leaves
// the module degraded to "draws nothing" rather than crashing) if any allocation or shader
// load fails.
bool weatherDrawInit(void);

// Frees the per-session vertex buffer and texture. The shader program is deliberately NOT
// freed -- see weatherdraw.c. Call at world leave/shutdown.
void weatherDrawExit(void);

// Draws the currently active kind's strip (nothing if the state is WEATHER_CLEAR or
// weatherDrawInit() has not succeeded). `projection` and `view` are the same matrices the
// world pass itself uses (scene/chunk_render.c's chunkRenderProjection() and its own view),
// passed in rather than reached for globally so this file stays decoupled from
// scene/chunk_render.c -- see docs/plan-1.8.9-weather-integration.md for exactly where the
// integration lane should source them.
void weatherDrawDraw(const C3D_Mtx* projection, const C3D_Mtx* view, const WeatherDrawState* st);

#endif
