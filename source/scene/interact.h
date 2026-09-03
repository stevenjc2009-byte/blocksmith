// Breaking and placing blocks: the bridge between where the camera is pointing and what
// the world and the mesh pool are told about it.
//
// This lives in scene/ rather than world/ because it is the one piece of the edit path
// that needs the camera and the mesh pool. The geometry it rests on (world/raycast.c) and
// the storage it writes to (world/world.c) are both host-tested on the PC; what is left
// here is button wiring and the ordering of "write the block, then tell the renderer",
// which is small on purpose.
//
// ── the __3DS__ split ───────────────────────────────────────────────────────────────────
//
// That ordering is exactly the thing that went wrong (v1.6.0: a break wrote air FIRST and
// only then asked whether the block could be carried, so a server-registered block was
// deleted from the world and refused by the bag — gone, with no message). An ordering bug
// is not provable by reading a diff, so the pure half of this module — the Interact state,
// interactInit and interactEdit — now sits above an `#ifdef __3DS__` guard and the host
// suite links the REAL scene/interact.c. The aiming half needs the camera (and so
// <citro3d.h>) and stays below the guard, along with the libctru key constants.
//
// Same arrangement as app/battery.c and app/debugmenu_ui.c: the directory says where the
// file belongs in the program, the guard says which half the host can actually prove.
#pragma once

#ifdef __3DS__
#include <3ds.h>
#else
// libctru's u32, and only that. The host build has no libctru, but interactEdit's key mask
// is part of the pure half and its type has to stay byte-identical to the console's.
#include <stdint.h>
typedef uint32_t u32;
#endif

// Named outright rather than leaned on: <3ds.h> drags both in on the console, but the
// break-progress fields below are bool and uint32_t in BOTH builds and must not depend on
// which branch of the guard above ran.
#include <stdbool.h>
#include <stdint.h>

#include "world/physics.h"
#include "world/raycast.h"
#include "world/relightq.h"
#include "world/world.h"

// How far the player can reach, in blocks. Five is the distance the genre trained people
// on and it is also about as far as the 400x240 top screen can show a highlight cage that
// still reads as sitting on one particular block rather than somewhere over there.
#define INTERACT_REACH  5.0f

// How many (item, count) pairs a broken stateful block can report back through
// broke_extra_item/qty below. 3 because a furnace — the only stateful block that exists
// today — never carries more than input + fuel + output at once. Raise this if a future
// stateful block (a chest) ever needs more slots than that.
#define INTERACT_BROKE_EXTRA_MAX 3

// v1.8.16 F1. Reads back what a just-broken STATEFUL block (`id`, the block that broke, at
// `x,y,z`) was carrying, filling up to INTERACT_BROKE_EXTRA_MAX (item, count) pairs into
// `items_out`/`counts_out` and returning how many were filled (0..INTERACT_BROKE_EXTRA_MAX).
// Called by breakComplete() on the success path, BEFORE the caller's own side-table cleanup
// (main.c's blockStateRemove(), driven off broke_valid/broke_x/y/z below) discards the
// record — so this is the one chance anything has to read it.
//
// A CALLBACK rather than a concrete BlockStateTable* (contrast interactSetRelightQueue just
// below, which takes a real RelightQueue* because world/relightq.c is already in every
// consumer's link line — tools/run_host_tests.sh's interact stanzas prove it). The table
// this needs lives in world/blockstate.c and the struct it unpacks lives in
// world/furnace.c, and neither is linked into scene/interact.c today; a concrete-pointer
// parameter would force both (and world/crc32.c, which blockstate.c needs) into every one of
// this file's host-test stanzas just to satisfy a type this module otherwise never touches.
// The callback keeps interact.c's link surface exactly what it was — a test can register a
// spy that never touches blockstate.c or furnace.c at all (see scene/interact_test.c) — and
// the real body (blockStateGet() + furnaceStateUnpack()) belongs in main.c, which already
// links both and already contains that exact unpack shape for the furnace panel.
//
// A reader that hands back qty 0 for a slot, or a count outside 0..INTERACT_BROKE_EXTRA_MAX,
// is a caller bug: breakComplete() drops a zero-qty entry and clamps an out-of-range count to
// 0 rather than trust either, so a misbehaving reader cannot corrupt Interact's own fields.
typedef int (*InteractBrokeContentsFn)(BlockId id, int x, int y, int z,
                                        BlockId items_out[INTERACT_BROKE_EXTRA_MAX],
                                        uint8_t counts_out[INTERACT_BROKE_EXTRA_MAX]);

// Registers the reader above. NULL (the default, same posture as interactSetRelightQueue's
// NULL) means a broken stateful block's contents are never recovered — the pre-v1.8.16
// behaviour — which is what every host-test case gets unless it opts in, and what the
// console gets until main.c is given the one new call this fix needs (see furnace.c's
// v1.8.16 notes for the exact wiring: a reader function implemented in main.c, registered
// once at startup next to the existing interactSetRelightQueue() call).
void interactSetBrokeContentsFn(InteractBrokeContentsFn fn);

typedef struct {
	RayHit  target;      // what the camera is looking at, recomputed every frame
	BlockId holding;      // what a place would put down
	int     broke;        // cumulative, for the overlay — a placed block that silently
	int     placed;        // did not appear is otherwise very hard to notice
	int     refused;      // edits the world, the player's own body, or a miss rejected
	u32     prev_keys;    // keys_down seen on the previous interactEdit call, so a held
	                       // button can only fire once per press regardless of what the
	                       // caller passes in — see interactEdit

	// What THIS interactEdit call broke and placed, or BLOCK_AIR for neither. Step 8.2's
	// inventory is filled from the first and charged for the second, and it needs the block
	// *id*, which the cumulative counters above cannot carry. Both are cleared at the top of
	// every interactEdit — they report one call, they do not accumulate — so a caller that
	// reads them once per frame after the call sees each edit exactly once. Only one edit of
	// each kind can happen per call, because the press that triggers it is edge-detected.
	BlockId broke_id;
	BlockId placed_id;

	// WHERE the break landed, added v1.8.15 for the furnace. broke_id answers "what did the
	// player earn" and is deliberately BLOCK_AIR for a plant, which breaks and drops nothing;
	// these answer the different question "which cell stopped containing a block", which a
	// block carrying side-table state has to know so it can drop that state when it is mined.
	//
	// They exist as separate fields because the break coordinates that were already here —
	// break_x/y/z above — are gone by the time the caller can read them. breakComplete() sets
	// broke_id and then calls breakCancel(), which zeroes break_x/y/z as part of retiring the
	// hold, so main.c currently learns WHAT broke but never WHERE. Reusing break_x/y/z would
	// mean not clearing them, and they mean "the block this in-progress hold belongs to" —
	// leaving a finished hold's coordinates standing there would make `breaking == false` with
	// live coordinates a state the rest of this file does not expect.
	//
	// broke_valid rather than a sentinel coordinate: (0,0,0) is an ordinary cell a player can
	// stand in and mine, so there is no coordinate triple free to mean "nothing happened".
	// Set on ANY landed break, including one that drops nothing, because state cleanup has to
	// happen whether or not the player got an item for it.
	bool    broke_valid;
	int     broke_x, broke_y, broke_z;

	// And WHERE a placement landed, the exact twin of the three fields above and added in the
	// same version for the other half of the same job: a furnace that is mined must drop its
	// state, and a furnace that is PLACED must acquire one, so main.c needs the cell in both
	// directions.
	//
	// The asymmetry with the break side is worth stating, because it looks like an oversight
	// and is not: the break path needed new fields because breakCancel() had already wiped the
	// coordinates by the time the caller ran, whereas the place path simply never recorded
	// them at all -- placed_id was the only thing anyone had asked for. Same remedy either way.
	//
	// placed_valid follows broke_valid's reasoning exactly. (0,0,0) is a placeable cell like
	// any other, so no coordinate triple is free to mean "nothing happened", and testing
	// placed_id != BLOCK_AIR instead would tie "did a placement land" to "was it a real block",
	// which are the same question today only by accident.
	bool    placed_valid;
	int     placed_x, placed_y, placed_z;

	// ── v1.8.16 F1: a broken stateful block's own contents ──────────────────────────────
	//
	// FIX for the defect where breaking a furnace mid-smelt silently destroyed whatever sat
	// in its input, fuel and output slots: main.c's break-cleanup already called
	// blockStateRemove() on broke_x/y/z (the P3 leak-prevention defence
	// docs/plan-1.8.15-furnace.md's own risk table asked for) WITHOUT ever reading the
	// record first, so main.c never had a chance to bank what was inside it.
	//
	// Filled by breakComplete(), on the same success path and same schedule as broke_id/
	// broke_valid above, by calling whatever reader interactSetBrokeContentsFn() was given —
	// see that setter for why this is a callback and not a concrete BlockStateTable*. 0
	// entries is the ordinary case: every block that carries no side-table state (which is
	// everything except the furnace today) and any break for which no reader is registered.
	int     broke_extra_count;
	BlockId broke_extra_item[INTERACT_BROKE_EXTRA_MAX];
	uint8_t broke_extra_qty[INTERACT_BROKE_EXTRA_MAX];

	// ── v1.8.1 task 50: a break takes time ──────────────────────────────────────────────
	//
	// Progress against ONE block, measured in simulation ticks rather than frames. Frames
	// were never an option: world/tick.h exists precisely because the Old 3DS renders at
	// whatever rate the GPU manages, so a frame-counted timer would make stone quicker to
	// mine on an emptier screen. The caller hands interactEdit the tick count the shared
	// 20 TPS clock produced this frame and the arithmetic below is the same on every
	// machine.
	//
	// break_x/y/z name the block the progress belongs to, and break_id what was there when
	// it started. Either changing — the player looked away, or the server replaced the
	// block under the crosshair — abandons the progress and starts again, because a
	// half-mined stone must not finish as a half-mined dirt.
	bool     breaking;      // is a hold in progress at all
	int      break_x, break_y, break_z;
	BlockId  break_id;      // what was in that cell when the hold began
	uint32_t break_ticks;   // ticks banked so far
	uint32_t break_need;    // ticks required, from breakTicksRequired, cached at the start
} Interact;

// How many crack pictures the overlay has. The progress bar is quantised to this many
// steps, so it is the count of stages the artwork must supply and nothing else depends on
// the number. Eight is what the genre trained people on and it is also the point past
// which a 16x16 tile has no pixels left to say anything new with.
#define INTERACT_BREAK_STAGES  8

void interactInit(Interact* it);

// Where a local break or place should QUEUE its column relight (v1.8.7). Call once at start
// up with the same RelightQueue the main loop drains; pass NULL to go back to relighting
// inline.
//
// Why this exists at all: lightRelightColumn() recomputes a whole 32768-cell column, and
// until v1.8.7 a local edit paid for that synchronously on the frame the player pressed the
// button — med 0.156 ms, p95 0.227 ms, max 0.287 ms on the host at -O1 over 240 samples
// (world/relight_drain.h), and dearer by an unmeasured factor on a 268 MHz ARM11 with no L2.
// Every OTHER caller in the tree already queued: source/main.c's onRemoteEdit does exactly
// `if (lightEnabled() && !relightqPush(...)) lightRelightColumn(...)`, and this makes the
// local edit path the same shape rather than the last exception to it.
//
// Nothing goes stale by deferring it. main.c calls interactEdit(), then relightDrain(), then
// chunkRenderDrainDirty(), in that order in the SAME frame — so a relight queued here is
// still drained ahead of the remesh that bakes it, and there is no frame on which a broken
// block can be drawn with the light it had before it broke.
//
// A setter rather than a parameter on interactEdit(): the queue is one process-wide object
// with one owner, exactly like the chunkRenderTouch() pool this module already calls into,
// and threading it through a signature that thirty host-test call sites already use would
// change all of them to say the same thing. NULL is the default and is not a special case —
// it relights inline, which is what a full queue already does (world/relightq.h calls that
// posture "slow, never wrong").
void interactSetRelightQueue(RelightQueue* q);

// Which crack picture to draw over Interact.target right now, 0..INTERACT_BREAK_STAGES-1,
// or -1 when nothing is being broken and the overlay should draw nothing at all.
//
// Derived rather than stored so there is exactly one definition of "how far along is this
// break", and so a caller cannot render a stage that disagrees with the progress that will
// actually complete the break.
int interactBreakStage(const Interact* it);

// Applies this frame's input to the world: INTERACT_KEY_BREAK held long enough sets the
// targeted block to air, INTERACT_KEY_PLACE pressed puts `holding` in the empty cell the
// ray entered from.
// Returns the number of chunks newly queued for a remesh, so a caller can see that an
// edit reached the renderer at all.
//
// `body` is the player, and a placement that would put a block inside it is refused
// rather than performed: standing in a wall is worse than a press that did nothing.
// `body` is required — passing NULL would silently disable that check, so there is no
// bypass for it here; every caller must supply the real player body.
//
// `keys_down` may be either hidKeysDown() or hidKeysHeld(): interactEdit tracks which
// bits were already set on the previous call internally (see Interact.prev_keys) and
// acts only on newly-set bits, so a held button still fires once per press either way.
// That is the PLACE path, and it is unchanged — a placement is a single instantaneous act
// and one press must put down exactly one block.
//
// `keys_held` is hidKeysHeld() and drives BREAK, which since v1.8.1 is not an act but a
// process: the button has to stay down for breakTicksRequired() ticks before the block
// goes. Break therefore reads a LEVEL and place reads an EDGE, and the two masks are
// separate parameters rather than one because collapsing them would force one of the two
// verbs to be wrong. Passing the same mask for both is legal and is what the host tests do
// for the place cases; it just means a break can never make progress, since a level that
// is only ever high for one call banks one tick and then releases.
//
// `ticks` is how many 20 TPS simulation ticks elapsed this frame — the return of
// tickClockAdvance, not a frame count. Zero is the ordinary case (the clock produces a
// tick roughly every third frame at 60 fps) and is not a no-op: a hold still has to be
// registered, and a completed break still has to be able to happen on a zero-tick frame if
// the requirement was already met.
int interactEdit(Interact* it, World* w, const Body* body,
                 u32 keys_down, u32 keys_held, int ticks);

// ── aiming and the button constants — console build only ────────────────────────────────
#ifdef __3DS__

#include "scene/camera.h"

// Recomputes `target` from the camera's position and facing. Call once per frame, before
// drawing, so the highlight and any edit in the same frame agree on what was aimed at.
void interactAim(Interact* it, const World* w, const Camera* cam);

// The buttons, in one place so the handoff and the code cannot disagree. The camera
// already owns A (boost), the D-pad (move), L and R (up/down) and START (exit), which
// leaves the two right-hand face buttons free.
#define INTERACT_KEY_BREAK  KEY_X
#define INTERACT_KEY_PLACE  KEY_Y

#endif  // __3DS__
