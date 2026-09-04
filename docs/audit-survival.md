# Survival mechanics audit — hunger, health, fall damage, animals, furnace, ores, caves, lava, zombies/skeletons

Audited 2026-09-04, branch v1.8.18 (uncommitted tree), by the SURVIVAL lane. Read-only audit —
nothing in `source/` was edited. All host-test evidence below was gathered by compiling and
running each stanza's exact link line **in isolation** under WSL (the full
`tools/run_host_tests.sh` run is currently red — an audio-cue-test failure at line 5152 sits
under `set -e` at line 224 and would silently blank out survival_test, animal_test and
monster_test, which all appear later in the file — so no claim below rests on the full-suite
log). No claim below rests on real 3DS hardware; nobody on this project has booted the current
tree on a console, and this audit does not change that.

## Problems, numbered

1. VERSION-LIST.md's v1.8.11 entry claims lava pools shipped. They did not — no `BLOCK_LAVA` id exists anywhere in the tree.
2. VERSION-LIST.md's v1.8.11 entry claims ravines shipped. They did not — zero references outside plan docs and one scope-out comment.
3. VERSION-LIST.md's v1.8.11 entry claims underground water (flood-to-water-table) shipped. It did not — no such logic in `cave_carve.c`.
4. Ore veins are not cave-targeted; they generate through all stone, exposed by caves only incidentally.
5. Zombie/skeleton spawning is a darkness gate, not a cave-specific predicate — it also fires on a dark surface at night.
6. Furnace fuel is wood-only (planks/logs); no coal or charcoal fuel exists yet, as `docs/VERSION-LIST.md` itself already says.
7. Tool tiers do not exist; ore is not gated behind anything to mine or use.
8. Breaking a furnace mid-cook is fixed (v1.8.17), but its lit front-face texture still never renders (known, deferred).
9. Nothing here has run on real 3DS hardware — every check below is a host x86-64 result.

## Verdict table

| Item | Verdict | Evidence (file:line) | Provenance |
|---|---|---|---|
| Hunger stat, drains, restores, saves | **Done** | `source/world/survival.h:1-140` (struct, constants); called at `source/main.c:5573`, `:6115` (eat), `:5860` (tick), `:4670`/`:7215` (load/save) | measured — read + isolated test run (`survival_test`: PASS 2592 checks, 0 failed, exit 0) |
| Health stat, drops, regenerates, zero-state | **Done** | Same file; regen gated `hunger>=18` for +1/4s, starvation −1/4s floored at 1 (`survival.h:14-24`); death path at `main.c:5691-5694` calls `survivalRespawn()` | measured — same isolated run |
| Fall damage | **Done** | `SURVIVAL_FALL_FREE=3` blocks free, `damage=floor(peak_y-landing_y)-3` (`survival.h:52`); tracked per-frame via `FallTrack`, called at `main.c:5691`; cancelled by water landing per header comment | measured — same isolated run; formula read directly, not re-derived |
| Animals (pigs/cows/chickens/sheep) | **Done** | `source/entity/animal.c`; herd spawn, idle/wander/flee AI, biome gating, meat drop on kill | measured — isolated run: `animal_test`: 236 checks, 0 failed, exit 0 |
| Furnace with fuel | **Partial — done for wood, no coal/charcoal** | `source/world/furnace.c:100-110` (`FURNACE_FUEL_TICKS_PLANKS`/`_LOG`); real cook/fuel state machine keyed through `blockstate.c`; wired at `main.c:2991-3102`, `:5987-5998`, `:6409-6416` | measured — isolated run: `furnace_test`: PASS 159 checks, exit 0. "Wood only, no coal/charcoal yet" is `docs/VERSION-LIST.md`'s own v1.8.15 line, confirmed by reading `furnace.c` (only two fuel branches exist) |
| Ores generate | **Done, not cave-specific (see problem 4)** | Mask built `worldgen_density.c:653`, consumed and written at `:843-846` (`block==BLOCK_STONE` guard, real payoff, not just a table) | measured — isolated run: `ore_gen_test`: PASS 180827 checks, 0 failed, exit 0; Y-band census in that run's own output matches the table `docs/VERSION-LIST.md` v1.8.12 documents |
| Caves, legacy-console style | **Done** | `source/world/cave_carve.c`: momentum-damped yaw/pitch drift (`:159-184`), tapered radius (`:193-206`), branching via recursive `caveWalkLoop()` (`:216-220`) | measured — isolated run: `cave_carve_test`: PASS 494/494 checks, exit 0; behaviour also read directly, not inferred from the test alone |
| Lava pools | **Missing** (problem 1) | `source/world/cave_carve.h:22-26` explicitly lists lava as "out of scope, deliberately... not implemented here" and states it "needs a new BLOCK_LAVA id"; `grep -rn "BLOCK_LAVA" source/` returns nothing; `CHANGELOG.md`'s real `[1.8.11]` entry (lines 640-700) never mentions lava | measured — direct source read + a grep with a known-good red control (the same grep pattern correctly finds `BLOCK_WATER` at `block.h:130`, so the tool works and the absence is real, not a search miss) |
| Ravines | **Missing** (problem 2) | Same `cave_carve.h:22-26` scope-out; `grep -n "ravine" source/world/cave_carve.c` returns 0 hits; only appears in `docs/plan-1.8.11-caves.md` as a proposal (§2.5, "could make the whole neighbourhood more expensive... a separate, wider R_ravine") | measured |
| Underground water (flood-to-water-table) | **Missing** (problem 3) | Same scope-out comment lists "water" as deliberately excluded; no flood/water-table logic found in `cave_carve.c` | measured |
| Zombies and skeletons: behavior | **Done** | `source/entity/monster.c`: zombie chase+melee cooldown (`:190-` region), skeleton windup/LOS-recheck/fire/cooldown, shared idle-wander from `animal.c`, 2 Hz torch despawn (`:320-333`), damage plumbed to the player via `survivalDamage()` at `main.c:5939` | measured — isolated run: `monster_test`: 126 checks, 0 failed, exit 0 (matches the figure given in the task brief, independently reproduced) |
| Zombies and skeletons: spawn "in caves" | **Partial** (problem 5) | `monsterSpawnTick()` at `monster.c:379-460`: gate is darkness (`lightGetSky()==0 && lightGetBlock()==0`, `:432`) plus a generic solid-footing test (`monsterSpotIsGood()`, `:371-377`, deliberately dropped the grass/dirt allow-list "because a monster spawning in a cave stands on stone, not grass") — no `worldgenIsCave()` or cave-mask check anywhere. A sufficiently dark surface at night (no torches) will also spawn monsters | measured — same isolated run, plus direct read of the spawn predicate |
| Hotbar icon shape bug (pork chop etc.) | **Fixed, verified no other current item is affected** | `source/gfx/item_icons.h:52-91`: 9 dedicated item-art slots (apple + 4 raw + 4 cooked meats) map via `itemIconTile()`; `source/scene/ui.c:398-406` falls back to `blockFaceTex(id, FACE_TOP)` only when `itemIconTile()` returns `ITEM_ICON_NONE`. Checked the full `BlockId` enum (`block.h`, ids 0-43): every remaining id is either a real cube (correct as a top-face icon) or a cross-shaped plant/torch that already carries real alpha-cut art in its block face texture (shipped v1.8.8/v1.8.10, unlike the meats' original flat-color placeholder) | measured by source read only — **not visually confirmed**; no screenshot or emulator/hardware render was taken, per the task's own "nobody here can boot real hardware" constraint. If a future item is added as a "drop" without adding it to `itemIconTile()`, it will silently regress to the old failure mode — the comment at `ui.c:394-397` says so itself |

## What is genuinely missing, and what it would cost

### Lava pools (and lava damage)
Not built. `cave_carve.h`'s own header names the exact blocker: a new `BLOCK_LAVA` id must be
appended to `source/world/block.h`, which is one of the 11 files mirrored to
`deps/blocksmith-server/game/...` and gated by the `check-world-drift` Makefile target — so this
is not a client-only change. Per `docs/plan-1.8.11-caves.md` §3.2, the actual generation
mechanism was already designed: inside the existing carve-stamp function, any cell the carve
would otherwise leave as air below a fixed Y (`GEN_LAVA_LINE`) gets `BLOCK_LAVA` instead of
`BLOCK_AIR`. That is a small, local change inside `cave_carve.c`'s stamp write. What is not small:
(a) the registry/server-sync coordination the plan calls out explicitly (§3.2, "the lava
block-registry change and its server sync must land together"), (b) whether lava should do fall-
style or tick-style damage — `survival.h:78` already anticipates this ("the day a tick-driven
lethal effect is added — poison, drowning, a lava tick — the caller's death handling is already
wired") but no such call exists yet, and (c) lava's own render/animation, which does not exist in
the texture atlas today. Rough size: a real feature, not a one-line fix — new block id + server
coordination + damage wiring + art. This is the kind of scope-and-asset decision the project's
own rules say is not mine to just build; flagging it rather than doing it.

### Ravines
Also scoped in `docs/plan-1.8.11-caves.md` §2.5/§10 as reusing the same walked-path machinery
with different tuning (far less drift, roughly straight, ~10x rarer than tunnels, no branching) —
the plan explicitly frames it as a second per-region draw sharing the existing scan. Genuinely
smaller than lava (no new block id, no server coordination) but still real work: a second preset
table, a second region-hash salt, and the plan's own risk note that a wider `R_ravine` would
inflate the per-column neighbourhood scan cost for every column, not just ones with a ravine
nearby.

### Underground water (flood-to-water-table)
Smallest of the three missing v1.8.11 items — the plan frames it as reusing `world/water.c`'s
existing fluid simulation with a straightforward flood rule, and no new block id is needed since
`BLOCK_WATER` already exists (`block.h:130`). Still not zero-cost: it needs a rule for where the
water table sits per column and a decision about interaction with the walked-carver's cross-chunk
mask.

### Tool tiers gating ore
Not scoped to any version yet, per `docs/VERSION-LIST.md`'s own v1.8.12 entry. Not investigated
further here — out of the nine-item list this audit was asked to cover.

## What I could NOT verify

- **Nothing here has run on real 3DS hardware.** Every check above is a host x86-64 compile and
  run under WSL/gcc. No emulator screenshot, no console boot.
- **The hotbar icon fix was not visually rendered.** Verified by reading `item_icons.h` and
  `ui.c`'s fallback logic and cross-checking every `BlockId` against it — not by looking at a
  rendered frame.
- **The full `tools/run_host_tests.sh` run was not fixed or re-run to green.** The audio-cue-test
  failure that blanks the tail of the script is out of this lane's scope (survival/animals/
  furnace/ores/caves/monsters, not audio) and was left alone; the six relevant stanzas were instead
  extracted and run standalone to get real, uncontaminated exit codes, which is the isolation
  the task's own methodology section asked for.
- **Whether monster spawning "reads right" in actual play** (does it feel cave-specific enough,
  does darkness-only spawning produce surface zombies too often) is a playtest question, not
  something a host test or a source read can answer.

## Fixed vs. missing — nothing was fixed in this session

No source file was edited. The one candidate — the hotbar icon fallback comment self-documents
its own future failure mode (`ui.c:394-397`) — is not itself broken; it correctly falls back to
the pre-fix behavior only for items that were never given dedicated art, and no such item
currently exists in the tree. There was nothing unambiguously broken to fix on sight.
