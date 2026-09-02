#include "scene/interact.h"

#include "app/input_map.h"
#include "net/networld.h"
#include "world/block.h"
#include "world/inventory.h"
#include "world/light.h"
#include "world/mining.h"
#include "world/rng.h"

// ── the edit decision and the world write ───────────────────────────────────────────────
//
// No <3ds.h> and no <citro3d.h> above the __3DS__ guard further down, which is the whole
// point: tools/run_host_tests.sh links THIS file into interact_test, so the break/place
// ordering it checks is the ordering that ships. See the header for why an ordering bug is
// the one thing a diff cannot be read for.
#ifdef __3DS__
#include "scene/chunk_render.h"
#else
// scene/chunk_render.c is GPU code (<citro3d.h>) and cannot compile on the host, but
// interactEdit's *call* to it is unconditional in both builds and must stay that way — the
// absence belongs in the link, not in an #ifdef inside the function. tests/interact_stub.c
// supplies the symbol, exactly as tests/net_stub.c does for networldOnColumnLoad().
int chunkRenderTouch(const World* w, int x, int y, int z);
#endif

// ── where a local edit's relight goes (v1.8.7) ──────────────────────────────────────────
//
// NULL until source/main.c hands over the queue it drains, and NULL for every host case that
// does not ask for one. See interact.h for why this is a setter rather than a parameter on
// interactEdit, and why a NULL queue relighting inline is the honest default rather than a
// special case.
static RelightQueue* s_relightq;

void interactSetRelightQueue(RelightQueue* q)
{
	s_relightq = q;
}

// The edited column's relight, queued where there is a queue and run on the spot where there
// is not. This is source/main.c's onRemoteEdit shape verbatim (main.c:2957) — a remote edit
// and a local one produce exactly the same work, and until v1.8.7 only the remote one queued
// it. See interact.h for the cost, the measurement behind it, and why deferring is safe.
static void relightEdited(World* w, int cx, int cz)
{
	if (!lightEnabled())
		return;
	if (s_relightq == NULL || !relightqPush(s_relightq, cx, cz))
		lightRelightColumn(w, cx, cz);
}

// ── v1.8.7 F1: a local edit the server never heard about is undone, not kept ─────────────
//
// Tells the server about a write that has ALREADY been made locally, and says whether that
// write may stand. False means the caller must treat the edit as refused — the cell has been
// put back before returning, so there is nothing left for the caller to undo.
//
// WHY THE WRITE HAPPENS FIRST AND IS ROLLED BACK, rather than the send happening first: only
// worldSet() can say whether the edit is legal at all (it refuses y outside the world), and
// sending an edit the world would then have rejected would tell the server to do something
// this client did not do — the same divergence in the other direction. So the world decides,
// then the wire, and the rollback pays for the ordering. It is cheap precisely because this
// runs BEFORE worldMarkDirty, the relight and chunkRenderTouch: nothing has been marked, lit
// or queued yet, so an undone edit costs two world writes and not a 2.1 ms chunk rebuild.
//
// WHY THE SESSION CHECK IS NOT OPTIONAL. networldSendBlockEdit() is false in single player as
// well — there is no transport, so nothing can be sent, and nothing is out of step either.
// Reverting on every false would delete every edit in a single-player game. Only
// networldSessionActive() separates "the packet did not leave this console" from "there was
// never anywhere for it to go"; see net/networld.h, which was given that call for this.
//
// A false here also stops main.c sending the matching BS_INV_OP_PICKUP / BS_INV_OP_CONSUME,
// because the caller leaves broke_id/placed_id at BLOCK_AIR and those are the only channel
// main.c reads (main.c:4559, :4571). That is the point: the server takes those two ops on
// trust (deps/blocksmith-server/game/bsgame.c's handle_inv_action says so in as many words),
// so a bag that moved on a block edit that never arrived is a divergence the periodic
// BS_APP_INV_STATE snapshot silently repairs on the bag and never on the block.
static bool sendEditOrRevert(World* w, int x, int y, int z, BlockId written, BlockId before)
{
	if (networldSendBlockEdit(x, y, z, (uint8_t)written))
		return true;
	if (!networldSessionActive())
		return true;

	// A live session that did not get the packet. net/bsnet_transport.c's send_app_packet
	// is false only when nothing went out at all — no socket, the AEAD encrypt refused, or a
	// short/failed sendto — so there is no case where the server has the edit and this client
	// has thrown it away. Putting the cell back cannot itself fail: it is the same cell
	// worldSet just accepted, with the id that was in it.
	worldSet(w, x, y, z, before);
	return false;
}

// Does the player's box overlap the unit cube at (bx,by,bz)? This is a plain geometric
// test rather than a call into physics.c's bodyBlocked, because the block in question has
// not been written to the world yet — and it must not be, since writing it and then
// reverting would already have queued a remesh and charged the frame 2.1 ms a chunk for
// an edit that never happened.
static bool boxOverlapsCell(const Body* b, int bx, int by, int bz)
{
	const float hw = PLAYER_WIDTH * 0.5f;

	return (b->x - hw) < (float)(bx + 1) && (b->x + hw) > (float)bx &&
	        b->y       < (float)(by + 1) && (b->y + PLAYER_HEIGHT) > (float)by &&
	       (b->z - hw) < (float)(bz + 1) && (b->z + hw) > (float)bz;
}

// ── held-break progress (v1.8.1 task 50) ────────────────────────────────────────────────
//
// Forget whatever block was part-way broken. Called whenever the hold ends, the crosshair
// leaves the block, the block changes under the crosshair, or a break finishes — i.e. every
// exit from "a break is in progress", so there is one place that has to be right rather
// than five. Zeroing the coordinates as well as the flag is deliberate: `breaking` is the
// only field anything is allowed to read as "is there progress", and leaving stale
// coordinates behind invites a later reader to test the wrong one.
static void breakCancel(Interact* it)
{
	it->breaking    = false;
	it->break_x     = 0;
	it->break_y     = 0;
	it->break_z     = 0;
	it->break_id    = BLOCK_AIR;
	it->break_ticks = 0;
	it->break_need  = 0;
}

int interactBreakStage(const Interact* it)
{
	// break_need == 0 is a block with no hardness, which completes on the same call that
	// starts it and so is never left in progress. Guarding it here is still not optional:
	// it is also the divisor two lines down.
	if (!it->breaking || it->break_need == 0)
		return -1;

	uint32_t stage = (it->break_ticks * INTERACT_BREAK_STAGES) / it->break_need;

	// Clamp rather than trust the arithmetic. break_ticks can exceed break_need by up to
	// one frame's worth of catch-up ticks (tickClockAdvance hands out as many as four at
	// once) on the very frame the break completes, and this project has already paid for a
	// texture read that ran past its rows: a wrong texture constant still renders A
	// texture, so the bug shows up as bad art and never as an error.
	if (stage >= INTERACT_BREAK_STAGES)
		stage = INTERACT_BREAK_STAGES - 1;

	return (int)stage;
}

void interactInit(Interact* it)
{
	it->target    = (RayHit){0};
	it->holding   = BLOCK_STONE;
	it->broke     = 0;
	it->placed    = 0;
	it->refused   = 0;
	it->prev_keys = 0;
	it->broke_id  = BLOCK_AIR;
	it->placed_id = BLOCK_AIR;
	breakCancel(it);
}

// ── apples (v1.8.8: "apples dropping from leaves, pickable and functional") ─────────────
//
// registry.c's row for BLOCK_APPLE says apples "grow in oak and birch canopies", so only
// BLOCK_LEAVES and BLOCK_BIRCH_LEAVES roll for one; BLOCK_SPRUCE_LEAVES never does, on that
// same authority. Every other broken block keeps dropping itself exactly as before.
//
// The roll is POSITIONAL (world/rng.h's rngHash3), not a running RNG stream, for the same
// reason worldgen uses it and never a stream: a stream's output depends on how many times it
// has already been called, which for a break would mean "whether this apple falls depends on
// how many OTHER blocks this player broke first" — order-dependent, and rng.h's own header
// says a stream must never be used for exactly that reason. rngHash3(seed, x, y, z) is a pure
// function of the cell and nothing else, so the same cell always answers the same way.
//
// MULTIPLAYER SAFETY. This is a client-local pickup choice, not a world edit. The write this
// break sends over the wire is always "this cell becomes air" (sendEditOrRevert, below,
// unchanged by which item the break yields), so there is nothing here for two clients to
// disagree about in the TERRAIN. The bag side is already advisory: main.c reads it->broke_id
// once per break and sends BS_INV_OP_PICKUP, which the server takes on trust
// (deps/blocksmith-server/game/bsgame.c's handle_inv_action; see world/inventory.h's note on
// the periodic BS_APP_INV_STATE snapshot repairing the bag and never the block). A randomised
// choice of WHICH item that one pickup names introduces no divergence beyond what already
// exists for every ordinary pickup.
//
// APPLE_DROP_ONE_IN is not Minecraft's rate — that game rolls saplings, apples and sticks
// independently per leaf-decay tick, a mechanic this game does not have (no decay, only a
// direct break). 1-in-200 is chosen so an apple is a find, not a routine harvest: a canopy of
// thirty-odd leaf cells yields roughly one every few trees, not one a cube.
#define APPLE_DROP_ONE_IN  200u
#define APPLE_DROP_SALT    0xA9D1E0u

static bool blockIsAppleBearingLeaves(BlockId id)
{
	return id == BLOCK_LEAVES || id == BLOCK_BIRCH_LEAVES;
}

// True on roughly one cell in APPLE_DROP_ONE_IN, decided purely from the leaf's id and
// position — see the note above for why positional and not a stream. The salt only
// decorrelates this roll from any other rngHash3 use seeded off the same coordinates
// (worldgen's own decoration hashes among them); it carries no other meaning.
static bool appleDropRoll(BlockId leaf_id, int x, int y, int z)
{
	const uint32_t h = rngHash3(APPLE_DROP_SALT ^ ((uint32_t)leaf_id * 0x1000193u), x, y, z);
	return h < (0xFFFFFFFFu / APPLE_DROP_ONE_IN);
}

// The world write, once the hold has earned it. Everything here is the pre-v1.8.1 break
// path verbatim; the only change is that it is now reached by running out of ticks instead
// of by a button edge.
static int breakComplete(Interact* it, World* w, int x, int y, int z, BlockId broken)
{
	// v1.7.1 task 47. A plant breaks and gives you nothing; it is not refused. See
	// world/block.h's blockDropsNothing for why this had been tangled up with the bag
	// ceiling below, and what that cost.
	const bool no_drop = blockDropsNothing(broken);
	int queued = 0;

	// worldSet refuses y outside the world, which is exactly what should happen when the
	// ray hit the solid floor below y == 0 — that is not a block anyone owns.
	if (worldSet(w, x, y, z, BLOCK_AIR)) {
		// v1.8.7 F1. The server is told FIRST, and the answer is read. Until now this was a
		// fire-and-forget call whose bool went in the bin, three lines further down and after
		// the column had already been marked dirty — so a refused send left the block gone
		// and SAVED here while the server still had it, and main.c then moved the bag on top
		// of that. See sendEditOrRevert for the whole shape of it.
		if (!sendEditOrRevert(w, x, y, z, BLOCK_AIR, broken)) {
			// Counted as a refusal, which is this module's existing word for "the press did
			// nothing" (a miss, an occupied cell, an empty hand, an uncarryable block) and is
			// already on the debug overlay as `r`. broke_id stays BLOCK_AIR, so main.c offers
			// nothing to the bag and sends no BS_INV_OP_PICKUP.
			it->refused++;
			breakCancel(it);
			return queued;
		}
		// Step 8.1. Only edits mark a column for saving — see the note on Column.dirty.
		worldMarkDirty(w, x, z);
		// Adaptive lighting: the edited column's channels are refreshed before the remesh is
		// queued, so the drained mesh bakes the new light in the same frame. v1.8.7 makes
		// that a QUEUE push rather than a full 32768-cell recompute charged to this frame —
		// see relightEdited, and interact.h for why the main loop's drain still lands ahead
		// of the remesh. The relight itself is unchanged: column-local and idempotent, a
		// full reflood of that one column, with no neighbour able to go stale behind it.
		relightEdited(w, x >> 4, z >> 4);
		queued += chunkRenderTouch(w, x, y, z);
		it->broke++;
		// Left as BLOCK_AIR for a plant, which is exactly how main.c already spells "this
		// break earned nothing" — it adds to the bag only when broke_id != BLOCK_AIR. So the
		// plant is removed from the world, the edit goes to the server as an ordinary air
		// write (id 0, legal on the wire and below every ceiling on both sides), and no
		// unholdable id is ever offered to the inventory.
		// An apple-bearing leaf substitutes BLOCK_APPLE for itself on the roll — see the
		// apple block above for the whole argument. Every other block, leaves included on a
		// miss, drops itself exactly as it always has.
		BlockId drop = broken;
		if (!no_drop && blockIsAppleBearingLeaves(broken) && appleDropRoll(broken, x, y, z))
			drop = BLOCK_APPLE;
		it->broke_id = no_drop ? BLOCK_AIR : drop;
	} else {
		it->refused++;
	}

	// Whether it landed or the world vetoed it, this hold is spent. A veto that left the
	// progress standing would re-fire the same doomed worldSet on every following frame of
	// the hold and count a refusal each time; restarting the timer instead costs the player
	// nothing they can feel and bounds the noise to one refusal per full break time.
	breakCancel(it);
	return queued;
}

// One frame of holding the break button with something under the crosshair. `pressed` is
// true only on the frame the hold began, and exists so a refusal is counted once per press
// rather than sixty times a second.
static int breakProgress(Interact* it, World* w, const RayHit* t, int ticks, bool pressed)
{
	const BlockId here = worldGet(w, t->x, t->y, t->z);

	// Start, or restart. The block id is part of the identity, not just the coordinates:
	// a server edit can swap what is in the cell mid-hold, and a stone that is 90% mined
	// must not finish instantly as the dirt that replaced it.
	if (!it->breaking ||
	    it->break_x != t->x || it->break_y != t->y || it->break_z != t->z ||
	    it->break_id != here) {

		// A block the bag cannot hold is not breakable — and the question is asked BEFORE
		// anything is mutated, which is the whole of the v1.6.0 fix. Until then the order
		// was the other way round: worldSet wrote air, the id went to invBridgeAdd, and
		// world/inventory.h refused it because a server-registered dynamic id (0x80..0xFD)
		// is over the BLOCK_COUNT ceiling every slot-indexed array in this client is built
		// to. The block was then in neither place — deleted from the world, absent from the
		// bag, with no message. An unbreakable block is at least legible to the player; a
		// vanishing one reads as save corruption.
		//
		// v1.8.1 moves the question earlier still, from the moment of the break to the
		// moment the hold starts. That is not just tidiness: with a timed break, asking at
		// the end would have the player watch a full crack animation play out on a block
		// that was never going to go, which reads as the game being broken rather than as
		// the block being unbreakable.
		//
		// The rule lives in inventoryCanHold() and nowhere else. Guarding here rather than
		// at the pickup also covers the multiplayer path for free: this function is the ONLY
		// origin of a player-initiated break in the client, so refusing before the write
		// also refuses the BS_APP_BLOCK_EDIT — which matters, because the server would have
		// honoured the removal and then dropped the matching BS_INV_OP_PICKUP
		// (deps/blocksmith-server/game/bsgame.c mirrors the same ceiling), destroying the
		// block for every player on the server, not just this one.
		//
		// v1.8.8 made inventoryCanHold() registry-aware, which is the change the ⚠ that used
		// to sit here said would come. The branch is KEPT rather than deleted, and the reason
		// is worth writing down because deleting it is the tempting move:
		//
		// inventoryCanHold() is now "defined, not air, not a liquid" and blockIsTargetable()
		// is "drawn, not a liquid" — the SAME SET. Said plainly rather than softened: through
		// world/raycast.c, which stops the ray on blockIsTargetable() and nothing else, no hit
		// can hand this function an id the bag refuses. In ordinary play this branch is now
		// unreachable, and that is checked rather than assumed — scene/interact_test.c's
		// testTheBreakGuardStillRefusesTheUncarryable() walks the whole core id space and
		// asserts the two predicates give the identical answer for every id in it.
		//
		// It is kept anyway, and not out of timidity. "No ray can produce it" is not "it
		// cannot happen": an id whose row never arrived — a registry sync that failed, or a
		// server ahead of this client — sits in the world as a raw byte and reaches here the
		// moment anything aims at it. And this is the seam where "can I aim at it" and "may
		// the bag hold it" separate again, which world/inventory.h says is scheduled: the
		// first ItemId that is not a BlockId (a tool, or the apple v1.8.8 is being built for)
		// is carryable and not targetable, and the first block that is targetable and not
		// carryable (a bucket away from a liquid, an unmineable bedrock) puts a live case
		// straight back through here. Deleting it means rediscovering the v1.6.0 defect above
		// the day one of those lands.
		//
		// That same test also drives an undefined id through this branch and asserts it still
		// refuses, so it cannot rot into code nothing has executed.
		if (!blockDropsNothing(here) && !inventoryCanHold(here)) {
			breakCancel(it);
			// `refused` is the module's existing idiom for "the press did nothing", already
			// counted for a miss, an occupied cell and an empty hand, and already surfaced
			// on the debug overlay as `r`. Nothing louder is invented here: this client has
			// no toast or status line for a refused action (world/inventory.h says so in as
			// many words), and a silent refusal is a far smaller lie than a silent deletion.
			if (pressed)
				it->refused++;
			return 0;
		}

		it->breaking    = true;
		it->break_x     = t->x;
		it->break_y     = t->y;
		it->break_z     = t->z;
		it->break_id    = here;
		it->break_ticks = 0;
		it->break_need  = breakTicksRequired(here, it->holding);
	}

	it->break_ticks += (uint32_t)ticks;

	// >= rather than ==: the clock hands out up to TICK_MAX_CATCHUP_DEFAULT ticks in one
	// go, so the exact requirement can be stepped straight over.
	if (it->break_ticks < it->break_need)
		return 0;

	return breakComplete(it, w, t->x, t->y, t->z, it->break_id);
}

int interactEdit(Interact* it, World* w, const Body* body,
                 u32 keys_down, u32 keys_held, int ticks)
{
	// Internal edge detection: act only on bits newly set since the last call, no matter
	// what the caller passes. With hidKeysDown() (already a one-frame pulse) this is a
	// no-op — prev_keys cannot already hold a bit that keys_down is just now raising, so
	// fresh == keys_down on the one frame the press happens. With hidKeysHeld()
	// (level-triggered, high for the whole hold) this is what makes it safe to pass:
	// the first frame of a hold still has the bit clear in prev_keys and fires, but every
	// later frame of the same hold has it already set, so the bit drops out of fresh and
	// the edit now happens exactly once per press instead of once per frame held.
	const u32 fresh = keys_down & ~it->prev_keys;
	it->prev_keys = keys_down;

	// Step 8.2. Cleared every call, not accumulated: these two report what THIS call did, so
	// the caller can act on it once. A cumulative counter would leave the caller diffing two
	// integers to work out whether anything happened, and a stale id left over from an
	// earlier frame would put a second copy of the same block into the inventory every frame
	// until the next edit.
	it->broke_id  = BLOCK_AIR;
	it->placed_id = BLOCK_AIR;

	// Step 8.4. The two verbs come from the player's bindings rather than the INTERACT_KEY_*
	// constants directly. app/input_map.c answers with exactly those constants' values until
	// an options.ini says otherwise.
	const u32 key_break = inputKey(ACTION_BREAK);
	const u32 key_place = inputKey(ACTION_PLACE);

	// The one asymmetry in this function, and it is the point of task 50: place reads the
	// edge computed above, break reads the raw level. A break is a process now, so it needs
	// to know the button is STILL down, which an edge cannot say.
	const bool holding_break = (keys_held & key_break) != 0;

	// A clock that appears to run backwards must never rewind a break, for the same reason
	// tickClockAdvance clamps negative elapsed time to zero.
	if (ticks < 0)
		ticks = 0;

	if (!it->target.hit) {
		// Aiming at nothing abandons the break in progress. Progress is anchored to a
		// specific block and there is no longer one.
		breakCancel(it);
		// A press with nothing to aim at is still a refusal, not a no-op: `refused` is
		// documented (interact.h) as counting edits the world, the body, or a miss
		// rejected, and a press that found no target was rejected same as one the world
		// vetoed. Leaving this uncounted made the overlay's `r` figure understate how
		// many presses actually did nothing.
		if (fresh & (key_break | key_place))
			it->refused++;
		return 0;
	}

	const RayHit* t = &it->target;
	int queued = 0;

	if (!holding_break) {
		// Released, or never held. Progress does not survive letting go — a break the player
		// walked away from and came back to must start again, which is both what the genre
		// does and the only rule that does not need a decay timer nobody asked for.
		breakCancel(it);
	} else {
		queued += breakProgress(it, w, t, ticks, (fresh & key_break) != 0);
	}

	if (fresh & key_place) {
		// Nothing in hand is a refusal, not a crash and not a silent no-op. Before step 8.2
		// `holding` was a constant BLOCK_STONE and this could not happen; now it comes from
		// the inventory's selected hotbar slot, and an empty slot is the ordinary case.
		// Placing BLOCK_AIR would otherwise read as a second, longer-ranged break.
		if (it->holding == BLOCK_AIR) {
			it->refused++;
			return queued;
		}

		// No entry face means the camera is inside a solid block, so there is no sensible
		// cell to place into — the ray never crossed a boundary to name one.
		if (t->face == RAY_FACE_NONE) {
			it->refused++;
			return queued;
		}

		// Do not overwrite something solid. The place cell is the cell the ray was in
		// before the hit, so it is air in the ordinary case, but a caller could aim at a
		// block through a gap narrower than a block and land here.
		//
		// Kept rather than re-read, because v1.8.7's rollback below has to put back what was
		// really there and "air" is only the ordinary case: a non-solid cell can hold tall
		// grass, and reverting that to BLOCK_AIR would delete the plant on a failed send.
		const BlockId place_was = worldGet(w, t->px, t->py, t->pz);
		if (blockIsSolid(place_was)) {
			it->refused++;
			return queued;
		}

		// Refuse to entomb the player. `body` is required (see interact.h) so this check
		// can no longer be silently skipped by a caller passing NULL.
		if (boxOverlapsCell(body, t->px, t->py, t->pz)) {
			it->refused++;
			return queued;
		}

		if (worldSet(w, t->px, t->py, t->pz, it->holding)) {
			// v1.8.7 F1, the break path's twin — same reasoning, same order, and `place_was`
			// is what the cell goes back to if the packet did not leave. placed_id stays
			// BLOCK_AIR on the failure, so main.c charges the hotbar nothing and sends no
			// BS_INV_OP_CONSUME.
			if (!sendEditOrRevert(w, t->px, t->py, t->pz, it->holding, place_was)) {
				it->refused++;
				return queued;
			}
			worldMarkDirty(w, t->px, t->pz);
			// Same relight-before-remesh ordering as the break path above, and since v1.8.7
			// the same queue-with-inline-fallback.
			relightEdited(w, t->px >> 4, t->pz >> 4);
			queued += chunkRenderTouch(w, t->px, t->py, t->pz);
			it->placed++;
			it->placed_id = it->holding;
		} else {
			it->refused++;
		}
	}

	return queued;
}

// ── aiming — console build only ─────────────────────────────────────────────────────────
//
// Below the guard because it takes a Camera, and scene/camera.h includes <citro3d.h>.
// Nothing about the raycast itself is console-specific (world/raycast.c is host-tested in
// its own right); it is the camera type that cannot cross.
#ifdef __3DS__

#include <math.h>

void interactAim(Interact* it, const World* w, const Camera* cam)
{
	// The same forward vector cameraUpdate walks along, and it has to stay the same one:
	// if the ray and the movement disagree about which way "forward" is, the highlight
	// sits somewhere off to the side of the crosshair and no amount of staring at
	// raycast.c explains why. The view matrix is Rx(pitch) * Ry(yaw) * T(-pos), so
	// positive pitch looks down and -Z is forward at zero yaw.
	const float cp = cosf(cam->pitch);
	const float fx = sinf(cam->yaw) * cp;
	const float fy = -sinf(cam->pitch);
	const float fz = -cosf(cam->yaw) * cp;

	it->target = worldRaycast(w, cam->x, cam->y, cam->z, fx, fy, fz, INTERACT_REACH);
}

#endif  // __3DS__
