// v1.8.13 — health, hunger and fall damage. The survival half of survival mode.
//
// Nothing in here includes <3ds.h>, for the reason world/inventory.h states about itself:
// the rules that are actually easy to get subtly wrong (a countdown that fires one tick
// early, a fall measured from the wrong height, a floor that is really a cap) are checked
// by host gcc in under a second rather than by relaunching an emulator and squinting at a
// health bar. world/survival_test.c is that check.
//
// ── The two clocks, and why there are two ──────────────────────────────────────────────
//
// Hunger and health run on DIFFERENT periods — 1200 ticks and 80 — so one countdown cannot
// serve both. They are separate fields rather than one counter with a modulo because a
// modulo ties the two phases together forever: eating would have to leave the regen phase
// alone while resetting nothing, and the 80-tick regen would silently re-align itself every
// time the 1200-tick hunger drain came round. Two uint16_t is four bytes.
//
// `regen_ticks` IS shared between regen and starvation, and that sharing is safe rather than
// lucky: regen runs while hunger >= 18 and starvation while hunger == 0, and since 18 > 0
// those two conditions cannot both be true. One counter, one period, two mutually exclusive
// consumers. See survivalTick() in survival.c for what happens in the band between them.
//
// ── What can kill you, and what deliberately cannot ────────────────────────────────────
//
// Fall damage CAN kill. Starvation CANNOT — it floors at 1 health and holds there. That
// asymmetry is a decision, not an oversight, and it is the one thing in this module most
// likely to be "tidied" by someone who reads only one of the two. A player who wanders too
// far from food is left alive and able to walk home; a player who jumps off a mountain is
// not. Both are pinned by survival_test.c.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "world/inventory.h"
#include "world/physics.h"
#include "world/world.h"

#define SURVIVAL_MAX_HEALTH 20
#define SURVIVAL_MAX_HUNGER 20

// Ticks between events, at the 20 TPS world/tick.h runs at. Named rather than spelled at the
// use site so survival_test.c can assert against the same constant the code branches on, and
// so "60 seconds" and "4 seconds" are derivable from one place.
#define SURVIVAL_HUNGER_PERIOD 1200   // 60 s: one hunger point
#define SURVIVAL_REGEN_PERIOD    80   // 4 s: one health point, up OR down

// Regen needs hunger AT OR ABOVE this. Starvation needs hunger at exactly 0.
#define SURVIVAL_REGEN_HUNGER    18

// Blocks of fall that cost nothing. damage = floor(peak_y - landing_y) - this.
#define SURVIVAL_FALL_FREE        3

typedef struct {
	uint8_t  health;        // 0..20
	uint8_t  hunger;        // 0..20
	uint16_t hunger_ticks;  // countdown to next -1 hunger, at 20 TPS
	uint16_t regen_ticks;   // countdown to next +1 health / next starvation tick
} Survival;

// The fall tracker is SEPARATE from Survival because it is per-body, not per-player: it
// carries no health of its own and is reset by landing rather than by dying. Keeping it out
// of Survival is also what lets survivalSave() have a two-byte payload — a half-finished fall
// is not a thing worth persisting across a world close, and persisting it would mean deciding
// what a fall interrupted by a quit is supposed to do.
typedef struct {
	bool  falling;
	float peak_y;           // highest feet-y since on_ground last went false
} FallTrack;

void survivalInit(Survival* s);
void fallTrackInit(FallTrack* ft);

// 20 TPS. Hunger drain, regen, starvation. Returns true if the player JUST died this tick.
//
// Under the rules this version ships that return value is ALWAYS false, because the only
// health loss on this path is starvation and starvation floors at 1. It is computed honestly
// (health was non-zero, health is now zero) rather than written as `return false`, so the day
// a tick-driven lethal effect is added — poison, drowning, a lava tick — the caller's death
// handling is already wired and the change is confined to this file. survival_test.c pins
// the false, so a future effect that starts killing on this path shows up as a red check
// rather than as a player who cannot die.
bool survivalTick(Survival* s);

// Once per rendered frame, right after the body's physics step.
// Returns true if the player JUST died from this landing.
//
// A FRAME function, not a tick function, and that is forced rather than chosen: on_ground is
// set by bodyMove() on the frame the move was stopped, and physics.c's resolveY() zeroes vy in
// the same branch, so the landing is only observable on the frame it happens and the impact
// speed is already gone by then. See survival.c for the whole argument and for why peak_y is
// tracked every frame rather than captured once.
bool fallDamageUpdate(FallTrack* ft, Survival* s, const Body* body, const World* w);

// How much hunger this item restores. 0 means "not food".
//
// A TABLE, not an if/else chain, and the reason is a dated one: v1.8.14 adds meat and v1.8.15
// adds cooked meat, and both must be a single row here and nothing else anywhere. A chain
// makes each addition an edit to control flow that a reviewer has to re-read; a row is a row.
uint8_t survivalFoodValue(BlockId id);

// Eats one unit out of `slot`. Returns true if a unit was consumed.
//
// Takes a SLOT rather than an ItemId on purpose: inventoryRemove() scans from slot 0 upward
// and would happily consume the apple the player is not pointing at, which is invisible in
// single player and infuriating with two stacks in the bag. The slot the UI acted on is the
// slot that is emptied.
//
// Refuses at full hunger — returns false, consumes nothing, changes nothing — so a misfire
// cannot silently burn food. Never heals: regen is the only path to health, and eating
// exists to feed it rather than to replace it.
bool survivalEat(Survival* s, Inventory* inv, uint8_t slot);

// ── Save / load ────────────────────────────────────────────────────────────────────────
//
// "<world_dir>/survival.dat", written with world/playerpose.c's protocol exactly: magic,
// version, CRC32 over the payload, payload; whole write to "<path>.tmp", flushed by fclose,
// the real path removed, then renamed over. The remove-before-rename is not superstition —
// Windows' rename() refuses an existing destination — and the window it opens is closed by a
// recovery pass on the next load that promotes an orphaned .tmp. Copied rather than re-derived
// because this file has the same access pattern inventory.c and playerpose.c have: a small
// fixed record rewritten on every quit.
//
// The payload is { uint8_t health; uint8_t hunger; } and NOTHING ELSE. The two countdowns are
// deliberately not persisted: they are sub-second phase inside a 60-second and a 4-second
// cycle, worth nothing to a player and worth two more bytes and a validation rule to this
// file. survivalLoad() resets both to survivalInit()'s values.

bool survivalSave(const Survival* s, const char* world_dir);

// False means "nothing usable on disk" — a missing file (the normal case for every world
// saved before v1.8.13), a bad magic, version, length or checksum, or a health/hunger byte
// outside 0..20. On false, *out is NOT written and the caller keeps its own defaults. That
// out-parameter discipline is half the contract: a caller that falls back to survivalInit()
// on false must not be reading a struct this function half-filled first.
bool survivalLoad(Survival* out, const char* world_dir);
