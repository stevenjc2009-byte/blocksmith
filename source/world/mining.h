// How long a block takes to break (roadmap task 50, part 1).
//
// One function with one job: given the block being broken and the item in the player's hand,
// say how many TICKS the break should take. Everything else about mining — the held-button
// timer, the crack overlay, the packet — is somebody else's file. This is the arithmetic
// seam, and it is deliberately the whole module.
//
// ── Why ticks, and not frames ───────────────────────────────────────────────────────────
//
// A break timer counted in FRAMES is a break timer whose duration depends on the GPU. This
// client's frame rate is 59.83 Hz when the renderer keeps up and whatever the renderer
// manages when it does not — a chunk streaming in, a big remesh, an Old 3DS where a New 3DS
// was measured — so "60 frames of holding A" is between one second and several depending on
// where the player happens to be looking. That is the exact defect world/tick.h was written
// to prevent, and its own header says so at length about fluid spread and mob AI: the fixed
// 20 TPS clock exists so that a mechanic quoted as a rate means one thing on every console,
// under every load. A break time is a mechanic. It is quoted in ticks.
//
// The unit is therefore ticks at TICK_HZ (20), which makes the stored hardness byte in
// world/registry.h's BlockDef directly comparable against a tick counter with no scaling and
// no division on the hot path. The ARM11 has no hardware divide instruction; this module's
// one division happens once, when a break STARTS, and never per tick.
//
// ── Why the tool table is empty ─────────────────────────────────────────────────────────
//
// There are no tools in this game. world/inventory.h states it outright — there is no item
// registry distinct from the block registry, and `ItemId` is a typedef of `BlockId`, so every
// id that can be in a hand is a block that was mined. So miningSpeedMultiplier() has exactly
// one populated case today: everything is bare hands, and bare hands are 1x.
//
// The parameter and the table exist anyway, because roadmap task 32 (v1.10.0) introduces
// tools and this is the shape it will need — a lookup that returns a multiplier, extended by
// adding rows, not by restructuring the caller. Nothing here guesses what those rows will
// say: no tiers, no pickaxe requirement, no per-block tool categories, no "wrong tool"
// penalty. Task 32 owns all of that. What is built once here is the call shape and the
// rounding rule, so that the day tools land, no call site moves.
#pragma once

#include <stdint.h>

#include "world/block.h"
#include "world/inventory.h"   // ItemId

// Speed multipliers are FIXED POINT, in units of 1/MINING_SPEED_ONE, and not floats.
//
// Fixed point because the ARM11 has no FPU worth using for this and because a break time is
// compared against an integer tick counter: a float multiplier would mean a conversion and a
// rounding decision at every comparison instead of one at the start. A power-of-two scale so
// the multiply is exact and the compiler can shift rather than multiply where it wants to.
//
// 256 gives task 32 a 1/256 step to express a tool with — Minecraft's own tool multipliers
// are 2, 4, 6, 8 and 12, none of which needs a fraction at all, so this is far finer than the
// content is likely to ask for.
#define MINING_SPEED_ONE  256u

// Divides and rounds UP. Exposed rather than kept static because it carries the rule that
// "a break never finishes early", and today's only multiplier is 1x — so this is the only
// way for a test to prove the rounding actually rounds. Returns 0 for num == 0; den == 0 is
// treated as 1 rather than trapping, because there is no sane answer and a crash in a break
// timer is worse than a slow break.
uint32_t miningCeilDiv(uint32_t num, uint32_t den);

// The speed multiplier for the item in hand, in 1/MINING_SPEED_ONE units. Today: always
// MINING_SPEED_ONE, for every id, because there are no tools. Task 32 fills this in.
uint32_t miningSpeedMultiplier(ItemId holding);

// How many 20 TPS ticks breaking `broken` takes while holding `holding`.
//
//   required = ceil(hardness(broken) / speed(holding))
//
// Zero if and only if the block has no hardness at all (air, water — neither is targetable,
// so neither is ever asked). Any block WITH hardness takes at least one tick however fast the
// tool: a break that completes in zero ticks is a break with no press, and the crack overlay
// would never draw a single frame of it.
uint32_t breakTicksRequired(BlockId broken, ItemId holding);
