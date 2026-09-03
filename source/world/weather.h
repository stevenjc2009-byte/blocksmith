// Biome-aware weather (v1.8.9, the "weather" half of "Sky and weather"): what is falling, where,
// and the rule for snow piling up on the ground and melting back off it.
//
// ── What this is, and what it deliberately is not ─────────────────────────────────────
//
// This is the weather MODEL: a classification of every world position into clear, rain or snow,
// and the one gameplay effect that classification drives so far — a snow block appearing on top
// of cold ground after it has been snowing there for a while, and disappearing again once it
// stops. It does not draw anything. There are no particles, no shader, no texture and no sound
// in this file, on purpose — see the bottom of this header for what that would need and why it
// is a separate piece of work.
//
// ── Stateless by construction, and why that matters on this console ──────────────────
//
// world/water.h's own header records the fact this design leans on hardest: "a settled lake
// costs nothing per tick" because water's state is a sparse map of only the cells that are
// moving. This system goes one step further and keeps NO side state at all, not even a sparse
// one. weatherAt() is a pure function of (seed, tick, x, z) — no struct, no init, no per-column
// byte anywhere. That was not a nice-to-have: the project's own budget note for this release
// says a loaded column already costs 65,648 bytes on the console and radius 5 sits at 89.2% of
// WORLD_BUDGET_BYTES, so a new per-column allocation was never really on the table. This design
// adds zero.
//
// The only "memory" the system has is the world itself: whether a cell holds BLOCK_SNOW or not
// IS the accumulation state, exactly the way a placed BLOCK_WATER cell already tells the water
// simulation what it needs to know without a side table. A column that unloads and reloads later
// re-derives the identical answer from the same pure function — there is nothing to save,
// nothing to restore, and nothing that can desync between "the state" and "the world" because
// there is only ever one of them.
//
// ── Determinism, and the honest answer about multiplayer ──────────────────────────────
//
// weatherAt() takes an explicit `tick` rather than owning a clock, on purpose: the caller decides
// what "the" tick is, exactly as world/daynight.h's pure time-of-day functions do ("all of these
// take a raw 0..23999 tick... and need no struct to be tested"). Two players computing weatherAt
// from the SAME (seed, tick, x, z) get the same answer without a single byte crossing the wire —
// no new packet, no server round trip, nothing to desync on its own.
//
// **What that does NOT solve, stated plainly**: nothing in this file guarantees two clients agree
// on what `tick` currently IS. That is exactly the gap world/daynight.h documents about its own
// clock ("nothing here yet guarantees two clients share an identical tick value") and is
// deliberately not solved twice. The recommended integration — see weatherAt()'s own comment
// below — is to feed this file whatever tick daynight.c's DayNight is using (dayNightTicks()),
// so weather rides on the exact same synchronisation daynight.c already needs for its own sky.
// The day the day/night packet described in daynight.h's multiplayer note is wired up
// (BS_APP_TIME_SYNC), weather becomes agreed-upon in the same round trip, for free. Until then,
// two independently-run local clocks drift together, and it is one drift, not two.
//
// ── Biome-aware: decided per location, not one flag for the whole world ───────────────
//
// A naive "it is globally raining or not" flag would put snow on a desert the moment a tundra
// biome two thousand blocks away started snowing, which is the exact thing steve asked this NOT
// to be. Instead the world is tiled into weather CELLS at the same physical scale worldgen's own
// biome field already varies at (WEATHER_CELL_SHIFT below is GEN_BIOME_SHIFT, not a coincidence —
// see the constant), and every cell rolls its own "is a front passing through here right now"
// bernoulli, independently of its neighbours, for every weather episode. A whole front therefore
// covers roughly one biome region and different regions can be under completely different
// weather at once — snowing in the tundra while the plains next door stay dry is the ordinary
// case, not an edge one. Within a front that IS passing through a cell, the manifestation still
// depends on the biome sampled at the exact block asked about: cold biomes see snow, temperate
// and jungle biomes see rain, and desert mostly stays dry even inside a "wet" front (a much lower
// chance of its own) — see weatherAt()'s body for the exact thresholds and why they are a
// judgement call, not a measurement.
//
// ── Snow accumulation: gradual, and capped at exactly one block ───────────────────────
//
// steve asked for snow that "piles up in layers, capped at ONE block. Not a snow block that
// appears instantly, and never more than one block deep." This build does not have the parts a
// literal multi-layer snow block would need — see the note at the bottom of this header, "what a
// true multi-layer snow block would cost" — so what ships here is the two requirements that ARE
// achievable with what exists today, delivered exactly:
//
//   * NOT instant. A cell only gets a snow block once it has been classified WEATHER_SNOW for
//     WEATHER_SNOW_ACCUM_TICKS running (see weatherNextSurfaceBlock below) — the snow visibly
//     takes real time to settle rather than popping into existence the instant the front arrives.
//   * NEVER more than one block deep, BY CONSTRUCTION rather than by a bound that has to be
//     checked: the rule only ever writes ONE cell per (x, z) — the air cell directly above the
//     predicted ground — and that cell can only ever hold BLOCK_AIR or BLOCK_SNOW. There is no
//     code path that stacks a second snow block on top of the first, so "capped at one block" is
//     not a limit that could be exceeded by a missed case; there is no second cell it could ever
//     reach.
//
// Melting is the mirror rule, and is also stateless: once a cell has been classified as anything
// OTHER than snow for WEATHER_MELT_TICKS running, an existing snow block there reverts to air.
// "Running" is computed from the CURRENT weather episode's own start, not from a stored timer —
// see weatherAt()'s episode arithmetic — so there is nothing to reset, forget or leak.
//
// ── Why this is safe against a player having edited the ground ────────────────────────
//
// The anchor for "the ground" is worldgenHeight(g, x, z) — the GENERATED height, exactly the
// convention world/world.h's worldStandingY() already uses for spawn-finding, and exactly the
// gap that function's own header warns is a real one: worldgenHeight "knows nothing about what a
// player has built since" worldgen ran. This file does not try to solve that (doing so properly
// needs a real "highest solid block" query maintained incrementally, which is a bigger change
// than this release and belongs with whoever owns that data structure, not with a stateless
// weather pass) — instead it is written so a stale height can only ever produce "nothing happens
// here this visit", never a wrong or dangerous write:
//
//   * Before placing snow, the cell one below the predicted air cell must currently read SOLID.
//     If a player mined the top block away since worldgen ran, that check fails and nothing is
//     written — no snow floating over a hole.
//   * Snow is only ever placed into a cell that currently reads BLOCK_AIR, and only ever removed
//     from a cell that currently reads BLOCK_SNOW. A player who built something else at that
//     exact cell — a torch, a fence post, anything — is never overwritten, because the write is
//     gated on the cell already holding exactly what this system itself would have put there.
//
// The cost of this safety is coverage, not correctness: a column a player has built up well above
// its generated height simply never gets snowed on by this pass, because the pass is still
// looking at the old height. That is a real, stated gap — not a hidden one — and it is the same
// shape of gap worldStandingY's own header names for exactly the same underlying reason.
//
// ── Cost ─────────────────────────────────────────────────────────────────────────────
//
// Zero persistent bytes (see "stateless by construction" above). The only per-call cost is
// query work: weatherAt() is one worldgenBiomeAt() call (already paid by worldgen, and the same
// cost class as every biome-driven decoration pass in worldgen.c) plus one rngHash3. Applying it
// to a cell (weatherStepCell) adds one worldgenHeight() call and up to two worldGet() plus one
// worldSet(). See weather.c's header for the per-tick budget this suggests at the Old 3DS's
// render radius, and weather_test.c / docs/plan-1.8.9-weather.md for the numbers and their
// provenance.
//
// ── No <3ds.h> ───────────────────────────────────────────────────────────────────────
//
// Same discipline as world/tick.h, world/daynight.h and world/worldgen.h: everything here is
// plain C over world/world.h and world/worldgen.h, neither of which pulls in <3ds.h> either, so
// the host suite (tests/weather_test.c) exercises the real rule directly instead of a copy of it.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "world/block.h"
#include "world/world.h"
#include "world/worldgen.h"

// What is falling at a location right now. Not a global flag — see the header above — and never
// stored: every caller re-derives it from weatherAt().
typedef enum {
	WEATHER_CLEAR = 0,
	WEATHER_RAIN,
	WEATHER_SNOW,
} WeatherKind;

// ── The weather cell grid ───────────────────────────────────────────────────────────────
//
// Deliberately the SAME shift worldgen.h's GEN_BIOME_SHIFT uses for its own biome field (128
// blocks), rather than a second magic number invented here. Two independent reasons, both real:
//
//   * a weather front that is the same physical size as a biome region is what makes "it snows
//     where it should snow" hold up as you walk around, rather than a front straddling a dozen
//     biomes or fitting inside a single one;
//   * reusing worldgen.h's own constant means the two scales cannot drift apart on a future
//     retune of the biome field without this file being touched in the same diff (this file
//     already includes world/worldgen.h for WorldGen and BiomeId, so the dependency costs
//     nothing new).
//
// Weather cell coordinates are block coordinates shifted right by this amount, using the same
// arithmetic-shift-is-floor convention world/world.h states for chunk coordinates: "Arithmetic
// shift gives the right floor behaviour for negative coordinates, which plain division does not."
#define WEATHER_CELL_SHIFT GEN_BIOME_SHIFT

// ── The episode clock ───────────────────────────────────────────────────────────────────
//
// A weather EPISODE is the span one front's precipitation roll stays fixed for a given cell.
// Ticks are partitioned into fixed-length episodes by a shift, not a division: world/rng.h's own
// header records that "the ARM11 has no integer divide instruction", and while a compile-time
// constant divisor is normally folded into a multiply by the compiler anyway, a power-of-two
// period turns the whole operation into a shift and a mask with no ambiguity about whether that
// folding happened. 13 bits is 8192 ticks, 409.6 real seconds at world/tick.h's TICK_HZ of 20 —
// about six minutes and fifty seconds per front. This number is a JUDGEMENT CALL made by eye
// against how long a Minecraft-style shower reads as "weather" rather than "a flicker" or "the
// whole session", not a measured or play-tested figure — the same honesty world/worldgen.h's own
// GEN_FLAT_FRACTION comment applies to its own by-eye constant. It is the first knob to retune if
// a playtest says fronts feel too short or too long, and it is one #define.
#define WEATHER_EPISODE_SHIFT 13
#define WEATHER_EPISODE_TICKS (1u << WEATHER_EPISODE_SHIFT)   // 8192

_Static_assert((WEATHER_EPISODE_TICKS & (WEATHER_EPISODE_TICKS - 1)) == 0,
               "WEATHER_EPISODE_TICKS must stay a power of two -- the mask arithmetic in "
               "weather.c assumes it, and a non-power-of-two would silently misclassify which "
               "tick each episode starts on");

// How long a cell must sit classified WEATHER_SNOW, continuously within the CURRENT episode,
// before weatherNextSurfaceBlock() will place a snow block there — see the header above for why
// this is what makes accumulation gradual rather than instant. 2400 ticks is 120 real seconds at
// TICK_HZ 20, two minutes: long enough that a player walking through a light flurry does not see
// the ground snow over behind them before they have gone ten paces, short enough that standing
// somewhere and watching it happen does not feel like nothing is going on. Judgement, not
// measurement, exactly as WEATHER_EPISODE_SHIFT above is not.
#define WEATHER_SNOW_ACCUM_TICKS 2400u

// How long a cell must sit classified as anything OTHER than WEATHER_SNOW, continuously within
// the current episode, before an existing snow block there melts. Half the accumulation time —
// 1200 ticks, 60 seconds — on the same by-eye basis: melting reads as an obviously-triggered
// event (the front visibly left) so it can afford to happen a little faster than the pile-up did
// without looking rushed.
#define WEATHER_MELT_TICKS 1200u

_Static_assert(WEATHER_SNOW_ACCUM_TICKS < WEATHER_EPISODE_TICKS,
               "a cell must be able to finish accumulating snow within a single episode, or a "
               "cell that rolls SNOW every episode without ever landing on the SAME episode "
               "twice in a row (which cannot happen -- see weatherAt below -- but the constants "
               "must not silently rely on that) could never actually place the block");
_Static_assert(WEATHER_MELT_TICKS < WEATHER_EPISODE_TICKS,
               "a cell must be able to finish melting within a single non-snow episode, for the "
               "same reason as the accumulation assert above");

// ── Classification chances, out of 256 — the same idiom worldgen.h's GEN_GRASS_*, GEN_TREE_* "
// and GEN_CACTUS_CHANCE tables all use, so a byte of hash decides the question with no division.
//
// ONE roll per (weather cell, episode) decides whether a front is passing through that cell at
// all (WEATHER_CHANCE_PRECIP for every biome except desert, WEATHER_CHANCE_DESERT for desert) —
// see weatherAt()'s body. Splitting deserts onto their own, much lower chance rather than giving
// every biome the same roll and then zeroing deserts out afterwards is deliberate: a desert
// biome sitting inside an otherwise-precipitating front should mostly, not always, stay dry —
// real deserts do occasionally get rain — and folding that into the SAME roll (rather than a
// second independent one) keeps one hash answering one question per cell per episode.
//
// 64/256 (25%) and 8/256 (~3%) are both placed by eye against no existing ruler in this codebase
// — there is no prior "how often should it rain" measurement to place them against the way
// worldgen.h's tall-grass chance was placed against a measured meadow density. Stated as a
// judgement call rather than dressed as a measurement.
#define WEATHER_CHANCE_PRECIP 64u
#define WEATHER_CHANCE_DESERT 8u

_Static_assert(WEATHER_CHANCE_DESERT < WEATHER_CHANCE_PRECIP,
               "the desert chance is supposed to be the rare case; if this is ever raised above "
               "the ordinary chance the two constants have swapped meaning by accident");

// The weather classification at one world block position, at one tick. Pure: no allocation, no
// side effect, safe to call from any thread that already owns `g` read-only (the same rule
// worldgenBiomeAt documents for itself).
//
// `tick` is whatever the caller's authoritative clock says right now — see the header's
// multiplayer note above for what "authoritative" does and does not mean today. Passing
// dayNightTicks() from whichever DayNight the caller already owns is the recommended source: it
// is the shared clock every other per-tick system in this release is built against, and it means
// this file does not have to know how ticks reach it.
//
// Answers for ANY world, including a legacy or density one — worldgenBiomeAt() itself makes the
// same promise and for the same reason: nothing here is gated on g->version, only on the biome
// the position resolves to, which is defined for every generator this build knows about.
WeatherKind weatherAt(const WorldGen* g, uint64_t tick, int32_t x, int32_t z);

// ── The accumulation/melt rule, as a pure function ─────────────────────────────────────
//
// Deliberately factored out from the world-touching code below it, exactly so it can be tested
// with a hand-picked table of (kind, ticks_into_episode, current) -> expected and no WorldGen or
// World in sight — the same shape world/daynight.c's own pure functions of a tick are tested at.
//
// `ticks_into_episode` is `tick & (WEATHER_EPISODE_TICKS - 1)`, i.e. how far the CURRENT episode
// has run at the position weatherAt() was asked about. `current` is whatever block presently
// occupies the one cell this system ever touches for this (x, z) — the air cell directly above
// the predicted ground. Returns the block that cell should hold; equal to `current` when nothing
// should change.
//
// The whole rule, stated once so weather.c's body does not have to be read to know it:
//
//   kind == WEATHER_SNOW, current == BLOCK_AIR,  ticks_into_episode >= ACCUM  -> BLOCK_SNOW
//   kind != WEATHER_SNOW, current == BLOCK_SNOW, ticks_into_episode >= MELT   -> BLOCK_AIR
//   every other input                                                        -> current unchanged
//
// Note what is NOT a case: `current` already BLOCK_SNOW while `kind == WEATHER_SNOW` returns
// BLOCK_SNOW unchanged — that is the one-block cap, expressed as "there is no rule that adds a
// second layer" rather than as a check that could be gotten wrong. And `current` holding anything
// other than BLOCK_AIR or BLOCK_SNOW (a player's torch, a plant, a tree trunk that grew there
// since) always returns `current` unchanged in both branches — this function never proposes
// overwriting anything it did not itself place.
BlockId weatherNextSurfaceBlock(WeatherKind kind, uint64_t ticks_into_episode, BlockId current);

// Applies the rule at exactly one world block column (x, z): reads the predicted ground height
// and the block presently above it, asks weatherAt() and weatherNextSurfaceBlock(), and writes
// the result back through worldSet() if and only if it differs from what is there now. Returns
// true if a write happened (for a caller that wants to know, e.g. to remesh/relight that cell —
// see docs/plan-1.8.9-weather.md's rendering-hook-up section for who would want that and why
// this file does not call it itself).
//
// Safe to call on a cell whose ground has moved since worldgen ran — see the header's "why this
// is safe against a player having edited the ground" section for exactly what that does and does
// not cover.
bool weatherStepCell(const WorldGen* g, World* w, uint64_t tick, int32_t x, int32_t z);

// Applies weatherStepCell to a small, deterministic, opportunistic slice of one loaded column's
// 16x16 surface cells — WEATHER_CELLS_PER_VISIT of them, chosen from `tick`'s low bits so
// different calls (whenever the caller makes them) tend to land on different cells, without this
// file owning a per-column cursor to remember where it left off. A column visited only rarely —
// far from the player, wherever the caller's own decimation schedule puts it — updates slowly and
// unevenly, and that is fine: there is no requirement that every cell of a column agree at the
// same tick, only that each one eventually converges to what weatherAt() says it should be, which
// it does the next time this function happens to land on it.
//
// **Caller decides the schedule; this file does not.** world/tick.h's own header says exactly
// what it is for here: "Use it rather than writing another scheduler." A caller in main.c (which
// this file cannot touch — see docs/plan-1.8.9-weather.md's hand-off section) is expected to call
// this once per loaded column at a period from tickPeriodForDistSq(), the same near/far
// decimation every other per-tick system in this release already uses.
//
// Returns how many of the up to WEATHER_CELLS_PER_VISIT cells actually changed a block.
#define WEATHER_CELLS_PER_VISIT 4

int weatherTickColumn(const WorldGen* g, World* w, uint64_t tick, int32_t cx, int32_t cz);

// ── What rendering this would need — scoped, not built ────────────────────────────────
//
// Nothing above draws anything, and nothing in this file's scope does. For whoever picks up the
// visible half:
//
//   * There is no particle system anywhere in this project (checked: zero hits for "particle"
//     across source/ and tools/). Rendering falling rain or snow means building the project's
//     first one, not plugging into an existing one — this is a bigger piece of work than it
//     sounds, and a prior blueprint in this project's history (scoped when this was one larger
//     task, before the biome system existed) sized the visible half at roughly a dozen new files
//     — a vertex format, a shader, a texture generator, a render pass with its own GPU state
//     save/restore, and a hardware frame-time reading, because this console's emulator reports a
//     constant for GPU time and cannot answer the one question ("does this fit the frame") that
//     matters most.
//
//     ── v1.8.17 / 2026-09-03 correction ────────────────────────────────────────────────────
//     Both counts above were accurate the moment they were written and stopped being true
//     within the same day. "Zero hits for 'particle'" was correct when this section landed in
//     v1.8.8 (commit 6e56f52, 2026-09-02 09:54): grepping that commit's tree for "particle"
//     across source/ and tools/ turns up exactly one hit — this comment's own use of the
//     word. It stopped being true 94 minutes later, in the very next commit, v1.8.9 (051a588,
//     2026-09-02 11:28), which shipped source/gfx/particles.c and source/gfx/particles.h — a
//     real particle system: a 512-slot pool with O(1) ring recycling, used for splash scatter
//     (see its own call sites in source/scene/player.c). Grepping the tree today (2026-09-03,
//     `grep -rlI -i particle source tools`) returns 11 files, not zero — particles.c/h,
//     weatherdraw.c/h, main.c, player.c, entitymodel.c/h, shaders/particle.v.pica, this file,
//     and tools/run_host_tests.sh.
//
//     The "roughly a dozen new files" estimate for the visible rendering half landed in that
//     same v1.8.9 commit, and the real count came in lighter: 8 new files, not a dozen —
//     source/gfx/weatherdraw.c, source/gfx/weatherdraw.h, source/shaders/weather.v.pica,
//     tools/make_weathertex.py, gfx/weathertex.png, gfx/weathertex.t3s,
//     tests/weatherdraw_test.c, and docs/plan-1.8.9-weather-integration.md. (particles.c/h and
//     its shader/test were a separate, additional system built in the same commit for splash
//     scatter, not part of this estimate.) The render pass this bullet scoped is real and
//     wired in: source/gfx/weatherdraw.c, called from source/main.c
//     (weatherDrawInit/SetState/Update/Draw/Exit — grep those names in main.c), and it does
//     get driven by weatherAt() at the cadence and query shape the next bullet describes,
//     exactly as scoped — see main.c's per-tick weather loop, which calls weatherAt() and
//     feeds the result to weatherDrawSetState(). What was NOT done: the hardware frame-time
//     reading. weatherdraw.c carries no such instrumentation, and v1.8.9's own commit message
//     (051a588) says plainly "nothing here has run on real 3DS hardware." Read weatherdraw.c
//     and weatherdraw.h for what actually shipped and which parts of this bullet's design they
//     followed.
//
//   * The natural query surface for that pass is exactly weatherAt(g, tick, x, z), asked at the
//     player's own position (or a small neighbourhood of it) a few times a second — not per
//     particle, not per frame — to decide whether to spawn rain or snow geometry at all and
//     which. This file was written so that hook-up is the whole of the integration on the model
//     side: nothing about weatherAt()'s signature needs to change for a renderer to consume it.
//   * "What a true multi-layer snow block would cost", for whoever wants Minecraft's literal 1-8
//     layer accumulation instead of this release's single-block gradual placement: a new
//     BLOCK_SHAPE (world/block.h's shape enum has three bits reserved for exactly this, per its
//     own comment, but no mesher geometry for a partial-height cube exists yet — that is a
//     world/mesher.c change, outside this file's ownership this release), a place to store the
//     1-8 count per cell (chunk.h's storage is one BlockId per cell today, no metadata channel —
//     see chunk.h's own header for the three storage forms, none of which carry a second byte),
//     and a registry/wire change, which world/block.h's own BLOCK_COUNT comment and
//     world/water.h's registryCrc16 note both explain is a cross-repo lockstep with
//     deps/blocksmith-server, not a local edit. None of that is attempted here; it is flagged so
//     the decision is steve's rather than assumed.
