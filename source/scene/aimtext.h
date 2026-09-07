// v1.9.0 AIM-TEXT. The block-name readout under the reticle: "stone", "birch_planks",
// "chest" — whatever the block the crosshair is on is called in the registry — and the fade
// that takes it away again when the crosshair comes off a block.
//
// This is the PURE half. It decides whether a label is showing, what it says and how opaque
// it is; it never draws. The drawing is scene/crosshair.c's third and fourth parameters, which
// join the reticle's own sprite batch (one fewer draw call than a batch of its own, and the
// reticle already binds the font texture) — the split exists so that everything here can be
// linked and proved on the host, exactly the carve-out scene/ringorder.c made for main.c's
// ring walk. No <3ds.h>, no <citro3d.h>, no gfx/*.h: only world/registry.h, which is itself
// host-pure.
//
// ── Where the name comes from ──────────────────────────────────────────────────────────
//
// world/registry.h's BlockDef.name (registry.h:87, `char name[REGISTRY_NAME_MAX]`), read
// through registryGet(): the table is world/registry.c's `static BlockDef s_defs[REGISTRY_MAX]`,
// seeded from kCoreDefs and grown by registryRegister / registryRemoteApply at boot or join.
// That is the one authoritative name for a block — the same string registryFind() resolves
// and the DEFS packet carries — so this shows it verbatim. It is NOT prettified
// ("cooked_porkchop" stays "cooked_porkchop"): a display-name transform is a second table by
// another route, and it was not asked for.
//
// ── The two hide rules, and why the second one exists ──────────────────────────────────
//
// Nothing to name when nothing is targeted (RayHit.hit false) — obviously. ALSO nothing to
// name for air and for any id the registry does not define, and that second rule is not
// pedantry: registryGet() deliberately never returns NULL and answers the AIR row for an
// undefined id (registry.h:120), so a readout that trusted it would print "air" for a corrupt
// or stale id with no error anywhere. registryIsDefined() is the gate, and tests/aimtext_test.c's
// red arm for a dropped gate is what proves the string on screen is the string in the table.
//
// ── The fade ───────────────────────────────────────────────────────────────────────────
//
// Losing the target does not blank the label; it starts a ramp. The text that was showing
// stays exactly as it was and its alpha runs 255 -> 0 linearly over AIMTEXT_FADE_MS, after
// which aimTextLabel() answers NULL. Aiming at a nameable block at ANY point — mid-fade
// included — snaps the alpha straight back to 255, with the new name if the block changed.
// The ramp is clocked by the `now_ms` the caller passes (main.c hands it osGetTime()), not by
// frames, so it takes the same 0.4 s on a full 60 fps frame and a struggling 30 — the same
// reason world/tick.h exists — and the arithmetic is wrap-safe: `now - lost_at` in uint32_t is
// the elapsed time on either side of the 49-day rollover. It is milliseconds and not the
// 20 TPS tick count because a 400 ms fade is eight ticks, and eight steps of alpha is a
// staircase, not a fade.
//
// ── Cost ───────────────────────────────────────────────────────────────────────────────
//
// The label is formatted only when the targeted id CHANGES: same block two frames running is
// one byte compare and nothing else, and hiding never touches the cached text, so flicking the
// crosshair off a block and back on does not rebuild it either. `formats` counts every rebuild
// so the host test can assert that, rather than take it on faith. No allocation, ever: the
// text lives inline, REGISTRY_NAME_MAX bytes, and the whole struct is 28 bytes, so it is a
// file static in main.c and never a stack buffer.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "world/block.h"
#include "world/registry.h"

// The longest label this can ever hold. A registry name is NUL-terminated inside
// REGISTRY_NAME_MAX bytes (registry.c's copyName / registryDefUnpack both enforce it), so the
// longest possible name is 15 characters — "cooked_porkchop" in the shipped core table is
// exactly that. gfx/font.c emits one quad per printable non-space character, so this is also
// the readout's worst-case quad count per pass; see crosshair.c for the budget arithmetic.
#define AIMTEXT_MAX_CHARS (REGISTRY_NAME_MAX - 1)   // 15

// How long the label takes to go from fully opaque to gone once the target is lost, in the
// caller's milliseconds. 400 ms: long enough that grazing the edge of a block does not blink,
// short enough that the name of a block the player has already turned away from is not still
// hanging under the reticle when the next one is under it.
#define AIMTEXT_FADE_MS 400u

// The three states. A uint8_t field rather than an enum-typed one so the struct is the same
// size on the host and under the console's -fshort-enums; the values are what matter.
enum {
	AIMTEXT_HIDDEN = 0,   // nothing to show; alpha is 0
	AIMTEXT_SHOWN  = 1,   // a nameable block is under the reticle; alpha is 255
	AIMTEXT_FADING = 2,   // the target was lost at lost_at_ms; alpha is ramping to 0
};

typedef struct {
	BlockId  cached;                     // the id `text` was built for; BLOCK_AIR = nothing cached
	uint8_t  state;                      // AIMTEXT_HIDDEN / SHOWN / FADING
	uint8_t  alpha;                      // 0 (gone) .. 255 (full); what aimTextAlpha answers
	uint32_t lost_at_ms;                 // the now_ms of the update that started the fade
	uint32_t formats;                    // how many times `text` was (re)built — the cache's audit
	char     text[REGISTRY_NAME_MAX];    // NUL-terminated; meaningful only while cached != AIR
} AimText;

// All-zero IS the initial state (hidden, nothing cached, no formats), so a zero-initialised
// static needs no call. This exists for the per-session reset: dynamic ids are assigned at join
// and 0x80 on one server is not 0x80 on the next, so a label cached across a rejoin could name
// the previous server's block for one frame. Call it wherever interactInit() is called.
void aimTextInit(AimText* t);

// Once per frame, from wherever the aim raycast is resolved. `targeted` is RayHit.hit (and
// false when something other than a block — an animal — owns the aim); `id` is the block at
// the hit cell (worldGet on RayHit.x/y/z) and is ignored when !targeted, so a miss may pass
// BLOCK_AIR without reading the world. `now_ms` is any monotonic millisecond count; only the
// differences between successive calls matter, and they may wrap. Returns whether anything is
// visible afterwards — true through the whole fade, false only once alpha has reached 0.
bool aimTextUpdate(AimText* t, bool targeted, BlockId id, uint32_t now_ms);

// The string to draw, or NULL when hidden. Never an empty string and never a stale one: NULL
// covers every hidden case, so a caller that only tests for NULL cannot draw garbage. During a
// fade this is still the name that was showing when the target was lost — that is the point.
const char* aimTextLabel(const AimText* t);

// 255 while a block is targeted, ramping to 0 after it is lost, 0 whenever aimTextLabel() is
// NULL. Feed it to crosshairDraw() as the label's opacity.
uint8_t aimTextAlpha(const AimText* t);
