// Health, hunger and fall damage. See world/survival.h for the contract and the constants.
//
// The save half is world/playerpose.c's protocol with a different payload, and it is a
// deliberate copy rather than a shared helper: playerpose.c is itself a copy of
// world/inventory.c's, and each of the three states in its own file why the tmp+rename dance
// is right for ITS access pattern. Factoring the three into one writer would move that
// reasoning away from the format it is about, and the next save file added would inherit the
// mechanism without the argument. Three small copies of eleven lines each is the cheaper
// mistake.

#include "world/survival.h"

#include "world/crc32.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SURVIVAL_FILE_NAME "survival.dat"

// "BSV1" — Blocksmith surVival, format 1 — read as bytes little-endian. A different letter
// from world/inventory.c's "BSI1" (0x31495342), world/playerpose.c's "BSP1" (0x31505342) and
// world/region.c's "BSR1" (0x31525342), for the reason inventory.c gives about its own: these
// files sit in the same directory and a crossed path must never read as a valid record of the
// wrong kind. Checked against the tree before it was picked — no other magic uses 'V'.
#define SURVIVAL_MAGIC   0x31565342u
#define SURVIVAL_VERSION 1u

#define SURV_HDR_BYTES   12
#define SURV_PAYLOAD     2                                  // health, hunger
#define SURV_FILE_BYTES  (SURV_HDR_BYTES + SURV_PAYLOAD)    // 14

// ── the clocks ─────────────────────────────────────────────────────────────────────────

void survivalInit(Survival* s)
{
	if (!s) return;
	s->health       = SURVIVAL_MAX_HEALTH;
	s->hunger       = SURVIVAL_MAX_HUNGER;
	s->hunger_ticks = SURVIVAL_HUNGER_PERIOD;
	s->regen_ticks  = SURVIVAL_REGEN_PERIOD;
}

void fallTrackInit(FallTrack* ft)
{
	if (!ft) return;
	ft->falling = false;
	ft->peak_y  = 0.0f;
}

// One step of a countdown that must survive arriving at zero.
//
// Written as "decrement if non-zero, then test" rather than the obvious `if (--t == 0)`
// because both counters are uint16_t and both live in a struct a caller can legally have
// memset to zero (net/ hands out zeroed player records, and a Survival that skipped
// survivalInit is exactly the shape of bug that never shows up in a happy path). `--t` on a
// zero uint16_t is 65535, which would silently mute hunger for 55 minutes instead of firing.
// This form fires on the next tick instead, which is wrong by at most one tick and is
// self-correcting because the reset below re-arms the full period.
//
// The timing for an initialised struct is unchanged and is what survival_test.c pins: from
// SURVIVAL_HUNGER_PERIOD, the counter reaches 0 on the 1200th call, not the 1199th or 1201st.
static bool countdown(uint16_t* t, uint16_t period)
{
	if (*t > 0) (*t)--;
	if (*t != 0) return false;
	*t = period;
	return true;
}

bool survivalTick(Survival* s)
{
	if (!s) return false;

	const uint8_t before = s->health;

	// Hunger first, so a drain that empties the bar starts starvation on this same tick
	// rather than a tick later. The ordering is not observable at 20 TPS by a player, but it
	// is observable by a test, and an unstated order is one a later edit can flip for free.
	if (countdown(&s->hunger_ticks, SURVIVAL_HUNGER_PERIOD)) {
		if (s->hunger > 0) s->hunger--;
	}

	if (s->hunger >= SURVIVAL_REGEN_HUNGER) {
		if (countdown(&s->regen_ticks, SURVIVAL_REGEN_PERIOD)) {
			if (s->health < SURVIVAL_MAX_HEALTH) s->health++;
		}
	} else if (s->hunger == 0) {
		if (countdown(&s->regen_ticks, SURVIVAL_REGEN_PERIOD)) {
			// THE FLOOR. Starvation stops at 1 and can never take the last point. See
			// survival.h for why this differs from fall damage, which can and does kill.
			if (s->health > 1) s->health--;
		}
	} else {
		// The band between them — hunger 1..17 — is neither, and the counter is re-armed
		// rather than left where it stopped. Leaving it would mean a player who ate back up
		// to 18 with three ticks left on a stale countdown got a health point almost
		// instantly, and then waited a full 4 s for the next: a visible stutter in the bar
		// with no cause a player could see.
		s->regen_ticks = SURVIVAL_REGEN_PERIOD;
	}

	// Honest, not hardcoded — see the note on survivalTick in survival.h. `before != 0` is
	// what stops an already-dead player being reported as freshly dead on every subsequent
	// tick, which would re-fire whatever death screen the caller hangs off this.
	return before != 0 && s->health == 0;
}

// ── fall damage ────────────────────────────────────────────────────────────────────────

bool fallDamageUpdate(FallTrack* ft, Survival* s, const Body* body, const World* w)
{
	if (!ft || !s || !body || !w) return false;

	if (!body->on_ground) {
		if (!ft->falling) {
			ft->falling = true;
			ft->peak_y  = body->y;
		}

		// EVERY frame, not once at the start of the fall. A jump leaves the ground and THEN
		// rises: on_ground goes false at the bottom of the arc, so a peak captured at that
		// moment is the take-off height and the whole jump — about 1.3 blocks at
		// PLAYER_JUMP_SPEED — is missing from the measurement. Jumping into a pit would then
		// be scored as a shorter fall than walking into it, which is backwards. fmaxf, so a
		// body that is rising raises the peak and a body that is falling leaves it alone.
		ft->peak_y = fmaxf(ft->peak_y, body->y);
		return false;
	}

	// On the ground. If we were not falling there is nothing to score — this is the ordinary
	// walking frame and it must be free.
	if (!ft->falling) return false;

	ft->falling = false;

	// Water first, and it is a full negation rather than a reduction: a landing anywhere in
	// water costs nothing. bodySubmerged() is physics.h's own predicate (BODY_DRY or not), so
	// this asks the same question the swim code asks and cannot drift from it.
	if (bodySubmerged(w, body)) return false;

	// The impact speed is NOT available here and cannot be made available: physics.c's
	// resolveY() sets b->vy = 0.0f in the same branch that sets b->on_ground = true, so by the
	// time a landing is observable the velocity that caused it is gone. Distance fallen is the
	// only thing left, which is why FallTrack exists at all.
	const float drop = ft->peak_y - body->y;
	const int   dmg  = (int)floorf(drop) - SURVIVAL_FALL_FREE;
	if (dmg <= 0) return false;

	const uint8_t before = s->health;
	if (before == 0) return false;   // already dead; do not re-report

	// Fall damage CAN kill — health floors at 0, not at 1. The opposite of starvation, on
	// purpose. See survival.h.
	s->health = (dmg >= (int)before) ? 0 : (uint8_t)(before - dmg);

	return s->health == 0;
}

// ── food ───────────────────────────────────────────────────────────────────────────────

// The whole food table. v1.8.14's meat and v1.8.15's cooked meat are one row each and touch
// nothing else in this file.
static const struct {
	BlockId id;
	uint8_t hunger;
} kFoods[] = {
	{ BLOCK_APPLE, 4 },
	// v1.8.14 "Animals". Four rows, one line each, exactly as this table's header note above
	// said they would be — that note is why this version's food change touches nothing else
	// in this file.
	//
	// The values sit deliberately BELOW the apple's 4, and the reason is v1.8.15 rather than
	// anything about this version: cooking has to be worth doing. A cooked cut must restore
	// strictly more than the raw one it came from, and with SURVIVAL_MAX_HUNGER at 20 there
	// is only so much room above. Start raw meat at the apple's level and the cooked tier has
	// nowhere to go but a number big enough to refill most of the bar from one item.
	//
	// Two tiers rather than four distinct numbers, and the split is the honest one:
	//
	//   porkchop 3, beef 3   the two big mammals — the substantial cuts
	//   chicken 2, mutton 2  the smaller animals
	//
	// This is the one table in this change where EQUAL values are right, which is worth
	// saying out loud given that world/registry.c's hardness ladder next door is deliberately
	// four distinct numbers. The two are not the same kind of number. A hardness is a
	// per-block fact a player learns by feel, so four identical break times would be four
	// blocks nobody can tell apart while mining — which is what coreHardnessIsDeclared()
	// exists to catch. A hunger value is a balance number: beef and pork being worth the same
	// is a statement that they ARE the same trade, and no distinctness rule reaches this
	// table.
	{ BLOCK_RAW_PORKCHOP, 3 },
	{ BLOCK_RAW_BEEF,     3 },
	{ BLOCK_RAW_CHICKEN,  2 },
	{ BLOCK_RAW_MUTTON,   2 },
};

uint8_t survivalFoodValue(BlockId id)
{
	for (size_t i = 0; i < sizeof kFoods / sizeof kFoods[0]; i++)
		if (kFoods[i].id == id) return kFoods[i].hunger;
	return 0;
}

bool survivalEat(Survival* s, Inventory* inv, uint8_t slot)
{
	if (!s || !inv) return false;
	if (slot >= INV_SLOT_COUNT) return false;

	// Refuse at full BEFORE anything is read out of the slot, so the "consumes nothing,
	// changes nothing" half of the contract is true by construction rather than by unwinding.
	if (s->hunger >= SURVIVAL_MAX_HUNGER) return false;

	InvSlot* sl = &inv->slots[slot];
	if (sl->count == 0) return false;

	const uint8_t value = survivalFoodValue(sl->item);
	if (value == 0) return false;   // not food, and an empty slot is ITEM_NONE which is not food either

	// Exactly one unit, out of THIS slot. A slot that hits zero goes back to the { ITEM_NONE,
	// 0 } shape inventoryInit() leaves — never a stale id with a zero count, which is the
	// invariant inventory.h states and which every "is this slot empty" test in the UI relies
	// on being one field compare.
	sl->count--;
	if (sl->count == 0) sl->item = ITEM_NONE;

	const int fed = (int)s->hunger + (int)value;
	s->hunger = (fed > SURVIVAL_MAX_HUNGER) ? SURVIVAL_MAX_HUNGER : (uint8_t)fed;

	// Health is deliberately untouched. Eating feeds regen; it is not itself a heal.
	return true;
}

// ── save / load ────────────────────────────────────────────────────────────────────────
//
// Byte-at-a-time little-endian, copied from world/playerpose.c's put32/get32 rather than
// re-derived, since this is the same "a save file must load on whichever machine reads it"
// problem. Both machines are little-endian today and this does not depend on that staying
// true.

static void put32(uint8_t* p, uint32_t v)
{
	p[0] = (uint8_t)(v);
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t* p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static bool survPath(char* out, size_t cap, const char* world_dir)
{
	return snprintf(out, cap, "%s/%s", world_dir, SURVIVAL_FILE_NAME) < (int)cap;
}

static bool tmpPath(const char* path, char* out, size_t cap)
{
	return snprintf(out, cap, "%s.tmp", path) < (int)cap;
}

// A NULL directory is the server session — main.c gates playerPoseSave on the same variable,
// so a joined world writes nothing to this card — and an empty one is a caller that has lost
// track of which world it is in. Both refuse rather than resolving to "./".
static bool dirUsable(const char* world_dir)
{
	return world_dir != NULL && world_dir[0] != '\0';
}

bool survivalSave(const Survival* s, const char* world_dir)
{
	if (!s || !dirUsable(world_dir)) return false;

	char path[512], tmp[512];
	if (!survPath(path, sizeof(path), world_dir)) return false;
	if (!tmpPath(path, tmp, sizeof(tmp))) return false;

	uint8_t buf[SURV_FILE_BYTES];
	buf[0x0C] = s->health;
	buf[0x0D] = s->hunger;

	// The crc covers the payload only and is stored above it, so computing it never has to
	// exclude itself — world/inventory.c's rule.
	put32(buf + 0x00, SURVIVAL_MAGIC);
	put32(buf + 0x04, SURVIVAL_VERSION);
	put32(buf + 0x08, crc32(buf + SURV_HDR_BYTES, sizeof(buf) - SURV_HDR_BYTES));

	FILE* f = fopen(tmp, "wb");
	if (!f) return false;

	const bool wrote_ok = fwrite(buf, 1, sizeof(buf), f) == sizeof(buf);

	// fclose is the flush: this is what makes the bytes actually reach the card rather than
	// sitting in stdio's buffer when the rename below runs.
	if (fclose(f) != 0 || !wrote_ok) { remove(tmp); return false; }

	// Windows' rename() refuses to replace an existing destination, so the old file has to go
	// first. remove() failing because `path` does not exist yet (the very first save) is
	// expected and is not a failure of this function.
	remove(path);

	if (rename(tmp, path) != 0) return false;

	// The window this leaves — a power cut between the remove and the rename — is closed by
	// survRecover below the next time anything tries to load this path.
	return true;
}

// Mirrors world/playerpose.c's poseRecover / inventory.c's inventoryRecover: if the last save
// was cut between removing the old file and renaming the new one into place, `path` is gone
// and `path.tmp` is a complete, unopened replacement. Promoting it here means survivalLoad
// never has to tell "never saved" apart from "saved, then interrupted right after".
static void survRecover(const char* path)
{
	char tmp[512];
	if (!tmpPath(path, tmp, sizeof(tmp))) return;

	FILE* t = fopen(tmp, "rb");
	if (!t) return;             // no interrupted save to recover
	fclose(t);

	FILE* real = fopen(path, "rb");
	if (real) { fclose(real); remove(tmp); return; }   // real file is fine; drop the leftover

	rename(tmp, path);
}

bool survivalLoad(Survival* out, const char* world_dir)
{
	if (!out || !dirUsable(world_dir)) return false;

	char path[512];
	if (!survPath(path, sizeof(path), world_dir)) return false;

	survRecover(path);

	FILE* f = fopen(path, "rb");
	if (!f) return false;   // the normal case for every world that existed before v1.8.13

	// One byte more than the record, so a file that is LONGER than 14 bytes is refused too.
	// world/genversion.c states the rule: "a file longer than the record is as wrong as a
	// short one". A survival file with anything appended to it was not written by this code,
	// and guessing what it means is how a format stops being one.
	uint8_t buf[SURV_FILE_BYTES + 1];
	const size_t n = fread(buf, 1, sizeof(buf), f);
	fclose(f);
	if (n != SURV_FILE_BYTES) return false;   // short, or trailing bytes

	if (get32(buf + 0x00) != SURVIVAL_MAGIC)   return false;
	if (get32(buf + 0x04) != SURVIVAL_VERSION) return false;

	const uint32_t stored   = get32(buf + 0x08);
	const uint32_t computed = crc32(buf + SURV_HDR_BYTES, SURV_FILE_BYTES - SURV_HDR_BYTES);
	if (stored != computed) return false;

	const uint8_t health = buf[0x0C];
	const uint8_t hunger = buf[0x0D];

	// The RANGE check the checksum cannot do. A crc proves the bytes are the bytes that were
	// written; it says nothing about whether they were sane when they were written, and a
	// health of 200 from a build with a different SURVIVAL_MAX_HEALTH would checksum
	// perfectly and then draw 200 hearts. playerpose.c pays the same toll for the same reason
	// (its isfinite/y-range pass, and its comment about why a checksum cannot catch a NaN).
	if (health > SURVIVAL_MAX_HEALTH) return false;
	if (hunger > SURVIVAL_MAX_HUNGER) return false;

	// Armed last, after every field has been validated, so *out is never left half-written on
	// a refusal.
	out->health = health;
	out->hunger = hunger;

	// The countdowns are not in the file — see survival.h. Reset to a full period so a
	// reloaded world does not drain hunger a fraction of a second after it opens.
	out->hunger_ticks = SURVIVAL_HUNGER_PERIOD;
	out->regen_ticks  = SURVIVAL_REGEN_PERIOD;

	return true;
}
