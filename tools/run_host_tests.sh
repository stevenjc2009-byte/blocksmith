#!/bin/sh
# Compiles the world data code with the host compiler and runs its self-test.
#
# Everything under source/world is plain C with no 3DS dependency, so it can be
# tested here in a second instead of through a devkitARM build and an emulator.
# The file list is explicit rather than a glob because these binaries each need a
# different subset of it, and because a glob would sweep in the _test.c files, every one
# of which carries its own main().
#
# world/block_tiles_check.c — the bridge that checks the two duplicated tile enums — is
# what that sentence used to be about: it included gfx/atlas.h, that header includes
# <3ds.h>, so the file was console-only and this script skipped it. It has its own stanza
# below now and is compiled on every run. See there for what the skip was costing.
#
# source/scene/render_dist.c is in the list under the same rule: step 7.7 put the
# render distance arithmetic in a file with no <3ds.h> precisely so the fog and
# near-plane numbers could be checked here instead of by squinting at the console.
#
# v1.6.0 task 10 added the dynamic-block half of testVisConnectivity to this binary.
# world/visgraph.c's openTable() filled only ids 0..BLOCK_COUNT-1 from the registry and left
# 0x08..0xFF at the air prefill, so a server-registered SOLID block (0x80 and up) read back
# see-through: the cave cull believed sight passed through a wall built out of one and culled
# everything behind it while the wall itself still drew. The new checks register a real dyn
# row through registryRegister() — the same call world/registry.c's DEFS path ends in — and
# assert the connectivity mask for a solid one, a transparent one, and a tunnel bored through
# the solid one (so the flood fill is exercised, not just the UNIFORM short-circuit).
#
# Sabotaged and measured: openTable() put back verbatim to the old prefill + BLOCK_COUNT loop.
# Before the widening this binary printed "world self-test: PASS 2768 checks"; with the new
# checks present and the old body restored it prints "FAILED - 2 of 2776 checks", first
# failure "L3263 visChunkConnectivity(c, &s_vis_scratch) == 0". With the loop at REGISTRY_MAX:
# "PASS 2776 checks".
#
# v1.6.0 task 12 added source/world/meshq.c to this binary's link and
# testMeshqRefusalIsRecoverable() to world_test.c. meshq.c exists so the fix could be tested
# at all: the bug is an ORDERING bug inside main.c's genQueueReadyColumns, main.c carries
# main() and includes <3ds.h>, and this project's rule is that a test links the real module
# rather than a copy of it — the same split app/battery.c and app/debugmenu_ui.c already use,
# and for the reason recorded against both of them further down this file.
#
# The bug: the column was marked queued BEFORE its chunks were pushed, and the mark did not
# depend on the pushes. So a jobqPush refused because the ring was full lost its chunk for the
# rest of the session — a permanent hole in the world that walking around does not repair. It
# was latent only because a radius-2 ring is 125 measured chunk meshes against JOBQ_CAP 128;
# radius 3 makes the burst 242 measured (392 structural).
#
# Sabotaged in the real code and measured, each restored afterwards and the restore verified
# by md5 (meshq.c back to c123385dee02183acc8ab196df1058a8 every time). Green arm throughout.
#
#  * meshqPushColumn's result ignored (`return all_pushed || true;`, i.e. the pre-fix
#    ordering) -> 2 failed, "!queued" and "meshed_mask == expect_mask". The second is the
#    hole itself: three chunks refused, never re-queued, never meshed.
#  * JOBQ_CAP put back to 128 -> 2 failed, first "JOBQ_CAP >= 49 * COLUMN_CHUNKS".
#  * renderDistDefault returning RENDER_DIST_MAX for a New 3DS again -> 2 failed, first
#    "renderDistDefault(true) == RENDER_DIST_DEFAULT_NEW".
#  * RENDER_DIST_MAX put back to 2 -> 9 failed, first "RENDER_DIST_MAX == 3".
#
# v1.6.0 F3 (second half) added testLightBlockChannelFromDynamicId() to world_test.c.
# world/light.c's anyLuminance() — the gate BOTH lighting engines ask before running the
# block-light pass at all — looped `i < BLOCK_COUNT` over an s_luminance table that is the
# full 256-id space wide, so a luminance set on a server-registered id (0x80..0xFD) read back
# as "nothing in this world glows" and the emitter sat dark with no error anywhere. Same class
# of bug as the visgraph one above it. Dormant today (no dynamic block declares luminance
# yet), fixed anyway.
#
# Sabotaged and measured: the loop bound put back to BLOCK_COUNT, everything else untouched.
# "world self-test: FAILED - 3 of 2962 checks", first "L5242 lightGetBlock(col, 7, 42, 7) == 7"
# — exactly the three cells the emitter should have lit; the four cells that should read 0
# stay green either way, and the pre-existing core-block arm (testLightBlockChannel, emitter
# at BLOCK_WOOD) stays green as the control. With the bound at REGISTRY_MAX: "PASS 2962
# checks", three consecutive runs. light.c restored to md5 7cb6624daa5b05579976861dc688e19c.
#
# Cost of the widening, measured on the host at -O1 (the scan is called once per
# lightFloodColumn/lightRelightColumn, never inside their cell loops, and neither runs per
# frame — the callers are one block edit and one generated column on the worker thread):
# 79.1 ns/call at 256 entries against 0.3 ns at 8, i.e. 78.7 ns added to a lightRelightColumn
# measured at 720.1 us — 0.011% of the call it gates. The 268 MHz ARM11 figure is an
# instruction-count estimate (~3.7 us), NOT a hardware measurement.
#
# NOTE ON THE FIRST ARM. It was first attempted as a plain `return true;` and that arm never
# ran: gcc rejected it with "error: variable 'all_pushed' set but not used
# [-Werror=unused-but-set-variable]", BUILD_EXIT=1. A sabotage that does not compile proves
# nothing, and had it been recorded as "red" it would have been a lie. `|| true` reads the
# variable and still always returns true, which is what the old code did.
#
# v1.7.1 task 48 added testRegionCache() to world_test.c, for world/region.c's new
# regionReadColumnCached(). The worker called regionReadColumn once per JOB_GENERATE — 81 of
# them for one genRequestArea at RENDER_DIST_MAX — and each call re-opened the region file and
# re-parsed both 3088-byte directory copies to answer a question about one 12-byte slot. That
# is per-column work paying for a per-region answer, and it is why loading a world that had
# been EDITED took about twice as long as creating one: a fresh world has no region file, so
# none of it ran. The cache keeps the handle and both parsed directories between calls.
#
# Measured with a probe built at -O1 with -DBS_REGION_PROBE over 16200 column reads (the
# 81-column area, 200 times, every column saved so both arms also do the payload read):
# 39.2 / 43.5 / 56.0 us per column uncached across three runs against 3.34 / 3.34 / 3.64
# cached — one area falling from 3.2-4.5 ms to 0.27-0.29 ms. The counts are the un-noisy half:
# 32400 fopen and 32400 dirRead uncached, 2 and 2 cached. Host page cache, not a FAT32 SD card
# behind a 268 MHz ARM11, where the open is the dearer half — so this understates the saving.
# The cache costs 12656 bytes of .bss (printed by the probe, not estimated).
#
# The whole risk is staleness: a directory parsed before a column was saved reports it as never
# saved, and app/worker.c then regenerates it from the seed — a player's build deleted, not
# corrupted, with no error anywhere. So every writer in region.c drops the cache, in
# openRegion(create=true) and again after regionWriteColumn's directory write.
#
# Sabotaged in the REAL source/world/region.c and measured, each arm restored afterwards and
# the restore confirmed by md5 (region.c back to b9720d7bc15e67bee9cd87e0fbf00f8e every time),
# built with -DWORLD_TEST_VERBOSE so EVERY failing line was read and not just the first.
#
# Run against a frozen snapshot of the tree rather than the working copy, because a parallel
# session was editing ~26 files in it at the time — so the CONTROL here is red, on that
# session's own in-flight mesher and water-walk-speed failures (L2339, L4101, L4102, L4117),
# and what each arm proves is the DELTA against a control taken from the same snapshot:
# "FAILED - 4 of 4370 checks" green-arm baseline, no failure of this test among them.
#
#  * BOTH invalidation drops disabled                -> 15 of 4370, +11, all in this test:
#    the negative entry surviving its first save (L7997/7998), the edit-then-reload that
#    would cost the blocks (L8008/8009), the repeat read (L8015/8016), the cached-vs-uncached
#    equivalence (L8022/8023), and the cross-region reloads (L8039/8040, L8050).
#  * the world directory dropped from the cache key  -> 5 of 4370, +1, exactly
#    "L8070 memcmp(got, b_bytes, b_n) == 0" — one world served the other world's blocks, and
#    that is the only check written for it.
#  * the region coordinates dropped from the key     -> 8 of 4370, +4: L8039/8040, L8050 and
#    L8055, i.e. every read that crosses a region boundary. Nothing else moves.
#
# Two further arms were run and are recorded because their result is the point: disabling
# openRegion's pre-write drop ALONE, and disabling regionWriteColumn's post-write drop ALONE,
# both left the suite at its green baseline. The two are deliberately redundant, so neither
# can be red on its own; only the both-disabled arm can go red, and it does.
#
# NOT recorded as arms, for the reason the meshq note at the top of this file gives. The
# key-dropping arms were first written as `if (0) continue;`, and gcc rejected both outright
# ("error: unused parameter 'world_dir'", "'rx'", "'rz'" [-Werror=unused-parameter]),
# GCC_EXIT=1 — and the harness then ran the PREVIOUS arm's binary and reported its failures as
# if they were this arm's. A sabotage that does not compile proves nothing, and a harness that
# runs a stale binary proves less than nothing. Rewritten as `world_dir == NULL` and
# `rx == INT32_MIN || rz == INT32_MIN`, which read every parameter and are never true.
#
# v1.7.1 task 48c added four tests to world_test.c for world/region.c's regionCompact():
# testRegionCompactPreservesColumns, testRegionCompactPowerCut, testRegionCompactCache and
# testRegionCompactKeepsPowerCutFallback. The function had NO caller and NO test, while
# region.h's header comment said it "reclaims the waste when a world is closed" — which was
# true of no build ever shipped. Unproven code holding the only bytes on the card a player
# cannot regenerate, described by a comment that was false.
#
# Control on the same snapshot before the four tests existed: "world self-test: PASS  4370
# checks", exit 0. With them: "PASS  4693 checks", exit 0 — +323, all in the new tests.
# Every arm below is a sabotage of the REAL source/world/region.c, built with
# -DWORLD_TEST_VERBOSE so every failing line was read rather than just the first, restored
# afterwards, and the restore confirmed by md5 (region.c back to
# 595c36b19b9d0f90809c6b40aadb190d every time). The green arm was re-run as a control and
# printed the 4693-check pass on both sides of the run.
#
# The preservation and size claims, which have to be able to fail independently — an
# implementation that copies the file unchanged cannot lose a column, and one that pads
# cannot lose one either:
#
#  * repack keeps the PRE-compaction offsets (packed.ent[i].off = dir.ent[i].off)
#    -> FAILED - 17 of 4693, first "L8211 compactReadAll(testWorldDir()) == 0". All eight
#    per-column regionDecodeColumn checks, the untouched-others check, both crash sweeps and
#    the cached read go with it. compaction's "after < before" stays GREEN in this arm.
#  * repack pads instead of packing (packed.arena_end += len + 4096)
#    -> FAILED - 3 of 4693, first "L8207 after < before". compactReadAll stays GREEN, which
#    is what says the size claim is checked separately from the contents claim.
#
# The crash-safety claim. regionCompact writes a whole .tmp, closes it, removes the .bsr and
# renames; regionRecover() resolves whichever pair a cut leaves behind. Both of its branches
# were disabled in turn and each takes down its own stage and no other:
#
#  * regionRecover promotes the .tmp even when a .bsr is present
#    -> FAILED - 1 of 4693, "L8322 s1_bad == 0". Stage 1 only. Stages 2 and 3 stay GREEN.
#  * regionRecover never promotes (rename(tmp, src) -> remove(tmp))
#    -> FAILED - 3 of 4693, first "L8333 compactReadAll(cutdir) == 0", plus the promoted-file
#    size check and stage 3's non-vacuity. Stage 1 stays GREEN.
#
# The cache claim. regionCompact is the writer that does not go through openRegion(create=
# true), so it carries its own two regionCacheClose() calls. Same result as the write path
# above, and recorded because the result IS the point — they are deliberately redundant:
#
#  * the drop at the top of regionCompact alone disabled     -> PASS 4693, GREEN.
#  * the drop before the remove/rename alone disabled        -> PASS 4693, GREEN.
#  * BOTH disabled -> FAILED - 1 of 4693, "L8436 regionReadColumnCached(testWorldDir(),
#    s_cc_cx[2], s_cc_cz[2], got, sizeof(got)) == 0". Exactly the check written for it, and
#    the uncached read on the line above it stays GREEN — so the file really was destroyed
#    and the cache was the only thing still answering from the file compaction replaced.
#    That check exists because compaction preserves every column's bytes, so a stale entry
#    returns the right answer and hides itself unless the new file is destroyed underneath it.
#
# THE DEFECT. Writing these tests found one, in shipping code, and it costs a column of a
# player's world. regionCompact read only the LIVE directory copy. dirLoadPair's own comment
# says why the other one matters: a card can flush the directory sectors before the payload
# sectors, so after a power cut the newest directory can point at a payload that is not all
# there while the previous one still describes a complete older copy, and regionReadColumn
# falls back to it — "the difference between a power cut costing the last save and a power cut
# costing the whole column". The repack dropped such a column and then deleted the old file
# that still held it. Permanent, silent, and it would have happened at world close.
#
#  * red-before, on the shipping region.c (md5 b9720d7bc15e67bee9cd87e0fbf00f8e) with the
#    test present and no fix: "FAILED - 2 of 4693 checks", "FAIL L8527 n == a_n" and
#    "FAIL L8528 memcmp(got, a_bytes, a_n) == 0" — the column read correctly on the line
#    before regionCompact and as nothing on the line after. The other 4691 checks, including
#    the three other new compaction tests, stayed GREEN.
#  * the fix (entryRead plus the same two-entry fallback reads use) re-disabled as an arm
#    -> the same two lines, "FAILED - 2 of 4693".
#
# NOT recorded as a passing check, because it could not have failed: L8528's memcmp was first
# written without clearing the buffer, so a failing read left the PREVIOUS read's bytes in
# `got` and the memcmp passed on the old answer — one red line where there should have been
# two. The memset above it is what makes the second check able to go red, and it does.
#
# CORRECTED IN v1.8.2, and corrected in place rather than deleted so the reversal stays
# readable. What stood here through v1.7.1 and v1.8.1 was "regionCompact is still not wired to
# anything, on the evidence rather than despite it": the remove-before-rename window depended
# on the .tmp being durable on the card, nothing in region.c fsyncs, and neither that nor
# rename() on libctru's sdmc devoptab could be tested from this suite. v1.8.2 closed that
# window instead of accepting it — the swap now renames the old .bsr aside to .bak first and
# removes it last — and wired the call in through regionMaintain(), called from app/worker.c's
# workerWriteSave. So regionCompact HAS a caller now, and source/world/region.h's header says
# the same. What that cost and what still cannot be measured from a host is in the
# region_growth_test stanza at the bottom of this file. Every number above is a host number.
#
# Usage, from the project root:  sh tools/run_host_tests.sh
set -e

cd "$(dirname "$0")/.."

# Every run gets its own subdirectory under build-host, named after this shell's own
# PID, rather than all runs sharing build-host/ directly. Measured cause: two Claude
# sessions running this script at the same time both write build-host/world_test(.exe)
# — whichever gcc finishes second silently overwrites the binary the other session is
# about to exec, and on Windows a still-open exe can also make the second gcc's link
# step fail outright with a sharing violation. A stray same-named foreign binary left
# behind this way was caught directly: build-host/ui_layout_test (no .exe extension, an
# ELF Linux binary from a non-Windows session) sat next to build-host/ui_layout_test.exe
# and bash's exact-name match picked the ELF file, failing with "cannot execute binary
# file: Exec format error" — not a hang, but the same shared-directory collision this
# guards against. $$ is this shell's own PID, unique per invocation, so concurrent runs
# never touch each other's binaries. Removed at the end, but only on success — a failed
# run's directory is left behind on purpose so its binaries can still be inspected.
BH="build-host/run-$$"
mkdir -p "$BH"

gcc -std=c11 -Wall -Wextra -Werror -O1 -g \
	-I source \
	tests/host_test.c \
	tests/net_stub.c \
	source/world/block.c \
	source/world/registry.c \
	source/world/chunk.c \
	source/world/chunk_codec.c \
	source/world/crc32.c \
	source/world/region.c \
	source/world/world.c \
	source/world/scratch.c \
	source/world/mesher.c \
	source/world/noise.c \
	source/world/raycast.c \
	source/world/physics.c \
	source/world/remesh.c \
	source/world/handbuilt.c \
	source/world/budget.c \
	source/world/dirtyq.c \
	source/world/jobq.c \
	source/world/meshq.c \
	source/world/relightq.c \
	source/world/tick.c \
	source/world/visgraph.c \
	source/world/worldgen.c \
	source/world/worldgen_density.c \
	source/world/genversion.c \
	source/scene/render_dist.c \
	source/world/world_test.c \
	-lm \
	-o "$BH/world_test"

"./$BH/world_test"

# world/block_tiles_check.c — the BTEX_*/TILE_* mirror (v1.8.3).
#
# COMPILED, NOT RUN, and the only stanza in this file that is. The whole translation unit is
# _Static_assert; there is nothing to execute, and wrapping it in a main() that printed
# "PASS 12 checks" would be a check that cannot fail — this project has eleven recorded cases
# of exactly that. gcc exiting non-zero IS the red arm, and `set -e` above turns it into a
# failed suite.
#
# WHY IT IS HERE AT ALL. world/block.h hand-mirrors gfx/atlas.h's TILE_* enum as BTEX_*, and
# this file's _Static_asserts were the only thing holding the two together. It included
# gfx/atlas.h, which includes <3ds.h>, so it was never linked into any host binary and this
# script's own header comment said so. A mismatch therefore compiled silently here and was
# caught only by a devkitPro console build, if somebody happened to run one.
#
# Measured on the unfixed tree, on a frozen snapshot (a parallel session's in-flight
# genversion work had world_test at "FAILED - 4 of 5179", which `set -e` turns into an
# aborted suite, so the files that session was editing were restored to HEAD inside the
# snapshot only). Green control: "world self-test: PASS  5143 checks", SUITE_EXIT=0. That
# control reads 5184 in the red arms further down rather than 5143: HEAD moved between the two
# measurements, on that other session's work. It is green in every run either way, which is all
# it is being asked.
#
#  * gfx/atlas.h's TILE_TALL_GRASS given the value 12 while world/block.h's BTEX_TALL_GRASS
#    stayed 11 — a real mirror mismatch, the exact thing the asserts exist for -> SUITE_EXIT=0,
#    "atlas uv shader self-test: PASS  4280 checks", and grep -c FAIL over the whole run: 0.
#    The suite could not see it, because nothing it compiles had ever heard of TILE_*.
#
# The mismatch was introduced on the TILE side deliberately, and the reason is worth keeping:
# the same drift introduced on the BTEX side is caught INCIDENTALLY, by something that is not
# about the atlas at all. world/block.h's BTEX_TALL_GRASS set to 12 was measured first and gave
# "FAIL 53 checks, 1 failed", the one being registry_test's "core-only crc matches the pinned
# golden 0x4066" — because a core block's tex[] bytes go into that crc16. That covers only the
# tiles a CORE block's faces actually use (never TILE_SENTINEL, and nothing a server-registered
# dyn row picks), it says nothing about the atlas in its message, and it disappears the moment
# the golden is re-pinned. It is not a guard on the mirror; the TILE-side arm is what shows the
# hole with nothing else standing in front of it.
#
# THE FIX, and what was rejected. The tile list moved to gfx/atlas_tiles.h, which has no 3DS
# dependency; gfx/atlas.h includes it, so the console build sees the same enum it always did,
# and this file includes atlas_tiles.h instead of atlas.h. Rejected: deleting world/block.h's
# BTEX_* mirror now that TILE_* is includable from the host — that would make source/world
# depend on source/gfx, which block.h's opening comment exists to prevent, and it would touch
# every file that names a BTEX_*. Rejected: generating the asserts from tools/make_atlas.py's
# TILES list — it adds a codegen step to a build that has none.
#
# world/block.h itself is NOT touched by this change, and that is a decision rather than an
# oversight: it is vendored into deps/blocksmith-server/game/world/block.h, and the Makefile's
# check-world-drift target fails the console build outright when the two differ. Measured — an
# earlier version of this work added a BTEX_USED_COUNT sentinel there and `make` stopped with
# "Makefile: deps/blocksmith-server/game/world/block.h has drifted from source/world/block.h",
# MAKE_EXIT=2, before compiling anything. Closing that last case needs a sync and a review in a
# repository this change does not own, so it is left open and named in block_tiles_check.c.
#
# THE SECOND DEFECT, which is the miss-prone one. One assert per tile means omitting a line
# leaves that tile unguarded, silently, on host and console alike — and v1.8.3 Phase 3 appends
# about six tiles at once. The pairs are an X-macro list now, expanded twice: once into the
# twelve asserts, once into a count that is asserted equal to gfx/atlas_tiles.h's
# TILE_USED_COUNT. A tile appended without its assert is a build error.
#
# Both halves sabotaged in the REAL production headers and measured, each restored afterwards
# and the restore confirmed byte-identical by md5 AND cmp (block.h back to
# 2e22f1758b2dc87483dde29b00c8e650, atlas_tiles.h to 39e9e5cf292aaf15c5676f52a0662ccf). Control
# in both arms: the world_test stanza immediately above, which runs FIRST and printed
# "world self-test: PASS  5184 checks" in the healthy build and in each arm — so the red below
# is the mirror, not the tree failing to build in general.
#
# The two arms fail through DIFFERENT asserts, one each, which is what says the two guards
# discriminate instead of both firing on any edit at all:
#
#  * world/block.h's BTEX_TALL_GRASS given the value 12 (the value-mirror arm)
#    -> ONE error: "static assertion failed: "atlas tile order changed - BTEX_TALL_GRASS and
#    TILE_TALL_GRASS disagree; update world/block.h to match gfx/atlas_tiles.h"", with gcc's
#    expansion notes naming the exact X(BTEX_TALL_GRASS, TILE_TALL_GRASS) line. The other
#    eleven pair asserts and the pair-COUNT assert stay green: the list did not change length.
#  * gfx/atlas_tiles.h given a thirteenth tile, appended, with no X() line here (the
#    missing-assert arm) -> ONE error: "a tile was added (or removed) without its assert, so
#    that tile is unguarded. Add the missing X(BTEX_..., TILE_...) line." ALL TWELVE per-pair
#    asserts stay green — they all still hold — which is exactly the failure the hand-written
#    form could not see, and the reason the count assert is not redundant with them.
#
# NOT recorded as an arm, and worth the line. The missing-assert arm was first written as an
# INSERTION (TILE_GRAVEL between TILE_WATER and TILE_TALL_GRASS) and that also renumbers
# TILE_TALL_GRASS, so it takes the pair assert down with the count assert and proves nothing
# about which guard caught what. Appending is both the realistic edit and the one that
# isolates the count assert.
#
# tools/make_atlas.py needs no change for either arm: the count is derived from the C enum,
# not from that script's TILES list.
#
# NOT recorded as an arm, and the reason is the mirror image of the meshq note at the top of
# this file. Both count asserts were first written without the (int) casts, and gcc rejected
# the HEALTHY tree with "error: comparison between 'enum <anonymous>' and 'enum <anonymous>'
# [-Werror=enum-compare]", SUITE_EXIT=1. A sabotage that does not compile proves nothing; a
# guard that does not compile guards nothing, and it would have been read as a red arm working.
# The casts are why the per-pair asserts have always had them.
gcc -std=c11 -Wall -Wextra -Werror -O1 -g \
	-I source \
	-c source/world/block_tiles_check.c \
	-o "$BH/block_tiles_check.o"

echo "block tiles mirror: BTEX_*/TILE_* static asserts compiled"

# The master block registry itself (v1.6.0 Phase A). Sixteenth binary, own main(),
# same reason as every one above it: registration, name lookup, the full-range
# refusal, crc16 stability (with a pinned golden for the core table) and the
# registry.bin sidecar round-trip are all plain-C contracts provable here in
# seconds, and a broken registry must not stop the world suite from running.
gcc -std=c11 -Wall -Wextra -Werror -O1 -g \
	-I source \
	source/world/block.c \
	source/world/registry.c \
	source/world/registry_test.c \
	-o "$BH/registry_test"

"./$BH/registry_test"

# Step 8.4's options.ini module, built and run separately rather than folded into the list
# above. It carries its own main() — the world suite is driven by tests/host_test.c — and
# two mains cannot share a link. Separate binaries also mean a broken options parser cannot
# stop the world suite from running, which is the half that guards the save format.
gcc -std=c11 -Wall -Wextra -Werror -O1 -g \
	-I source \
	source/app/options.c \
	source/app/options_test.c \
	-o "$BH/options_test"

"./$BH/options_test"

# Universal input remapping logic (source/app/remap.c). Tenth binary, own main(),
# same reason as options_test above it: pure logic that needs no <3ds.h>, tests
# conflict swapping, reset-to-defaults, and round-trip persistence through the
# existing options.ini mechanism.
gcc -std=c11 -Wall -Wextra -Werror -O1 -g \
	-I source \
	source/app/options.c \
	source/app/remap.c \
	source/app/remap_test.c \
	-o "$BH/remap_test"

"./$BH/remap_test"

# Step 8.4's world-list module. Third binary for the same reason options_test is a second
# one: it carries its own main(). scene/worldlist.c lives under scene/ but has no <3ds.h>
# in it — the directory says where it belongs in the program, not what it depends on.
gcc -std=c11 -Wall -Wextra -Werror -O1 -g \
	-I source \
	source/scene/worldlist.c \
	source/world/worldlist_test.c \
	-o "$BH/worldlist_test"

"./$BH/worldlist_test"

# Step 8.2's inventory and crafting model. crc32.c is in the link because inventory.c's
# save file is checksummed the same way a region file is, and the checksum implementation
# is shared rather than duplicated.
gcc -std=c11 -Wall -Wextra -Werror -O1 -g \
	-I source \
	source/world/inventory.c \
	source/world/crafting.c \
	source/world/crc32.c \
	source/world/inventory_test.c \
	-o "$BH/inventory_test"

"./$BH/inventory_test"

# Step 8.2's UI hit-testing/layout arithmetic (scene/ui_layout.c), pulled out of
# scene/ui.c precisely so it could be linked here without <citro3d.h> -- see
# scene/ui_layout.h's file comment. Fourth binary for the same reason inventory_test
# is a separate one: it carries its own main(). Only ui_layout.c itself is linked in --
# it pulls in world/inventory.h and world/crafting.h for INV_*/RECIPE_COUNT constants
# only (compile-time macros), never calling a function those headers declare, so
# inventory.c/crafting.c have nothing this binary needs from them.
gcc -std=c11 -Wall -Wextra -Werror -O1 -g \
	-I source \
	source/scene/ui_layout.c \
	source/scene/ui_layout_test.c \
	-o "$BH/ui_layout_test"

"./$BH/ui_layout_test"

# The multiplayer screen's navigation decision (scene/title_nav.c), pulled out of
# scene/title.c for the same reason ui_layout.c came out of scene/ui.c above: title.c draws
# with gfx/sprite.h and gfx/font.h and includes <3ds.h> for KEY_*, so it cannot be linked
# outside a devkitARM build, and what was worth checking here was not a rectangle but a rule.
# Own main(), same reason as every binary above it. Only title_nav.c is linked -- its header
# takes four plain booleans and returns two, so it needs nothing from net/ or gfx/ at all.
#
# The defect it exists for: backing out of the multiplayer screen set the screen to
# TITLE_SCR_MAIN and then FELL THROUGH into the world-entry gate twenty lines below, so B
# pressed on the one frame the block table finished syncing -- or the frame the 2000 ms sync
# deadline expired, anywhere in a wait the UI explicitly invites the player to give up on --
# left the screen AND entered the server's world, and main.c acts on the action.
#
# Sabotaged and measured, restored afterwards. Green arm: "title_nav self-test: PASS  47 checks".
#
#  * the fix reverted (`if (in.back) out.leave_to_main = true;` with no early return, i.e.
#    title.c's pre-fix two-unrelated-ifs shape) -> "title_nav self-test: FAIL 4/47", the four
#    being the sync-completes frame, the deadline-expires frame, the seed-arrives frame, and
#    the exhaustive 16-input sweep's mutual-exclusion check. Every FAIL line of the red run
#    was read, not just the first -- the CHECK macro in title_nav_test.c prints each one for
#    exactly that reason.
gcc -std=c11 -Wall -Wextra -Werror -O1 -g \
	-I source \
	source/scene/title_nav.c \
	source/scene/title_nav_test.c \
	-o "$BH/title_nav_test"

"./$BH/title_nav_test"

# app/session.c -- the per-session reset main.c runs on every lap of its session_start label,
# pulled out of main.c for the same reason title_nav.c was pulled out of title.c above: main.c
# includes <3ds.h> and cannot be linked here, and what was worth checking was a rule about
# lifetimes rather than anything drawable. The REAL world/registry.c is in the link, so the
# checks go through registryRemoteApply() -- the function both the join path and the
# single-player registry.bin path actually commit through -- and not a stand-in for it.
#
# The defect it exists for (v1.6.0, F2). registryFreeze() runs in genStart() for single player
# too, and registryInitCore() is the only thing that clears the frozen flag. Leaving a SERVER
# session reached it by accident (netDisconnect -> networldInit -> registryInitCore); leaving a
# SINGLE-PLAYER world did not, because `if (quit_to_title) goto session_start;` jumped back to
# the label and touched nothing. So boot -> Single Player -> Quit to title -> Multiplayer left
# the table frozen and still holding the single-player world's sidecar rows, and every
# BS_APP_REGISTRY_DEFS batch the server sent was refused by registryRemoteApply()'s
# `if (s_frozen) return 0;`. Player-visible as every server-defined block being an invisible
# hole for the whole session -- walk through walls, fall through floors -- cleared only by a
# reboot, which is what made it look intermittent.
#
# net/networld_test.c's test_registry_table_resets_between_sessions() PASSED against all of
# that, because it calls networldInit() itself. This binary does not: it walks boot, a real
# single-player session (rows, then registryFreeze()), the lap, and the join, with nothing but
# sessionBegin() in between -- and it reads source/main.c to check the call is wired into the
# lap at all, the same source-text technique world/atlas_uv_shader_test.c uses on the .pica
# files. Run from the repository root, which is where that relative path resolves.
#
# Measured. Written first against the UNFIXED tree (sessionBegin() empty, no call in main.c),
# which is the state the defect was reported in: "session self-test: FAIL 17/36", first
# "L162 !registryFrozen()". Green after the fix: "session self-test: PASS  36 checks".
#
# Then sabotaged and restored, both restores checksum-verified against the original md5 and a
# control run made green on both sides of each arm. Two arms, because there are two ways to
# break this and only one of them is in a linkable file:
#
#  * registryInitCore() removed from sessionBegin()          -> FAIL 14/36, first
#    "L162 !registryFrozen()". The behavioural half: the frozen flag, the stale row count,
#    the refused join batch, the wrong definition under id 0x80, the single-player-into-
#    single-player arm, and both halves of the every-exit-agrees comparison. The structural
#    checks stay green here, which is what says they are testing a different thing.
#  * `sessionBegin();` deleted from main.c's session_start lap -> FAIL 2/36, first
#    "L316 call != NULL". Only the two source-text checks go red -- and this is the arm that
#    matters most, because it is the exact failure net/networld_test.c's
#    test_registry_table_resets_between_sessions() could not see: every behavioural check in
#    this file still passes against a game in which nothing ever calls the reset.
#
# NOT recorded as a third arm, but measured by accident and worth keeping: the
# `call < title` check went red on its own during development because a comment written
# ABOVE the call happened to contain the text "runTitleScreen()". That check can fail, and it
# fails for the reason it exists -- a reset ordered after the menu instead of before it.
#
# Every FAIL line of both red runs was read, not just the first -- the CHECK macro in
# session_test.c prints each one for exactly that reason.
gcc -std=c11 -Wall -Wextra -Werror -O1 -g \
	-I source \
	source/app/session.c \
	source/world/registry.c \
	source/app/session_test.c \
	-o "$BH/session_test"

"./$BH/session_test"

# ---------------------------------------------------------------------------------------
# build/atlas.t3x, the one input to this suite that git does not carry (v1.8.3).
#
# The atlas stanza below decodes build/atlas.t3x and asserts on its texels — that is the half
# that checks the atlas's ART, and it is the half the v1.6.0 F7 defect lived in. But
# build/atlas.t3x is a devkitPro build product, .gitignore:3 matches build/, and it is
# therefore untracked. Two consequences, both measured on the tree as it stood before this
# guard existed, on a frozen snapshot (a parallel session's in-flight files restored to HEAD
# inside the snapshot only; the live tree was never touched):
#
#  1. A CLEAN CHECKOUT CANNOT PASS. With build/atlas.t3x removed:
#     "atlas uv shader self-test: FAIL 1/4089  L1087 cannot open build/atlas.t3x - a build
#     product (tex3ds, from gfx/atlas.t3s), not in git...". Note the TOTAL: 4089 against 4293
#     with the file present. The 204 art checks do not fail, they cease to exist, so the count
#     a reader compares against moves for a reason the failure line does not state.
#
#  2. WORSE: THE TEST VALIDATES WHATEVER t3x IS LYING AROUND. gfx/atlas.png patched — the sand
#     tile's red channel raised by 32, which is four RGBA5551 quantisation steps, so the change
#     survives the 5-bit channel (a one-step change does not; see the sand note in the atlas
#     stanza) — and build/atlas.t3x deliberately NOT regenerated:
#     "atlas uv shader self-test: PASS  4293 checks". Fully green, over an atlas that no longer
#     exists. For scale, the correct t3x for that PNG is 2045 bytes against the 2033 on disk.
#     This project has the lesson already: a diagnostic armed off disk inherits stale state.
#
# THE GUARD. tex3ds is deterministic — measured, two runs on the same input byte-identical, and
# byte-identical again to the build/atlas.t3x that `make` had produced minutes earlier — so the
# freshness question has an exact answer: rebuild the texture into this run's own scratch
# directory and compare it with the artefact. That is a CONTENT test, not a presence test and
# not an mtime test, and it needs no stamp file and no ritual anybody can forget.
#
# Rejected: having this script regenerate build/atlas.t3x in place. It is the Makefile's output
# and a parallel console build may be writing it at that moment — the same shared-artefact
# collision the BH="build-host/run-$$" comment at the top of this file exists for — and if the
# two tex3ds invocations ever disagreed, the console would ship one texture while this suite
# validated another. Rejected: a sha256 of gfx/atlas.png recorded in a stamp file beside the
# artefact. Only the Makefile knows when the t3x is genuinely fresh, the Makefile is not this
# script's to change, and a stamp this script writes itself can only ever bless whatever it
# already found. Rejected: comparing mtimes. mtime is not content: a checkout, a copy or a
# touch resets it, and `git archive` writes every file at the same instant.
#
# tex3ds is REQUIRED, not optional, and its absence is a loud failure rather than a skip. A
# skip is how the situation above arose in the first place. Under WSL the binary found is
# devkitPro's Windows tex3ds.exe through interop, and it is invoked from gfx/ with RELATIVE
# paths on purpose: a Windows executable cannot resolve a WSL absolute path, and passing one
# fails with "No such file or directory" while still exiting 0 on the tool's own terms.
ATLAS_T3X="build/atlas.t3x"
ATLAS_T3X_FRESH="$BH/atlas.expected.t3x"

TEX3DS=""
if command -v tex3ds >/dev/null 2>&1; then
	TEX3DS="tex3ds"
elif [ -n "$DEVKITPRO" ] && [ -x "$DEVKITPRO/tools/bin/tex3ds" ]; then
	TEX3DS="$DEVKITPRO/tools/bin/tex3ds"
elif [ -x /mnt/c/devkitPro/tools/bin/tex3ds.exe ]; then
	TEX3DS="/mnt/c/devkitPro/tools/bin/tex3ds.exe"
elif [ -x /c/devkitPro/tools/bin/tex3ds.exe ]; then
	TEX3DS="/c/devkitPro/tools/bin/tex3ds.exe"
fi

if [ -z "$TEX3DS" ]; then
	echo "atlas artefact guard: FAIL - tex3ds not found, so build/atlas.t3x cannot be checked" >&2
	echo "  the atlas stanza below decodes that file and asserts on its TEXELS. Without tex3ds" >&2
	echo "  this script cannot tell a fresh artefact from one left behind by an older build," >&2
	echo "  and it will not guess: a green run over a stale atlas is the failure being guarded" >&2
	echo "  against. Looked for: tex3ds on PATH, \$DEVKITPRO/tools/bin/tex3ds," >&2
	echo "  /mnt/c/devkitPro/tools/bin/tex3ds.exe, /c/devkitPro/tools/bin/tex3ds.exe." >&2
	echo "  Fix: install devkitPro, or set DEVKITPRO, then re-run." >&2
	exit 1
fi

if ! ( cd gfx && "$TEX3DS" -i atlas.t3s -o "../$ATLAS_T3X_FRESH" ) >/dev/null 2>&1; then
	echo "atlas artefact guard: FAIL - tex3ds ($TEX3DS) could not build gfx/atlas.t3s" >&2
	echo "  this script rebuilds the texture into $ATLAS_T3X_FRESH purely to COMPARE it with" >&2
	echo "  $ATLAS_T3X; it never writes into build/. Run it by hand to see the tool's own error:" >&2
	echo "    cd gfx && $TEX3DS -i atlas.t3s -o /tmp/atlas.t3x" >&2
	exit 1
fi

if [ ! -f "$ATLAS_T3X" ]; then
	echo "atlas artefact guard: FAIL - $ATLAS_T3X is MISSING" >&2
	echo "  it is a devkitPro build product (tex3ds, from gfx/atlas.t3s) and .gitignore:3" >&2
	echo "  matches build/, so a fresh clone never has it. The atlas stanza below decodes it" >&2
	echo "  and asserts on its texels; without it those ~204 art checks do not fail, they" >&2
	echo "  silently cease to exist, and the suite's total drops from 4293 to 4089." >&2
	echo "  Fix: run a console build from devkitPro's MSYS2 shell, from the repository root:" >&2
	echo "    /c/devkitPro/msys2/usr/bin/bash.exe -lc 'cd \"\$PWD\" && make'" >&2
	exit 1
fi

if ! cmp -s "$ATLAS_T3X" "$ATLAS_T3X_FRESH"; then
	echo "atlas artefact guard: FAIL - $ATLAS_T3X is STALE" >&2
	echo "  it is NOT what gfx/atlas.t3s and gfx/atlas.png produce today, so every texel check" >&2
	echo "  in the atlas stanza below would be asserting against an atlas the game no longer" >&2
	echo "  has. Rebuilt for comparison with: $TEX3DS" >&2
	echo "  on disk : $(wc -c < "$ATLAS_T3X") bytes" >&2
	echo "  expected: $(wc -c < "$ATLAS_T3X_FRESH") bytes" >&2
	echo "  Fix: run a console build from devkitPro's MSYS2 shell, from the repository root:" >&2
	echo "    /c/devkitPro/msys2/usr/bin/bash.exe -lc 'cd \"\$PWD\" && make'" >&2
	exit 1
fi

echo "atlas artefact guard: build/atlas.t3x matches a fresh tex3ds build of gfx/atlas.t3s"

# The whole atlas layout: both shaders' uvScale constants against source/world/atlas_uv.h,
# atlasRect()'s geometry, and gfx/atlas.png's own IHDR dimensions. The original guard existed
# because step 9.3c left world.v.pica at 1/256 after the atlas shrank from 256x256 to 64x64,
# so every tile's UV collapsed into one cell and the whole world rendered flat brown. A .pica
# file has no #include and the picasso shader compiler has no _Static_assert, so the
# world/block_tiles_check.c trick (duplicate the value on both sides, then _Static_assert the
# two copies agree) does not apply here; this test parses the shader source text directly.
#
# v1.6.0's strip atlas widened it. uvScale is two components now, because a 16x256 sheet does
# not divide u and v by the same number; world_dynamic.v.pica is checked as well as
# world.v.pica, which it never was (a New 3DS binds that program instead, so a drift there
# would be invisible on every Old 3DS the game was tested on); the PNG's own width and height
# are read straight out of its IHDR, closing the copy of the layout that lives in
# tools/make_atlas.py; and atlasRect() is proven to give every addressable tile a 16x16 rect
# inside the sheet that shares no TEXEL with any other, plus the U-repeat property greedy
# meshing depends on.
#
# block.c and registry.c are in the link because the coverage half asks what blockFaceTex()
# ACTUALLY returns for all 256 raw ids — the same range world/mesher.c's planBuild() builds
# its rect table over. A hand-copied tile table here would only be checking itself.
#
# v1.6.0 F7 added the PIXEL half, taking this binary from 2121 checks to 2166. Everything above
# it is geometry, and geometry cannot see whether a slot has any ART in it — which was the
# defect: tex 10..14 address real, addressable slots tools/make_atlas.py never painted, so they
# drew the sheet's near-black background fill, and an out-of-range tex clamped to slot 0 and
# drew grass. Neither is an error at any level a rect test can reach; both are a block with a
# texture on it. The registry lets a SERVER pick that tex, so a server misconfiguration
# presented as a Blocksmith rendering bug.
#
# The fix paints every unfilled slot with a magenta/black quadrant checker and clamps
# out-of-range ids onto ATLAS_TILE_MISSING (slot 14, reserved). This binary now DECODES
# build/atlas.t3x — LZ11, then the PICA200's 8x8-tile Morton swizzle — and asserts on texel
# values. NOTE: build/atlas.t3x is a build product and is gitignored, so this stanza needs a
# console build to have run ("make" from devkitPro MSYS2). Its absence is a loud FAIL here, not
# a skip: skipping would leave the atlas's art unchecked, which is where the defect lived. Since
# v1.8.3 that absence is caught EARLIER, by the artefact guard above this comment, which also
# catches the case this stanza cannot see at all: a t3x that opens perfectly well and is simply
# from a different build. The in-test failure stays as the backstop.
#
# The decoder was cross-checked against gfx/atlas.png with an independent Python decoder before
# any golden was pinned: all 4096 texels matched with no vertical flip and all 4096 mismatched
# with one, and tex3ds itself prints "Used lz11 for compression". The ten painted slots'
# fingerprints were taken from the t3x built BEFORE the generator was touched and are unchanged
# after it. Their per-tile SHA-256 over the PNG's own pixels is identical before and after as
# well (slots 0..9); slots 10..15 went from e9ab7c89..adf62a766, the flat fill, to
# e387583a..e70bb15d, the marker.
#
# Sabotaged and measured, every arm restored afterwards and the restore confirmed by md5
# (atlas_uv.h back to 2e3a4f3a7357c05a14783e0d5d7bc082, make_atlas.py to
# b9e5c9c09cbe2be6ddd00b00b7aec582, block.h to aff3661ac20bf3fbbe57064c5ba75a3b, atlas.png to
# 6174f5ced0adc9d6f5bf4fd58e73b4d5, atlas.t3x to 953039074678148db6012e674a4252cd). Green arm
# before and after the run: "PASS 2166 checks". Red-before, with the marker painted but the
# clamp not yet changed: FAIL 18/2166.
#
#  * atlasRect()'s clamp put back to `tile = 0`  -> FAIL 6/2166, first "atlasRect(-1) gave u
#    0..16 v 0..16". All six out-of-range indices, INCLUDING 16 — under the old clamp-to-0 that
#    index could not go red at all (16*16 wraps to exactly 0, so the wrapped rect IS the clamped
#    rect), and clamping to slot 14 gives it a distinct answer. Every pixel check stayed green,
#    which is the control.
#  * ATLAS_TILE_MISSING moved to ATLAS_TILE_COUNT-2 -> FAIL 1/2166, "ATLAS_TILE_MISSING=13 is
#    not the TOP addressable slot". The six clamp checks stay GREEN because they follow the
#    constant; that one check owns the CHOICE of slot, and this is what proves it does.
#  * BTEX_PLANKS pointed at 14                   -> FAIL 1/2166, "6 block faces map to tile 14,
#    which is ATLAS_TILE_MISSING - first block 7 face 0".
#  * make_atlas.py's marker pre-fill deleted (the pre-F7 sheet, rebuilt through tex3ds)
#    -> FAIL 12/2166: six "slot N is not the missing-texture marker: 256 of 256 texels differ
#    ... 0x1887" and six "slot N is a solid block of the sheet's background fill (0x1887)". The
#    ten painted fingerprints stay GREEN — the art is untouched by the pre-fill.
#  * tile_missing() painted FLAT magenta instead of quadrants -> FAIL 6/2166, "128 of 256 texels
#    differ, first at tile-local (8,0)". The background-fill checks stay GREEN, which is the
#    control: the texel check discriminates the PATTERN, not merely "anything but fill".
#  * one sand shade moved 132 -> 164             -> FAIL 1/2166, "slot 4's art changed". Only
#    slot 4: swapping a colour consumes the same number of draws from the seeded stream, so the
#    other nine tiles do not move, and that is the control.
#  * slot_png_y()'s v flip reversed              -> FAIL 16/2166, all ten fingerprints plus all
#    six marker slots. Slot 0 came back as 0xAB5D4C84CED60325, which IS the marker's
#    fingerprint — the reversed flip put the marker where tile 0 belongs.
#
# Every FAIL line of every red run above was read, not just the first.
#
# NOT recorded as an arm, for the reason the meshq note at the top of this file gives. The sand
# sabotage was first attempted as 132 -> 133 and the suite PASSED: 132 >> 3 and 133 >> 3 are
# both 16, so a one-step change in an 8-bit channel does not survive RGBA5551's five bits, and
# the fingerprint — taken over the DECODED texels, i.e. what the GPU actually receives — cannot
# see it. A sabotage the format swallows proves nothing. 164 crosses the quantisation step and
# goes red. Worth knowing either way: this pin cannot catch a sub-quantisation art edit.
#
# Sixth binary for the same reason ui_layout_test is a fifth one: it carries its own main().
gcc -std=c11 -Wall -Wextra -Werror -O1 -g \
	-I source \
	source/world/block.c \
	source/world/registry.c \
	source/world/atlas_uv_shader_test.c \
	-o "$BH/atlas_uv_shader_test"

"./$BH/atlas_uv_shader_test"

# The updater's version-parse/compare/asset-naming seam (source/app/updater_version.c).
# Pulled out of source/app/updater.c into its own pure-C file for exactly this reason: the
# updater itself needs libcurl, AM and RomFs, none of which exist on this host, but the tag
# parsing and comparison it depends on has no <3ds.h> in it at all and can be proven here.
# Seventh binary for the same reason atlas_uv_shader_test is a sixth one: it carries its own
# main().
gcc -std=c11 -Wall -Wextra -Werror -O1 -g \
	-I source \
	source/app/updater_version.c \
	source/app/updater_version_test.c \
	-o "$BH/updater_version_test"

"./$BH/updater_version_test"

# The loading screen's state machine (source/scene/loading.c). Eighth binary, own main(),
# same reason as every one above it. This one is not optional cover: the code it replaced —
# genWaitSpawnColumn's bare spin on the worker, with no aptMainLoop() in it — hung a real
# console hard enough to take the HOME button with it, and the property that makes the
# replacement safe ("every wait ends, in every phase, whatever the world does") is a claim
# about input sequences, not something the frame loop can be read for.
gcc -std=c11 -Wall -Wextra -Werror -O1 -g \
	-I source \
	source/scene/loading.c \
	source/scene/loading_test.c \
	-o "$BH/loading_test"

"./$BH/loading_test"

# net/blockdiff.c's pending-diff store. Ninth binary, own main(), same reason as every one
# above it. This one had a test file sitting in the tree that the suite never built, so the
# store's 256-slot-to-65536-slot rewrite — which replaced a linear scan with hash chaining
# and a free list, and is what a join sync's whole payload now lands in — had cover written
# for it that had never once run. A test nobody executes is not cover, it is a comment.
gcc -std=c11 -Wall -Wextra -Werror -O1 -g 	-I source 	source/net/blockdiff.c 	source/net/blockdiff_test.c 	-o "$BH/blockdiff_test"

"./$BH/blockdiff_test"

# net/networld.c, the client half of the multiplayer world. Tenth binary, own main(). Like
# blockdiff_test above it, this had a Makefile of its own (source/net/Makefile.networld-test)
# that had to be remembered and run by hand, which is the same thing as not running: the
# per-chunk subscription work landed with tests nobody's default command executed. Both
# Makefiles stay for their finer warning set; this line is what makes the suite the single
# thing that has to pass. -I deps/blocksmith-server is the wire protocol header the client
# and the server share, which is the whole point of it living in the server repo.
#
# That "stays for its finer warning set" had quietly stopped being true: Makefile.networld-test
# named six objects while this suite had grown to need ten, so running it by hand died with
# roughly 25 undefined references while its own header still advertised it as the way to run
# the tests. Repaired in v1.6.0 task 8b rather than deleted — the strict set (-Wconversion,
# -Wsign-conversion, -Wcast-qual, -Wshadow and the rest) is real coverage this line does not
# have, on exactly the file that parses attacker-controlled bytes. Its link line is now this
# one's file list in the same order, so a future divergence is visible by reading the two side
# by side. Keep them in step when a file is added here.
#
# -DBS_CLIENT_HAS_METERS=1 flips this build's copy of net/networld.h's compile-honest meter
# gate so test_send_player_report_encodes() can reach the PLAYER_REPORT encoder at all: in
# every other build (console included) the gate is 0 and the encoder does not exist, because
# sending zeros would wipe a returning player's saved state — see that header. Flipping it
# HERE and only here is what lets the encode path stay tested while production keeps it off.
#
# v1.6.0 task 8 put the block-registry WIRING under this binary. Phase A shipped it compiling,
# green and inert: the audit found it could never commit a dynamic row at all. Four scenarios
# were added (registry: retry / silent server / sync gate / session reset), taking this binary
# from 251 checks to 286. They link the REAL net/networld.c and the REAL world/registry.c —
# the only doubles are this file's own fake transport and fake clock, both link-time stand-ins
# for code that cannot exist on a host, and neither one carries a copy of a decision
# networld.c makes.
#
# All four defects sabotaged in the real code and measured, each restored afterwards. Green
# arm throughout: "PASS 286 checks, 0 failed".
#
#  * retry deleted (`if (1) return;` at the top of registryFetchTick(), i.e. Phase A's
#    one-FETCH-per-session behaviour) -> "FAIL 286 checks, 4 failed", first
#    "past the interval exactly one retry goes out, still asking from 0x80".
#  * the retry's `if (!s_sent_fetch) return;` removed — the old-server invariant, which the
#    new sender is a new way to break -> "FAIL 286 checks, 2 failed", first
#    "ten seconds of pumping a pre-registry server sends nothing at all".
#  * networldRegistryWaiting() forced to return false — s_reg_synced back to gating nothing,
#    which is what let registryFreeze() beat the DEFS to the table -> "FAIL 286 checks,
#    4 failed", first "the seed alone holds entry while INFO could still land".
#  * registryInitCore() dropped from networldInit() -> "FAIL 286 checks, 5 failed", first
#    "after leaving, only the eight core rows remain".
#
# Every FAIL line of each red run was read, not just the first: no case stayed green that
# should have gone red, and the deliberate control check
# ("control: the same pump DOES retry after a real INFO") stays green in the two arms it is
# a control for and goes red in the one that removes the retry, which is what makes the
# silent-server checks mean something rather than passing because nothing was wired at all.
#
# v1.6.0 task 8b closed the hole those 286 checks were all green around. s_reg_synced was set
# by the LAST flag of a BS_APP_REGISTRY_DEFS batch, and the server sends an empty terminating
# batch unconditionally (deps/blocksmith-server/game/bsgame.c) — so four bytes carrying no
# records, checked against nothing and refused by no frozen table, declared the sync complete.
# A client whose data batch was lost in the UDP disarmed its own retry, released the entry
# gate, and rendered every server-defined block as an air hole for the session with no
# degraded marker anywhere. It now means "the table reproduces the INFO's rev+count+crc16",
# rechecked after every batch. Two scenarios added (registry: terminator / fingerprint),
# taking this binary from 286 checks to 318.
#
# Sabotaged in the real net/networld.c and measured, each restored afterwards and the file
# confirmed byte-identical by md5sum. Green arm: "PASS 318 checks, 0 failed".
#
#  * the old line restored (`if (msg[3] & 0x01u) s_reg_synced = true;` in place of
#    `s_reg_synced = registryMatchesInfo();`) -> 13 failed, first "and a table that cannot
#    reproduce the server's crc16 is not a synced table".
#  * registryMatchesInfo()'s crc16 term dropped -> 10 failed, first "the mismatch draws one
#    FETCH asking from the dyn base 0x80" — i.e. it also stops the INFO comparison working,
#    which is the point: one function answers the question for both callers now.
#  * registryMatchesInfo()'s `if (!s_have_registry_info) return false;` flipped to `return
#    true` -> 2 failed, "but with no INFO there is no fingerprint, so nothing is verified"
#    and "and its terminator settles nothing either". That arm found NOTHING on its first
#    run — the guard had no cover at all — and those two checks were written because of it.
#
# Every FAIL line of each of those runs was read as well. The control inside the fingerprint
# scenario ("all three declared rows landed" / "the table reproduces the server's whole
# fingerprint") stays green in the arms it is a control for, so the negatives below it are
# not passing because the check simply never says yes.
#
# v1.6.0 task 11 (greedy u-merging in world/mesher.c) then landed three failures in
# test_mesher_tables_after_register() here, and they were a real behaviour change rather than
# a regression: out.faces stopped meaning "visible block faces" and became "quads it took to
# cover them" (a full chunk of one solid block is now 384 quads, not 1536 — the run caps at
# ATLAS_MAX_MERGE_BLOCKS=15, so a 16-long row costs two), and a merged quad's u1 is
# deliberately stretched past its own tile's u1 because u is sampled GPU_REPEAT. Rather than
# pin the new number — which would bake the merge policy into a test about the REGISTRY's
# rect table — both counts were restated in block faces via this file's own meshFaceCells(),
# the number merging cannot move, and the UV check now allows u at any whole-tile step from
# the tile's u0 up to the merge ceiling while keeping the v half exactly as strict as it was.
# Check count unchanged at 318; world/world_test.c owns the merge algorithm itself.
#
# Both rewritten claims re-proved able to go red, sabotaging only this file's fixture:
#
#  * chunkClear(c, id) -> chunkClear(c, 1), i.e. fill with core grass instead of the probe
#    block, so the mesh carries tiles the probe def never declared -> "FAIL 318 checks,
#    1 failed", exactly "every vertex UV corner comes from the def's own tiles". The two
#    count checks stay GREEN, which is the control: face count does not depend on which
#    solid block fills the chunk, so this arm proves the UV check discriminates rather than
#    the fixture merely breaking everything.
#  * chunkClear(c, id) -> chunkClear(c, 0), i.e. air, so nothing meshes -> "FAIL 318 checks,
#    2 failed", both "a full chunk of the new solid block emits every face" and "after
#    invalidation the next meshChunk rebuilds the same geometry". The UV check stays green
#    there, vacuously, over zero vertices — noted so nobody later reads that as coverage.
#
# Restored after each arm, file confirmed byte-identical by md5sum 8ec65ed2b566be8cc4e5ec06206042bf.
#
# v1.6.0 F2 added source/app/session.c to this link and
# test_registry_join_after_a_single_player_quit_to_title() to networld_test.c. The scenario
# right above it, test_registry_table_resets_between_sessions(), passed against the broken tree
# because it calls networldInit() itself; the new one calls only sessionBegin(), which is all
# main.c runs between a single-player world and the next join. Count 318 -> 326.
#
# Sabotaged and measured, restored (md5 verified, control green on both sides): registryInitCore()
# removed from sessionBegin() -> "FAIL 326 checks, 5 failed", the five being the committed row,
# the row count, the definition sitting under id 0x80, networldRegistrySynced() and
# networldRegistryWaiting(). The scenario's own three control checks — the sidecar row landing,
# genStart()'s freeze, and the FETCH going out — stay GREEN in that arm, which is what says the
# five discriminate rather than the fixture merely collapsing.
gcc -std=c11 -Wall -Wextra -Werror -O1 -g -DBS_CLIENT_HAS_METERS=1	-I source -I deps/blocksmith-server 	source/world/world.c 	source/world/block.c 	source/world/registry.c 	source/world/chunk.c 	source/world/budget.c 	source/net/blockdiff.c 	source/net/networld.c 	source/world/mesher.c 	source/world/visgraph.c 	source/world/scratch.c 	source/app/session.c 	source/net/networld_test.c 	-o "$BH/networld_test"

"./$BH/networld_test"

# net/inv_bridge.c, the seam between world/inventory.h and the wire (v1.3.0). Eleventh binary,
# own main(), same reason as every one above it. It links the REAL inventory.c and crafting.c
# alongside the real networld.c, because the thing under test is precisely the arithmetic
# BETWEEN them: "how many units actually moved" is a number only inventory.c can produce and
# only networld.c can encode, so a test that faked either half would be checking its own
# stand-in rather than the seam. crc32.c is in the link for the same reason inventory_test
# above needs it — the inventory save file is checksummed the same way a region file is.
gcc -std=c11 -Wall -Wextra -Werror -O1 -g \
	-I source -I deps/blocksmith-server \
	source/world/world.c \
	source/world/block.c \
	source/world/registry.c \
	source/world/chunk.c \
	source/world/budget.c \
	source/world/inventory.c \
	source/world/crafting.c \
	source/world/crc32.c \
	source/net/blockdiff.c \
	source/net/networld.c \
	source/world/mesher.c \
	source/world/visgraph.c \
	source/world/scratch.c \
	source/net/inv_bridge.c \
	source/net/inv_bridge_test.c \
	-o "$BH/inv_bridge_test"

"./$BH/inv_bridge_test"

# sleep.c's lid-close behaviour. Twelfth binary, own main(). The REAL source/app/sleep.c is
# in the link — its aptHook and svcGetSystemTick half sits behind an #ifdef __3DS__ (same
# split as app/battery.c below and app/debugmenu_ui.c), so the whole of what closing the lid
# actually does compiles here with nothing stubbed and nothing faked.
#
# It was not always. Until v1.6.0 this stanza compiled tests/sleep_test.c alone, and that
# file carried a private testSleepEnter()/testSleepShouldSkip() hand-copy of the flag machine
# — this script's own comment said so, in as many words: "tests/sleep_test.c's own
# miniaturised logic". Measured: with source/app/sleep.c deleted outright it still printed
# "sleep state machine: PASS / 13 checks". Linking the real file is the entire fix, and the
# v1.6.0 rewrite is what gives it something worth linking — the time-capped flush and the
# network leave that now happen in the APT hook.
#
# Both halves sabotaged, both measured. Making sleepFlushBounded() read its budget once at
# the top instead of between columns: old test PASS 13 checks, new test FAILED 7 of 34,
# first "L144 saved == 3". Dropping the leave hook call from sleepOnSleep(): old test PASS
# 13 checks, new test FAILED 4 of 34, first "L217 s_leave_calls == 1".
gcc -std=c11 -Wall -Wextra -Werror -O1 -g \
	-I source \
	source/app/sleep.c \
	tests/sleep_test.c \
	-o "$BH/sleep_test"

"./$BH/sleep_test"

# battery.c's bar mapping, low-battery and unknown-state logic. Thirteenth binary, own
# main(). The REAL source/app/battery.c is in the link — its PTM:U and drawing half sits
# behind an #ifdef __3DS__ (same split as app/debugmenu_ui.c), so the pure half compiles here
# with nothing stubbed and nothing faked.
#
# It was not always. Until v1.6.0 this stanza compiled tests/battery_test.c alone, and that
# file carried private testBars()/testLow() hand-copies of the arithmetic — so its 16 checks
# passed no matter what battery.c did. Measured: changing batteryBars() to return 99 for
# levels 3-4 still printed "battery bar mapping: PASS / 16 checks". Linking the real file is
# the entire fix; the same sabotage now fails 3 of 25 checks.
gcc -std=c11 -Wall -Wextra -Werror -O1 -g \
	-I source \
	source/app/battery.c \
	tests/battery_test.c \
	-o "$BH/battery_test"

"./$BH/battery_test"

# interop_test.c, source/net's real client (networld.c) speaking to a REAL bsgame daemon over a
# real Unix socket — see that file's own header comment for why networld_test.c and bsgame_test.c
# (deps/blocksmith-server/game/) each passing on their own proves nothing about whether the two
# processes agree with each other, only with themselves. `make -C .../game bsgame` first, so the
# binary interop_test spawns is freshly built from what is actually checked out here rather than
# whatever a previous session last built by hand — a stale daemon would make this suite validate
# nothing. Eleventh binary, own main(), same reason as every one above it.
# Skipped on Windows/MSYS2 — poll.h and sys/un.h are Unix-only.
case "$(uname -s)" in
MINGW*|MSYS*|CYGWIN*) echo "skipping interop_test on Windows (Unix sockets unavailable)" ;;
*)
make -C deps/blocksmith-server/game bsgame

gcc -std=c11 -Wall -Wextra -Werror -O1 -g 	-I source -I deps/blocksmith-server 	source/world/world.c 	source/world/block.c 	source/world/registry.c 	source/world/chunk.c 	source/world/budget.c 	source/net/blockdiff.c 	source/net/networld.c 	source/world/mesher.c 	source/world/visgraph.c 	source/world/scratch.c 	source/net/interop_test.c 	-o "$BH/interop_test"

"./$BH/interop_test"
;;
esac

# The minimap_test stanza that stood here is gone with the feature. scene/minimap.c,
# minimap.h and minimap_test.c were deleted in v1.6.0: minimap.c:251 called worldGet(NULL,
# wx, 0, wz) and world.c:44 dereferences w->slots[i] with no NULL guard, so the first pixel
# the minimap ever drew was a data abort on a real console. Repair was rejected in favour of
# removal — the spec's touch-screen map gets built fresh later — so its 16606 passing checks
# went with it rather than being kept green over code nothing calls. Measured before: 16606
# checks in this stanza; after: the stanza does not exist and the suite is 13 binaries.
#
# debugmenu.c registry logic + options.c debug_menu persistence, plus (v1.6.0) the input
# half of app/debugmenu_ui.c, which debugmenu_test.c #includes as source — see that file.
# Own main(), same reason as every one above it. Registry is pure data-structure work; the
# options half reuses options.c to prove the new key round-trips; the UI half proves the
# frame-input-reuse fix, which is a claim about a sequence of frames that no emulator run
# can make. Measured before the fix: FAIL 4/74, first failure "L268 s_slider_val == 3" —
# the A press that opened the menu also stepped the render-distance slider. After: PASS 80.
gcc -std=c11 -Wall -Wextra -Werror -O1 -g \
	-I source \
	source/app/options.c \
	source/app/debugmenu.c \
	source/app/debugmenu_test.c \
	-o "$BH/debugmenu_test"

"./$BH/debugmenu_test"

# app/whatsnew.c — the release-notes parser, word wrap, scroll clamp, scrollbar geometry and
# held-to-repeat timing behind v1.6.0 task 14b (the changelog the update screen shows on the
# TOP screen before the player commits to a download). Own main(), same reason as every
# binary above it.
#
# The REAL source/app/whatsnew.c is in the link with nothing stubbed, because that file has
# no <3ds.h> in it to stub around — it was split out of scene/title.c for exactly this, the
# same arrangement app/battery.c and app/debugmenu_ui.c use and for the same recorded reason
# (both of those once carried hand-copied test logic that stayed green with the real module
# deleted). The fetch lives in app/updater.c and the drawing in scene/title.c; neither makes
# a decision this binary does not already check.
#
# Seventeen sabotages, every one applied to the REAL source/app/whatsnew.c, measured, and
# restored (the file's sha256 was compared back to the original after each). Green arm before
# and after the whole run: "whatsnew notes self-test: PASS  463 checks". The check TOTAL moves
# in some arms because a sabotage can change how many CHECKs a loop executes; that is read
# alongside the failure count rather than instead of it.
#
#  * '\r' dropped from the trailing trim (CRLF files)   -> FAIL 5/463,  first "L130 !wn.malformed"
#  * unprintable bytes copied through instead of '?'    -> FAIL 9/453,  first "L276 !strcmp(wn.items[0].text, \"??? ??? hidden ?\")"
#  * WHATSNEW_BYTES_MAX no longer limits the parse      -> FAIL 2/465,  first "L254 wn.fix_count == 0"
#  * item cap stops reporting itself (truncated unset)  -> FAIL 1/463,  first "L235 wn.truncated"
#  * over-long item: no "..." and no truncation flag    -> FAIL 4/463,  first "L203 wn.truncated"
#  * item with no section dropped without malformed     -> FAIL 1/463,  first "L142 wn.malformed"
#  * word longer than the column emitted whole          -> FAIL 2/401,  first "L372 (int)strlen(l.lines[i]) <= cols"
#  * wrap width from a guessed 4px advance              -> FAIL 26/423, first "L347 (int)strlen(l.lines[i]) <= cols"
#  * scroll clamp: bottom end stops clamping            -> FAIL 6/463,  first "L435 whatsnewClampScroll(5, 10, 21) == 0"
#  * maxScroll off by one (exactly-full view scrolls)   -> FAIL 7/465,  first "L439 whatsnewMaxScroll(21, 21) == 0"
#  * thumb positioned over the whole track             -> FAIL 15/463, first "L496 t.y == track - t.h"
#  * thumb minimum height removed                       -> FAIL 2/463,  first "L507 t.h == 12"
#  * everything-fits thumb collapses instead of filling -> FAIL 3/463,  first "L469 t.h == 192"
#  * hold-before-repeat delay removed                   -> FAIL 6/463,  first "L539 whatsnewRepeatStep(&r, true) == 0"
#  * releasing no longer rearms the press step          -> FAIL 1/463,  first "L550 whatsnewRepeatStep(&r, true) == 1"
#  * layout line cap drops content without saying so    -> FAIL 1/463,  first "L422 l.truncated"
#  * no placeholder line when there are no notes        -> FAIL 5/463,  first "L296 l.count == 1"
#
# The last of those found a real defect rather than confirming a known-good line: wrapInto()
# originally returned at the line cap without setting `truncated`, so a changelog cut on its
# final item was silently shortened and the "(these notes were too long to show in full)"
# footnote never appeared. The test went red on the shipping code before the fix existed.
gcc -std=c11 -Wall -Wextra -Werror -O1 -g \
	-I source \
	source/app/whatsnew.c \
	tests/whatsnew_test.c \
	-o "$BH/whatsnew_test"

"./$BH/whatsnew_test"

# scene/interact.c — the break/place edit path. Own main(), same reason as every binary above
# it. The REAL scene/interact.c is in the link: v1.6.0 put its Camera-taking interactAim and
# the libctru key constants behind an #ifdef __3DS__ (same split as app/battery.c and
# app/sleep.c) precisely so this stanza could exist, because the defect it was written for is
# an ORDERING bug and no reimplementation of the order can be red against it.
#
# The defect (v1.6.0 F3): interactEdit's break path called worldSet(..., BLOCK_AIR) FIRST and
# only afterwards did main.c offer the id to invBridgeAdd, which refused it — world/inventory.c
# and net/inv_bridge.c both gate on BLOCK_COUNT, and a server-registered dynamic block lives at
# 0x80..0xFD. So placing a server block worked, mining one deleted it: not in the world, not in
# the bag, no message. The fix asks world/inventory.h's new inventoryCanHold() BEFORE anything
# is mutated, which also stops the BS_APP_BLOCK_EDIT going out — the server honours the removal
# and drops the matching BS_INV_OP_PICKUP over the same ceiling
# (deps/blocksmith-server/game/bsgame.c), so sending it would destroy the block for everyone.
#
# Two symbols the host cannot supply, both stubbed in the LINK and never inside interact.c:
# tests/interact_stub.c gives chunkRenderTouch (citro3d), and interact_test.c itself gives
# networldSendBlockEdit as a recording spy, because "no block edit went out" is one of the
# assertions. Neither makes a decision. tests/net_stub.c is here for world.c's
# networldOnColumnLoad, exactly as in the world stanza at the top.
#
# Red-before, on the break path put back verbatim to its pre-fix shape (the guard branch
# deleted, so worldSet runs first) — the proof the defect was real, not a hypothesis:
#
#   "interact self-test: FAIL 9/80  L173 worldGet(&s_world, TX, TY, TZ) == dyn", the nine
#   being, in order: the world cell (twice, once per break case), broke_id, broke, refused,
#   s_edits_sent, and the three repeat-press equivalents. Every one was read. The three
#   control cases at the bottom of the file — aiming at nothing, empty hand, placing into
#   the player's own body — stayed GREEN in that arm, so the red is about the guard and not
#   about the binary being broken. interact.c md5 f2ec801545c50800f840f6a5c3e653cf before
#   and after.
#
# Four further sabotages, each of the REAL shipping code, each restored and the restore
# confirmed by md5 (interact.c back to f2ec801545c50800f840f6a5c3e653cf, inventory.h back to
# db240cd41b9bb41a100bc8112f004fbd). Green arm before and after the run: "PASS 80 checks".
#
#  * inventoryCanHold's ceiling widened to 254 (world/inventory.h)   -> FAIL 12/80, first
#    "L144 !inventoryCanHold((ItemId)BLOCK_COUNT)". Both the ceiling's own truth table and
#    every dynamic-break check go red together, which is what says they are the same rule.
#  * the guard made over-broad, also refusing BLOCK_STONE              -> FAIL 11/80, first
#    "L221 worldGet(&s_world, TX, TY, TZ) == BLOCK_AIR". This is the check that the
#    "core blocks still break" cases can actually fail.
#  * networldSendBlockEdit dropped from the accepted break path       -> FAIL 10/80, first
#    "L225 s_edits_sent == 1". This is the check that the wire spy can fail — without it,
#    "no edit was sent for a dynamic block" would pass for a binary that never sends at all.
#  * the guard applied to the PLACE path as well (it->holding)        -> FAIL 6/80, first
#    "L285 worldGet(&s_world, TX, TY + 1, TZ) == dyn2". Placing a server block must keep
#    working; a blanket "dynamic ids are inert" passes every other case in the file.
#
# NOT recorded as an arm, for the reason the meshq note at the top of this file gives: the
# ceiling was first widened to 256, and gcc rejected it outright with
# "error: comparison is always true due to limited range of data type [-Werror=type-limits]"
# (ItemId is a uint8_t), GCC_EXIT=1. A sabotage that does not compile proves nothing. 254 is
# inside the type and still admits both ends of the dynamic range.
gcc -std=c11 -Wall -Wextra -Werror -O1 -g \
	-I source \
	source/world/world.c \
	source/world/block.c \
	source/world/registry.c \
	source/world/chunk.c \
	source/world/budget.c \
	source/app/options.c \
	source/app/input_map.c \
	source/world/mining.c \
	source/scene/interact.c \
	source/scene/interact_test.c \
	tests/interact_stub.c \
	tests/net_stub.c \
	-o "$BH/interact_test"

"./$BH/interact_test"

# Only reached if every binary above exited 0 (set -e stops the script on the first
# non-zero exit), so a failed run's directory is left behind for inspection rather than
# silently deleted along with the evidence of what failed.
rm -rf "$BH"

# world/playerpose.c — the single-player pose sidecar (v1.7.1 task 46b). Own binary, own
# main(), same reason as every one above it: a save format with eight distinct ways to be
# wrong and one right answer for all of them is a plain-C contract provable here in a
# second, and a broken pose file must not stop the rest of the suite from running.
#
# Appended BELOW the rm -rf above rather than inserted in front of it, with a build
# directory of its own, and that is deliberate rather than careless: this landed while
# another session was appending stanzas to this same file, and a strictly-append edit is
# the one shape that cannot silently drop somebody else's. It costs nothing — set -e still
# stops the script here on a non-zero exit, and this stanza cleans up after itself. Merge
# it back into the block above next time this file is quiet.
#
# The link is wider than playerpose.c because playerPoseUnstick() calls the REAL
# worldStandingY(): the un-stick is the half of task 46b whose bug — a restored player
# buried inside blocks that were filled in after they logged out — is task 46's bug in a new
# costume, and it is invisible against a copy of the function. world.c drags in block.c,
# registry.c, chunk.c and budget.c, and tests/net_stub.c supplies networldOnColumnLoad, the
# same set the interact stanza links for the same reason. -lm is for isfinite/floorf.
#
# source/world/Makefile.playerpose-test builds the same file list under the stricter warning
# set (-Wconversion, -Wsign-conversion, -Wcast-qual, -Wshadow), the way
# source/net/Makefile.blockdiff-test does for blockdiff_test. This line is what makes the
# suite the single thing that has to pass; keep the two file lists in step.
#
# ── The residency question this task turned on, and how it was settled ─────────────────
#
# genStart() centres the streaming ring, runLoadingScreen() fills exactly that ring, and
# worldGet() answers BLOCK_AIR for a chunk that is not resident (world.c:196). So
# worldStandingY() over an absent column returns the y it was given and silently does
# nothing. main.c:2949 used to read `genStart(0, 0)` — the ring centred on the spawn column
# — so a pose restored anywhere but spawn would have been un-stuck against air and the
# player would have come back inside their own blocks, with a green suite over it. The fix
# is part of this task: the pose is loaded BEFORE genStart and the ring is centred on its
# column, so the loading screen has filled it by the time the un-stick runs. Asserted by
# text in testMainCIsWired(), because main.c cannot be linked here.
#
# ── Sabotages, every one applied to the REAL shipping code, each restored afterwards and
#    the restore confirmed by md5 (playerpose.c back to f7996574e568f6cd512252b3f5a2ab0e,
#    main.c back to a792b829ed5c49359ef4ad913c1e77c7 every time). Green arm before and after
#    every run: "playerpose self-test: PASS 108 checks, 0 failed".
#
# The cross-arm control is testMainCIsWired(): it shares no code with playerpose.c and
# stayed green through all twelve sabotages of that file, so a red run there is about the
# sabotage and not about the binary having fallen over.
#
#  * the crc COMPARISON dropped           -> FAIL 2/108, "refused: one payload bit flipped".
#    Exactly two, nothing else — which is what says the checksum check is the thing being
#    tested rather than the writer. Narrowing the crc RANGE instead was tried first and is
#    NOT recorded as that arm: it makes every hand-built file refuse, so the corrupt case
#    stayed green for the wrong reason (FAIL 5/108, first "L193 memcmp(got, want) == 0").
#  * magic comparison dropped             -> FAIL 2/108, "refused: magic BSI1".
#  * version comparison dropped           -> FAIL 2/108, "refused: version 2".
#  * length check dropped                 -> FAIL 2/108, "refused: 33 bytes". The 31-byte
#    case stays GREEN here: a truncated file loses payload the crc covers, so the checksum
#    catches it a line later. It goes red only with the length check AND the crc comparison
#    both gone -> FAIL 6/108, first "refused: 31 bytes". Recorded because it says which of
#    the two length checks stands on its own.
#  * the isfinite loop removed            -> FAIL 8/108, first "refused: x NaN". Four of the
#    five cases; "y +inf" stays green because the y range check catches it too. This is the
#    check that stands between a corrupt file and the void: a NaN reaching bodyInit()
#    propagates through every physics comparison as false and the player falls forever.
#  * y bound widened to <= WORLD_HEIGHT   -> FAIL 1/108, "y=128.0 expected refused", with
#    the y=127 control green in the same run.
#  * a missing file reported as loaded    -> FAIL 1/108, "refused: no player.dat at all".
#  * the recover call removed             -> FAIL 5/108, first "L367 playerPoseLoad(...)".
#  * recover promotes the tmp always      -> FAIL 1/108, "L386 poseBitsEqual(&out, &real)".
#  * the !out guard dropped               -> SEGFAULT, BIN_EXIT=139. Armed only after the
#    check was moved to run against a directory that really holds a pose file: against an
#    empty one the load returns false at the fopen and the arm stayed green at 108/108.
#  * the !pose guard dropped              -> SEGFAULT, BIN_EXIT=139.
#  * un-stick uses a truncating cast      -> FAIL 1/108, "L461 bits(pose.y) == bits(42.0f)",
#    the negative-coordinate case only, which is the whole point of having it.
#  * un-stick applied to x instead of y   -> FAIL 4/108, first "L416 bits(pose.y) ==
#    bits(42.0f)".
#  * writer swaps the y and z offsets     -> FAIL 9/108, first "L210 poseBitsEqual(...)",
#    including "L193 memcmp(got, want)" — the check that the file on disk is the layout the
#    header documents, not merely one the reader agrees with.
#  * main.c: genStart's centre taken off the pose column -> FAIL 3/108, first "L551
#    strstr(src, \"genColumnOf(saved_pose.x)\") != NULL". THIS is the residency arm.
#  * main.c: the quit-path playerPoseSave deleted -> FAIL 3/108, first "L575 countOf(src,
#    \"playerPoseSave(\") >= 2", and every behavioural check above stayed green. That is the
#    roadmap task 12 / 20c failure class caught on purpose.
#  * main.c: the lid-close playerPoseSave deleted -> FAIL 2/108, discriminating from the one
#    above by which ordering check goes with it.
#  * main.c: the un-stick deleted from the restore path -> FAIL 3/108, first "L533 unstick
#    != NULL". Task 46's bug in a new costume, caught by text.
#
# NOT recorded as an arm: the NULL and EMPTY world_dir checks. With dirUsable() removed they
# still pass 108/108, because "%s/player.dat" against NULL or "" resolves to a path this
# process cannot open and the save fails one layer down. They document intent — a server
# session passes NULL here so this console gains no file from a world it does not own — and
# playerpose_test.c says so above them rather than letting them look like proof.
BHP="build-host/run-$$-playerpose"
mkdir -p "$BHP"

gcc -std=c11 -Wall -Wextra -Werror -O1 -g \
	-I source \
	source/world/playerpose.c \
	source/world/world.c \
	source/world/block.c \
	source/world/registry.c \
	source/world/chunk.c \
	source/world/budget.c \
	source/world/crc32.c \
	tests/net_stub.c \
	source/world/playerpose_test.c \
	-lm \
	-o "$BHP/playerpose_test"

"./$BHP/playerpose_test"

rm -rf "$BHP"

# world/inventory.c's on-disk SAVE FORMAT. world/inventory_test.c already exists and
# already links inventory.c (see the "Step 8.2's inventory and crafting model" stanza
# earlier in this file) — but that suite's job is inventoryAdd/Remove/Swap/MoveUnits/
# SplitStack and crafting, with only a handful of save/load smoke checks bolted on, and
# it was never given a Makefile.*-test of its own the way playerpose's, blockdiff's and
# networld's suites were. This stanza is the persistence format's OWN suite, built the
# same way source/world/Makefile.playerpose-test's stanza above it is, and modelled
# directly on playerpose_test.c — inventory.c is literally what playerpose.c's own file
# comment says it was written against "line by line". No world.c/chunk.c/registry.c in
# the link: inventory.c has no equivalent of playerPoseUnstick() that needs a real World,
# and inventory.h's only other dependency, world/block.h, is header-only.
#
# Every hand-built corruption and defence-in-depth file in inventory_persist_test.c is
# byte-for-byte self-consistent — its own CRC is computed over the exact bytes on disk,
# out-of-range item id or oversized count included — because inventoryAdd()/
# inventorySave() can never put one of those there themselves (inventoryCanHold() and
# INV_STACK_MAX refuse them before a slot is ever written), so a hand-built file is the
# only way to reach inventoryLoad()'s defence-in-depth code at all.
#
# Twelve sabotages, every one applied to the REAL shipping source/world/inventory.c, each
# restored afterwards and the restore confirmed by md5 (inventory.c back to
# 093754e9503932a272fdda4fbb71e68e every time). Green arm before and after the whole run:
# "inventory persistence self-test: PASS 75 checks, 0 failed".
#
#  * magic comparison disabled            -> FAIL 1/75, "L322 invEqual(&out, &empty)".
#  * version comparison disabled          -> FAIL 1/75, "L339 invEqual(&out, &empty)".
#  * slot_count comparison disabled       -> FAIL 1/75, "L359 invEqual(&out, &empty)". This
#    check is unique to inventory.c among this tree's save formats — playerpose.c's format
#    has no equivalent field — and had no test anywhere before this file.
#  * the crc comparison disabled alone    -> FAIL 1/75, "L377 invEqual(&out, &empty)".
#  * the length check disabled ALONE first attempted, then abandoned as an arm: with only
#    the length check gone the one-byte-short case still fails closed, because a file short
#    by even one byte loses payload the crc covers a line later — the same relationship
#    world/playerpose_test.c records about its own length check ("defence in depth behind
#    the checksum"). Recorded here rather than run as a no-op arm for the same reason this
#    file's own playerpose stanza above gives about ITS length check.
#  * length check AND crc comparison BOTH disabled together -> FAIL 2/75: "L377" (the crc
#    case, expected — crc is also off in this arm) and "L409 invEqual(&out, &empty)", the
#    one-byte-short case, going red only now. Isolating this required rebuilding the
#    fixture to fill every slot first: the original one-byte-short test only reused
#    refInventory()'s three occupied slots, which left slot 23 (the one the missing byte
#    belongs to) already empty, so the guard on ITEM_NONE zeroed it right back to the
#    expected answer regardless of the garbage in the dropped byte — a check that could
#    not have failed. Rewritten to fill all 24 slots before this arm was run.
#  * the per-slot defence-in-depth guard disabled (`if (!inventoryCanHold(item) ||
#    count == 0) continue;` -> never taken) -> FAIL 3/75: an out-of-range item id
#    (BLOCK_WATER, == BLOCK_COUNT) no longer dropped, ITEM_NONE paired with a nonzero
#    count no longer dropped, a valid item paired with a zero count no longer dropped.
#    Each test's NEIGHBOUR slot (an ordinary valid item in slot 1) stayed green in the
#    same run, which is what says the guard drops the one bad slot rather than the whole
#    load going generally wrong.
#  * the INV_STACK_MAX clamp disabled     -> FAIL 1/75, "L535 out.slots[0].count ==
#    INV_STACK_MAX" — a count of 200 came back as 200, not clamped.
#  * the selected_hotbar clamp disabled   -> FAIL 1/75, "L557 out.selected_hotbar == 0" — a
#    stored value of 250 came back as 250. inventoryHeldItem()/inventoryHeldCount() index
#    slots[] with this value and have no bounds check of their own, so this is the one
#    guard standing between a crafted file and an out-of-bounds read the next frame.
#  * the recovery pass removed from inventoryLoad -> FAIL 4/75: the tmp-promoted-when-
#    real-is-gone scenario's three checks (payload, real file present, tmp gone), plus the
#    real-file-beats-a-leftover-tmp scenario's tmp-cleanup check — but NOT that scenario's
#    own "loaded the real file, not the stale tmp" check, which stayed green: with no
#    recovery pass at all, the real file is simply read directly and the leftover tmp is
#    never touched either way, so this arm and the next one are cleanly distinguished by
#    which checks move.
#  * inventoryRecover()'s "a good real file wins" shortcut removed (always promotes the
#    tmp) -> FAIL 1/75, "L595 invEqual(&out, &real)" — a stale tmp overwrote a good save.
#    This is the check the arm above could not reach.
#  * the writer's item/count byte order swapped (save only, loader untouched) -> FAIL
#    6/75, including "L210 memcmp(got, want, TEST_FILE_BYTES) == 0" — the check that the
#    file on disk is the layout inventory.c's own header comment documents, not merely one
#    the reader agrees with, the same argument world/playerpose_test.c makes about its own
#    exact-bytes check.
#  * the `!inv || !world_dir` guard removed from inventoryLoad -> SEGFAULT, RUN_EXIT=139,
#    on inventoryLoad(NULL, INV_DIR) — armed against a directory that really holds a valid
#    save, the same discipline world/playerpose_test.c's equivalent note describes, so the
#    guard is shown standing between here and a real dereference rather than a decoration.
#
# TWO FINDINGS, neither fixed — see this task's own report for the full reasoning; both
# are documented as explicit test cases (testTrailingBytesAreSilentlyAccepted,
# testEmptyWorldDir) rather than silently left uncovered:
#
#  * inventoryLoad() reads exactly sizeof(buf) (68 today) bytes into a fixed buffer and
#    only checks `n != sizeof(buf)`, so a file LONGER than the record has its first 68
#    bytes read as a perfectly good save and every byte past that is never looked at,
#    never counted, never refused. world/playerpose.c's own header comment already names
#    this exact gap in inventory.c and says why IT added a one-byte-wider read to catch
#    it; inventory.c itself was never given the same treatment. Measured here: a 69-byte
#    file whose first 68 bytes are a valid, checksummed save loads successfully with the
#    69th byte silently dropped. Left as a finding — changing what inventoryLoad() accepts
#    is a validation-strictness decision, not a crash or a data-loss bug, so it is
#    "missing", not "broken".
#  * inventorySave()/inventoryLoad() check `!world_dir` but, unlike playerpose.c's
#    dirUsable(), do NOT check for an empty string. An empty world_dir resolves to
#    "/inventory.dat" — this process's filesystem root, not a sandboxed world directory.
#    Measured on this host (WSL, non-root): inventorySave(&in, "") -> false (fopen on
#    "/inventory.dat.tmp" is refused by the OS) and inventoryLoad(&out, "") -> true with
#    an empty inventory (fopen on "/inventory.dat" fails the same way, so it degrades to
#    the ordinary missing-file path) — safe today, but only because this host refuses the
#    write, not because inventory.c does. Left as a finding rather than given inventory.c
#    its own dirUsable() unasked.
BHI="build-host/run-$$-invpersist"
mkdir -p "$BHI"

gcc -std=c11 -Wall -Wextra -Werror -O1 -g \
	-I source \
	source/world/inventory.c \
	source/world/crc32.c \
	source/world/inventory_persist_test.c \
	-o "$BHI/inventory_persist_test"

"./$BHI/inventory_persist_test"

rm -rf "$BHI"

# --- v1.7.1 task 49, install half -------------------------------------------------------
#
# Added testChunkPlanAllMatchesLoadAll() to source/world/world_test.c. It links the real
# world/chunk.c like everything else in this binary, so no new gcc block is needed: the
# world_test binary near the top of this file already compiles that file.
#
# THE BUG IT NOW GUARDS AGAINST. main.c's comment on genInstallOne priced a column install at
# "0.12 ms each" and called it "a memcpy, not a mesh". The Azahar capture behind task 49
# measured it at roughly 4.36 ms -- about 36x that -- and because main.c folds install_ms into
# work_ms and publishes work_ms as the CSV's mesh_ms, all of it had been masquerading as mesh
# time. It stopped being a memcpy at step 9.2a, when Chunk became opaque: app/worker.c's
# workerInstall decompresses each staged chunk into a flat buffer and calls world/world.c's
# worldSetChunkAll, and THAT walked the 4096-cell buffer three times -- chunkFormForAll to
# size the budget claim, chunkLoadAll to rebuild the identical palette, and a third pass to
# pack the nibbles which searched the palette linearly (up to 16 compares) for every cell.
#
# Profiled on the host over a real 7x7-column streaming pass driving the REAL install calls
# (49 columns, 300 chunks, medians of 5 passes, us per installed COLUMN):
#
#   worldSetChunkAll      93.3   89.3%   (of which chunkFormForAll 22.5, chunkLoadAll 65.3)
#   chunkDecompressAll     9.4    9.0%
#   worldExit(staging)     1.2    1.1%
#   genQueueReadyColumns   0.6    0.6%
#   lightColumnCopy        0.1    0.1%
#   TOTAL                104.5
#
# The fix is world/chunk.h's ChunkPlan: one walk fills the palette AND a 256-entry
# id -> slot table, worldSetChunkAll builds it once so both the budget claim and the commit
# read the same answer, and the packing loop writes each index byte once from that table
# instead of read-modify-writing it twice through setNibble. Paired and INTERLEAVED against
# the unchanged code, four rounds each in one process (an unpaired run of the sibling mesher
# benchmark drifted 78 -> 29 us on unchanged code purely from host load, so a before taken an
# hour earlier is two anecdotes, not a measurement):
#
#   BEFORE  4513.7 / 4733.8 / 4121.9 / 4201.0 us per pass   (15.05 us/chunk median)
#   AFTER   1049.6 /  971.1 / 1107.4 / 1061.1 us per pass   ( 3.50 us/chunk median)
#
# 4.2x on worldSetChunkAll; 104.5 -> 29.8 us on the whole install path. Identity across the
# pair: an FNV-1a over the form byte, all 4096 decompressed cells, the chunkEncode payload
# bytes (so palette ORDER counts, not just contents) and the byte count of every one of the
# 300 chunks, plus the budget peak and used figures, is a8ed2826ea80538c in BOTH arms and in
# all eight runs.
#
# Sabotaged in the REAL source/world/chunk.c and measured, each arm restored afterwards and
# the restore confirmed by md5 (chunk.c back to e36cacf31b1087e73d653a33b2269fd4 every time),
# built with -DWORLD_TEST_VERBOSE so every failing line was read and not just the first. The
# green arm before and after every one of them: "world self-test: PASS  4815 checks".
#
#  * chunkLoadPlanned's packing loop with its nibble parity swapped (the even cell written to
#    the high nibble and the odd to the low) -> "FAILED - 1298 of 6079 checks", first
#    "L4518 worldGet(&s_world, lx, h - 1, lz) == cap". The new test's own lines in that run:
#    L8796 cells_a, L8797 cells_b, L8814 idx[0] == 0, L8815 idx[1] == 1, four times over --
#    plus eleven copies of "L7366 genTestHashColumnTerrain(...) == pinned[i].hash", i.e. the
#    pre-existing pinned-terrain hashes caught it too. This is why every buffer in the new
#    test alternates ids: a buffer of runs survives a swapped pair unnoticed.
#  * the palette's first two slots swapped in buildPlan, with slot_of swapped to match, so the
#    chunk DECODES identically and only its stored order differs -> "FAILED - 16 of 4815",
#    first "L8812 pal[0] == in[0]", and exactly L8812/8813/8814/8815 four times over. Nothing
#    else in the suite moved, which is the point: a reordered palette is invisible to every
#    behavioural check, so the order is asserted directly.
#  * the palette capacity cut from 16 to 15 (`if (n == 15)`) -> "FAILED - 2 of 4803", both
#    "L8777 plan.count == (n <= 16 ? n : -1)". The check total falls to 4803 because the
#    PALETTE4-only block is skipped once the 16-id buffers turn RAW.
#
# TWO CHECKS THAT DID NOT GO RED WHERE THEY LOOK LIKE THEY SHOULD, recorded so nobody reads
# them as proof they are not. "CHECK(form == chunkFormForAll(in))" stayed green under the
# capacity arm, and "memcmp(enc_a, enc_b, la) == 0" stayed green under the palette-order arm,
# both because chunkFormForAll and chunkLoadAll are now thin wrappers over the very pair being
# sabotaged, so both sides of each comparison moved together. They are real checks -- they pin
# the wrappers to the new pair -- but they are NOT the arm that catches a wrong plan.
# "plan.count" and "pal[0] == in[0]" are, and both are asserted against literals.
#
# NOT VERIFIED: any of this on hardware or in an emulator. The us figures above are host
# figures at -O2 on an x86-64; the console is a 268 MHz ARM11 with a far smaller cache, so
# they are a RATIO measurement, not a console measurement. The "~1.4 ms after" figure written
# into main.c's corrected comment is 4.36 ms times that ratio -- arithmetic, not evidence.
# Confirming it needs another Azahar CSV capture of the same walk.

# ───── v1.8.0 task 49h: three new binaries ─────
#
# Two of the three are built in a way nothing else in this file does, so the reason is written
# down here rather than left to be reverse-engineered.
#
# source/scene/chunk_render.c cannot be compiled on the host: it includes <3ds.h> and
# <citro3d.h> and is C3D_ calls most of the way down. Two of 49h's three changes live in it -
# the horizon cull's predicate and the build profiler's reset - and both are claims about
# RUNTIME BEHAVIOUR, so neither a _Static_assert nor a grep for the fixed line would have
# proved anything. The choice was between hand-copying those functions into a test file (the
# "a test that links nothing tests nothing" failure recorded against battery_test and
# sleep_test further up this file, and the reason a green suite once survived its feature
# being deleted) and lifting their REAL source text out of chunk_render.c at build time.
# This does the second: awk takes the exact range from each function's signature line to its
# closing brace in column 0, writes it into this run's own build dir, and the test #includes
# it. Sabotage chunk_render.c and these two binaries go red - there is no second copy of the
# code anywhere that could drift out of step with the shipped one.
#
# The extraction is anchored on the full signature line and terminated on a "}" in column 0,
# which is this project's brace style everywhere (every inner brace is tab-indented). If one
# of those functions is ever reformatted so its closing brace is indented, the extraction
# truncates, gcc fails on an unterminated function, and set -e stops the suite. That is the
# right failure: loud, not a silent pass.

BHZ="build-host/run-$$-49h"
mkdir -p "$BHZ"

# horizon_test: the cull predicate. Proves the cross-multiply rewrite decides every chunk the
# way the shipped float-angle code was TRYING to, judged by a double-precision oracle carried
# in the test. -lm for sqrtf/atan2f.
awk '
/^#define HZN_HALF_W/ { print }
/^static bool horizonHidden\(int cx, int cy, int cz\)$/ { inf=1 }
inf { print; if ($0 == "}") inf=0 }
' source/scene/chunk_render.c > "$BHZ/horizon_extract.inc"

gcc -std=c11 -Wall -Wextra -Werror -O1 -g 	-I source -I "$BHZ" 	tests/horizon_test.c 	-lm 	-o "$BHZ/horizon_test"

"./$BHZ/horizon_test"

# profile_reset_test: chunkRenderProfileReset() must clear s_vis_ticks, which it did not until
# 49h, so every microsecond figure chunkRenderVisUs() has ever printed after a reset was ticks
# since boot over a count that had restarted.
awk '
/^void chunkRenderProfileReset\(void\)$/                                       { inf=1 }
/^void chunkRenderProfile\(float\* scratch_us, float\* mesh_us, int\* builds\)$/ { inf=1 }
/^float chunkRenderVisUs\(void\)$/                                             { inf=1 }
inf { print; if ($0 == "}") inf=0 }
' source/scene/chunk_render.c > "$BHZ/profile_extract.inc"

gcc -std=c11 -Wall -Wextra -Werror -O1 -g 	-I source -I "$BHZ" 	tests/profile_reset_test.c 	-o "$BHZ/profile_reset_test"

"./$BHZ/profile_reset_test"

# scratch_light_test: an identity test on the REAL world/scratch.c, whose scratchFillLight()
# 49h rewrote for speed and must therefore still produce the same 5,832 bytes. Ordinary link,
# no extraction needed - source/world is host-clean. world.c #includes light.c (see its line
# 16), so light.c must NOT be listed separately or it is defined twice.
gcc -std=c11 -Wall -Wextra -Werror -O1 -g 	-I source 	source/world/world.c 	source/world/block.c 	source/world/registry.c 	source/world/chunk.c 	source/world/budget.c 	source/world/scratch.c 	tests/net_stub.c 	tests/scratch_light_test.c 	-o "$BHZ/scratch_light_test"

"./$BHZ/scratch_light_test"

rm -rf "$BHZ"

# ========================================================================================
# v1.7.1 task 48b -- "make world loading measurable, then make it faster"
# ========================================================================================
#
# Two new binaries below. Read the notes at the bottom before changing either of them: most
# of what this task learned is about MEASUREMENT being wrong, not about the code being slow.

# --- 48b, part 1: the load profiler ----------------------------------------------------
#
# source/debug/loadprof.{h,c} is an instrument, not a feature. It brackets the world-enter
# path into named stages -- worlddir, played, setup, region_io, decode, generate, light,
# install, queue, mesh, present -- and reports per-stage total ms plus a call count, so a
# load can be read without an emulator screenshot.
#
# WHY IT HAD TO EXIST. The figure this task was handed as its starting evidence was Azahar's
# CSV `frame_ms`, which showed a 20,481 ms "frame". The same CSV row reads cpu_ms 3.220,
# mesh_ms 0.004, recenter_ms 0.001. frame_ms is wall clock between in-world frame boundaries
# and therefore spans the pause menu, the title screen, the world list and the capture
# harness's own sleeps and screenshots. Every load-stall number ever quoted from it --
# 57,846 / 23,891 / 58,966 / 23,557 / 54,905 / 20,482 ms -- is that artefact. Do not revive
# them, and do not measure a load with frame_ms again.
#
# source/debug/loadprof_test.c drives the REAL load path (regionReadColumnCached ->
# regionDecodeColumn -> worldgenColumn -> lightPropagateColumn -> chunkDecompressAll +
# worldSetChunkAll -> meshChunk) over a world that is created, edited, saved and reloaded,
# and asserts the SHAPE of the breakdown rather than any timing: fresh must generate all 81
# columns and read none, reloaded must read all 81 and decode exactly the 25 that were
# edited, decode + generate must come to 81, both must install 81 and mesh 392, the edit
# must read back as BLOCK_WOOD, and the two worlds must hash differently. A run where fresh
# and reloaded produce the same breakdown has not reproduced the case.
BHL="build-host/run-$$-loadprof"
mkdir -p "$BHL"

gcc -std=c11 -Wall -Wextra -Werror -O1 -g \
	-I source \
	source/debug/loadprof.c \
	source/debug/loadprof_test.c \
	source/world/block.c \
	source/world/registry.c \
	source/world/chunk.c \
	source/world/chunk_codec.c \
	source/world/crc32.c \
	source/world/region.c \
	source/world/world.c \
	source/world/scratch.c \
	source/world/mesher.c \
	source/world/noise.c \
	source/world/budget.c \
	source/world/worldgen.c \
	source/world/worldgen_density.c \
	source/world/genversion.c \
	tests/net_stub.c \
	-lm \
	-o "$BHL/loadprof_test"

mkdir -p build-host
"./$BHL/loadprof_test"

rm -rf "$BHL"

# --- 48b, part 2: the noise A/B --------------------------------------------------------
#
# source/world/noise_ref.c is world/noise.c as it stood BEFORE this task, kept compiled so
# the change can be checked and timed against it in one process. It is a fixture: nothing in
# the game calls it and the whole file is #ifndef __3DS__. Do not "keep it in sync" -- its
# only value is that it is the old code.
#
# The change under test is noiseFbmNormalise, which replaces the fBms' closing `total /
# norm` -- a 64-bit divide by a value the compiler cannot see -- with a shift and a divide
# by a literal. noise_ab_test.c brute-forces it against the plain division across the whole
# documented input range at every octave count, then compares both fBms and both samplers
# old-against-new over 20,000 random seeds and positions, then times the two arms
# ALTERNATELY in the same process on the coordinate stream worldgenIsCave produces.
BHN="build-host/run-$$-noiseab"
mkdir -p "$BHN"

gcc -std=c11 -Wall -Wextra -Werror -O2 -g \
	-D_POSIX_C_SOURCE=200809L \
	-I source \
	source/world/noise_ab_test.c \
	source/world/noise_ref.c \
	source/world/noise.c \
	-lm \
	-o "$BHN/noise_ab"

"./$BHN/noise_ab"

rm -rf "$BHN"

# ========================================================================================
# v1.7.1 task 48b -- findings, in the order they were established
# ========================================================================================
#
# 1. WHERE THE TIME GOES, measured with the profiler above (host, -O1, 81 columns, 392
#    chunks, seed 1337, RENDER_DIST_MAX ring):
#
#      fresh, light off     135.4 ms   generate 117.784 (87.0%, n=81, 1454.1 us/col)
#                                      install    2.622 ( 1.9%, n=81)
#                                      mesh      15.023 (11.1%, n=392, 38.3 us/chunk)
#
#      reloaded, light off  104.0 ms   region_io  7.482 ( 7.2%, n=81,  92.4 us/col)
#                                      decode     0.578 ( 0.6%, n=25)
#                                      generate  77.752 (74.8%, n=56, 1388.4 us/col)
#                                      install    2.144 ( 2.1%, n=81)
#                                      mesh      16.030 (15.4%, n=392)
#
#      fresh, light on      147.4 ms   light 6.893 (n=81), mesh 27.539
#      reloaded, light on   117.5 ms   light 6.705 (n=81), mesh 24.551
#
#    THE REPORTED SYMPTOM IS NOT REPRODUCED, and the numbers say why it cannot be. The
#    reloaded-edited world loads FASTER than the fresh one -- 104.0 against 135.4 ms --
#    because 25 of its 81 columns come off the card instead of being generated, and a
#    cached region read is 92 us against 1388 us to generate. An edited world doing MORE
#    work than a fresh one was the pre-task-48 uncached region path: 81 successful fopens
#    plus 162 directory reads on an edited world against 81 FAILED opens on a fresh one.
#    That was fixed by regionReadColumnCached (39.2/43.5/56.0 us -> 3.34/3.34/3.64 us per
#    column, 32,400 fopens -> 2). If the doubling survives in v1.7.1 it is not in this path.
#
# 2. WHAT DOMINATES, gprof over the same harness:
#
#      65.22%  16,626,871 calls  value3At
#      15.22%   8,120,494 calls  noiseFbm3
#       2.17%         299 calls  wgdColumn  (0.03 self / 1.17 total ms per call)
#       0.00%   5,553,397 calls  worldgenIsCave
#
#    Four fifths of a world load is the value-noise lattice, driven from the cave test:
#    ~27,158 noiseFbm3 and ~18,573 worldgenIsCave per column. Nothing else is close.
#
# 3. WHAT WAS TRIED AND REJECTED -- hand-factoring the lattice hash.
#
#    rngHash3(s,x,y,z) is rngMix(rngHash2(s,x,z) ^ y*C) and rngHash2 mixes x then z, so the
#    eight corners value3At needs share their early stages: 14 rngMix and 6 multiplies
#    written out against 24 and 24 called separately. It was implemented, it was bit-for-bit
#    correct, and it was SLOWER. Two measurements killed it:
#
#      * ARM11, the toolchain the game ships with (-march=armv6k -mtune=mpcore -O2):
#        world/noise.o was 508 instructions / 94 multiplies BEFORE the factoring and
#        535 / 101 after. The sharing was not there to be won -- rngHash2 and rngHash3 are
#        `static inline`, so GCC already common-subexpression-eliminates the shared stages
#        across the eight call sites.
#      * Host, both arms in one process on the same coordinates: 0.974 ms against 0.881 ms,
#        10.5% worse, because pushing the corners through an array costs more scheduling
#        freedom than the arithmetic saves. An earlier two-array version measured 1.416
#        against 0.836 -- 69% worse.
#
#    Reverted. The note in source/world/rng.h says so at the site, so the next person does
#    not spend the same day on it.
#
# 4. WHAT WAS KEPT -- noiseFbmNormalise, and be honest about its size.
#
#    On the ARM11 the fBms' closing division compiles to `bl __aeabi_ldivmod`, because there
#    is no divide instruction on that core. Measured with objdump on the shipped toolchain:
#    world/noise_ref.o references __aeabi_ldivmod and contains 2 calls to it; world/noise.o
#    references it 0 times. That is 8.1 M library calls removed from one world load.
#
#    On the HOST the same change is a wash -- -1.3% at the median on the first interleaved
#    run, arms swapping the lead round to round -- because x86 has a hardware 64-bit idiv
#    and there was never much there to win. So the benchmark in noise_ab_test.c asserts a
#    REGRESSION GUARD (new min < ref min * 1.15), not a speedup. It is not a speedup claim
#    dressed down; the host genuinely cannot see this one.
#
#    v1.8.2 made that one guard OPT-IN, and only that one -- the 105,573 equivalence checks in
#    the same binary are untouched and still hard. It measures wall clock, so it is the only
#    check in this whole file whose answer depends on what ELSE the machine is doing, and it
#    failed once during a full-suite run with several parallel builds in flight, then passed
#    3/3 standalone. It still RUNS and still PRINTS its ratio against the ceiling on every run
#    -- read the "guard" line in the noise A/B output -- but it only fails the suite when
#    BS_NOISE_BENCH_STRICT=1 is in the environment. Widening 1.15 was rejected: see the comment
#    at the check itself in source/world/noise_ab_test.c for why that trades a flake for a
#    blind spot. Set the variable when benchmarking; leave it unset in this script.
#
#    Bit-identity is proved two ways: 105,574 checks comparing old against new across the
#    normalise input range and 20,000 random noise samples, and the load harness's whole-
#    world hash over every loaded chunk's decompressed cells, unchanged across the change at
#    fresh=5a7a4b485f734255 reload=6bf92d6e4a81c08c.
#
# 5. THE REAL CEILING ON WORLD ENTRY, which is NOT cpu time and was not changed.
#
#    The worker stages ONE column and parks until the main thread takes it (worker.c: the
#    s_ready handshake; there is one staging world, so a second result would overwrite the
#    first). runLoadingScreen's install loop is bounded at 8 but in practice gets one column
#    per pass, and each pass ends in C3D_FrameBegin(C3D_FRAME_SYNCDRAW) -- a VBlank wait.
#    81 columns therefore cost at least 81 frames, and at 59.83 Hz that is 1.354 SECONDS of
#    world entry against ~135 ms of actual work. No amount of making the generator faster
#    moves that number; the handshake has to allow more than one column in flight, which
#    means more than one staging slot and is a design change, not an optimisation.
#    LOAD_STAGE_PRESENT exists to make this visible: a load whose `present` total is roughly
#    frames * 16.71 ms is frame-bound, and that is the number to attack next.
#
# 6. saveWorldDir() is called TWICE per world entry -- once from main() (boot_dir) and again
#    from genStart() (world_dir) -- and each call does three mkdirs plus a write probe.
#    Left alone: it was not what this task was asked to change.
#
# NOT VERIFIED, and the gap matters:
#
#   * Nothing here has run on a 3DS or in an emulator. Every millisecond above is host,
#     x86-64, -O1 or -O2. The console is a 268 MHz in-order ARM11 with a much smaller cache,
#     so treat the stage SHARES as transferable and the absolute times as not.
#   * The __aeabi_ldivmod removal is a static instruction fact from objdump, not a timing.
#     Nobody has measured what it is worth in cycles on real hardware.
#   * The 1.354 s frame-bound floor in (5) is arithmetic from 81 columns and a 59.83 Hz
#     VBlank, not a capture. Confirming it needs loadprof's `present` total off a console.
#
# ========================================================================================
# v1.7.1 task 48b addendum -- the workerSubmitSave stall
# ========================================================================================
#
# A separate question, about the OTHER symptom: frame-rate drops "in certain spots".
#
# THE CLAIM. workerSubmitSave() spin-sleeps the MAIN thread in 1 ms steps while both save
# slots are full (SAVE_SLOTS is 2), and the worker it waits on is pinned to core 0 -- the
# same core as the main thread. genUnloadColumn() calls it once per DIRTY column leaving the
# ring, and genRecenter() can unload many columns in one frame. The col->dirty gate means an
# unedited world never enters the branch, so this is invisible everywhere except where the
# player has actually built.
#
# WHAT WAS ESTABLISHED, and what was not.
#
#   * The geometry. genInArea() is a square window of half-width s_area_radius, and
#     genRecenter() unloads every old-window column that falls outside the new one. One
#     ordinary AXIS-ALIGNED step therefore unloads a whole edge: 2*s_area_radius+1 = 9
#     columns at RENDER_DIST_MAX, and 17 on a diagonal step. This contradicts the
#     justification written on SAVE_SLOTS in worker.c, which said a third slot would only
#     buy the case of "three edited columns unloading in the same frame, which needs the
#     player to have built in three columns and then crossed a boundary diagonally". Three
#     dirty columns among the nine that leave on a plain sideways step needs a base three
#     columns wide and a walk out of it. That is not a diagonal-only rarity.
#
#   * The duration. NOT MEASURED, and not measurable from here. How long a blocked submit
#     actually costs is the remaining time in the worker's current column plus an SD write,
#     and neither exists on a PC. worker.c is libctru-only (LightLock, LightEvent,
#     svcSleepThread, threadCreate) so it cannot be linked into any host suite, and this
#     session may not run an emulator. A host model of the ring would be testing the model.
#
#   * NO FIX WAS MADE. Changing a save path that cannot be tested from here is how a
#     player's build gets lost. The measured geometry says the branch is reachable far more
#     easily than its comment claims; it does not say the wait is long.
#
# WHAT WAS ADDED INSTEAD -- the observability that makes one capture decisive.
# worker.c now counts the wait: workerSaveSubmits(), workerSaveWaits(), workerSaveWaitMs()
# and workerSaveWaitMaxMs(), cleared per world in workerStart, printed on the debug overlay
# as "save sv<n> w<n> <total> max <worst> ms".
#
# This exists because the existing `save_ms` CSV column CANNOT answer the question: a frame
# with no dirty column reports 0.0 ms and so does a frame that submitted three and never
# waited. The three readings are now distinguishable:
#
#     sv 0                -> the branch was never entered. Says NOTHING. Not a clean bill.
#     sv large, w 0       -> the measured negative. Exercised, never blocked.
#     w climbing, max ms  -> the positional hitch, caught.
#
# TO SETTLE IT: build in an area at least three columns wide, walk out of it sideways so a
# whole ring edge unloads at once, and read the save line. That capture is steve's -- it
# needs hardware or an emulator, and one run answers it either way.
#
# The scan that raised this flagged it as an inference from source constants, not a
# measurement. It still is. What changed is that the geometry half of it is now checked and
# the timing half is now instrumented.

# world/water.c -- the water simulation (v1.8.0 task 22). Own binary, own main(), appended
# below rather than merged into the world stanza for the reason the playerpose stanza above
# gives: an append is the one edit shape that cannot silently drop another session's work.
#
# The link is the world stanza's minimum plus water.c: world.c drags in block.c, registry.c,
# chunk.c and budget.c, tests/net_stub.c supplies networldOnColumnLoad, and chunk_codec.c
# plus crc32.c are there because the save-compatibility probe encodes a column the way the
# region writer does and compares the bytes from before the simulation ran against the
# bytes from after it.
#
# What this suite settles that nothing else can: the exact shape of a pour (a manhattan ball
# of radius 7, level 8-d at distance d, 112 flow cells), that a source in mid-air makes a
# one-block-wide fall and a pool rather than a solid block of water, that a settled body
# examines ZERO cells per tick for ever, that the same pour settles to a byte-identical
# world at four different per-tick budgets and with two sources disturbed in either order,
# that a v1.7.1 world opens with every generated water cell reading as a source and no work
# queued, and the real per-tick cost of a settled and of a saturated tick, measured in one
# process with the two arms interleaved.
BHW="build-host/run-$$-water"
mkdir -p "$BHW"

gcc -std=c11 -Wall -Wextra -Werror -O1 -g \
	-I source \
	tests/net_stub.c \
	source/world/block.c \
	source/world/registry.c \
	source/world/chunk.c \
	source/world/chunk_codec.c \
	source/world/crc32.c \
	source/world/budget.c \
	source/world/world.c \
	source/world/water.c \
	source/world/water_test.c \
	-lm \
	-o "$BHW/water_test"

"./$BHW/water_test"

rm -rf "$BHW"

# world/water_mesh_test.c -- the SHAPE of flowing water (v1.8.0 task 22b). Own binary, own
# main(), appended rather than merged into the water stanza above for the reason every stanza
# in this file is appended: an append is the one edit shape that cannot drop another session's
# work, and this file is shared.
#
# Separate from water_test.c on purpose. That suite owns the SIMULATION -- where water goes and
# what level it ends up at -- and this one owns what the mesher does with those levels. The two
# have different link sets (this one drags in scratch.c and mesher.c) and a failure in one
# should not be able to hide behind the other's totals.
#
# What this suite settles that nothing else can: that a flow cell of level L is emitted L/8 of a
# block tall and its floor does not move, that a cell with water directly above it is drawn FULL
# height (water.c gives a falling cell level 7, not 8, so without that rule a waterfall is a
# stack of slabs), that every one of the 64 ordered pairs of levels standing side by side leaves
# exactly one quad on the plane between them spanning the floor to the TALLER surface -- the
# level-boundary hole is the failure this task was most likely to ship -- that a greedy run stops
# where the height changes while cells at one level still merge, that waterFillScratch agrees
# with waterLevelAt over the whole 18-cube window, and -- the control -- that a world of
# ordinary terrain still meshes byte-identically to v1.7.1 with the band switched on, pinned at
# faces=88 hash=0x9f47e02f. That terrain pin is the check that must stay green while any of the
# others is sabotaged.
#
# CORRECTED IN v1.8.2. This paragraph used to claim the SOURCES world was pinned byte-identical
# to v1.7.1 as well, and it is not, deliberately: ask 2 dropped the water surface to 7/8 of a
# block so a source with sky above it no longer sits flush with the block beside it. That was a
# requested change, not a regression, so testSourceUnchanged asserts the NEW height rather than
# the v1.7.1 bytes. Only the terrain world is still pinned to v1.7.1. Do not "restore" the
# sources pin -- re-flattening the surface is the thing this suite now exists to catch.
# ── v1.8.2, testLevelChain: the pair test's blind spot ───────────────────────────────────────
#
# 69 checks -> 74, one new probe (testLevelChain), no other probe touched and no pin re-derived.
#
# testLevelBoundary is exhaustive over TWO cells and structurally blind to anything that needs
# three: a greedy run's state carried from one step into the next, or an off-by-one that
# compounds along a slope. The only 3+ fixture in the suite was testPinnedGeometry's staircase,
# guarded by a face count and a hash -- which say "a byte moved" and never which boundary moved
# it. That matters more than usual because the shelf cannot be PHOTOGRAPHED: water is unplaceable
# in this build (BLOCK_WATER == BLOCK_COUNT, inventoryCanHold refuses it, there is no bucket), so
# this suite is the only evidence the behaviour works at all.
#
# testLevelChain builds a real spreading shelf -- levels 7,6,5,4,3,2,1 side by side along +X at
# y = 8, z = 8, drawn drops 1..7 -- and puts all six internal boundaries through the same three
# assertions the pair test uses: exactly one wall quad on the plane, spanning the floor to the
# TALLER neighbour, and no quad covering cells on both sides of it. That last one is the check
# that reaches the gap. With seven DISTINCT heights no merge along X is legal, so a quad
# straddling a boundary plane is a run that swallowed a step.
#
# The arm, against real production source: mergeCandidate's drop-equality test in
# world/mesher.c:473 (`if (cellDrop(nb) != drop) return false;`) relaxed to "within 1 eighth", so
# adjacent-by-one levels wrongly merge -> "FAILED - 4 of 74 checks", and inside the new probe
# "FAIL   line 728: no quad spans a step: a run that swallowed one would flatten a visible
# boundary", with "...first offending plane: x = 2". Note WHICH check went red: the count and
# span checks stayed green, because a 1-wide chain's x-facing walls merge along Z where there is
# nothing to merge with -- the swallowed step surfaces in the FLOORS and the z-facing walls
# instead, and only the straddle check can see it. The rest of that arm's damage landed on
# testNoGapAtTheColumn (line 452) and the staircase's face count and hash (lines 850, 853).
#
# Control green in BOTH arms: "control: plain terrain meshes to the 88 faces and the bytes it did
# before v1.8.2" (faces=88 hash=0x9f47e02f). Terrain is all drop 0, so the relaxed test is a
# no-op on it -- which is what makes the red arm a statement about water and not about the build.
BHWM="build-host/run-$$-watermesh"
mkdir -p "$BHWM"

gcc -std=c11 -Wall -Wextra -Werror -O1 -g \
	-I source \
	tests/net_stub.c \
	source/world/block.c \
	source/world/registry.c \
	source/world/chunk.c \
	source/world/budget.c \
	source/world/world.c \
	source/world/scratch.c \
	source/world/mesher.c \
	source/world/water.c \
	source/world/water_mesh_test.c \
	-lm \
	-o "$BHWM/water_mesh_test"

"./$BHWM/water_mesh_test"

rm -rf "$BHWM"

# world/mining_test.c -- how long a block takes to break (roadmap task 50, part 1). Own binary,
# own main(), appended rather than merged into any stanza above it for the reason every stanza
# in this file is appended: an append is the one edit shape that cannot drop another session's
# work, and this file is shared.
#
# The link is deliberately tiny -- block.c, registry.c, mining.c -- because mining.c is a pure
# function of the registry and nothing else. The REAL registry.c is in it, not a table of
# hand-copied hardness values: this suite exists partly to prove that refreshView() actually
# copies BlockDef.hardness into the BlockInfo the game reads, and a test carrying its own copy
# of the ten numbers would stay green with that copy deleted. That is the exact failure mode
# recorded against app/battery.c and app/sleep.c further up this file.
#
# What it settles: that each of the ten core rows reads back the hardness it was given, BY NAME
# and through blockInfo() rather than through the def; that breakTicksRequired() at bare hands
# is the hardness itself, because the only multiplier in the game is 1x; that a block with any
# hardness never breaks in zero ticks; and that the division rounds UP rather than truncating.
#
# That last claim is driven through miningCeilDiv() directly and not through breakTicksRequired(),
# on purpose and with the reason written at the probe: at 1x every division is exact, so no
# fractional case can be reached through the public entry point until roadmap task 32 adds a
# tool with a multiplier that is not 1. Testing rounding through the entry point today would be
# testing nothing and printing green.
#
# Sabotaged in the REAL source and measured, each arm restored afterwards and the restore
# confirmed by md5 (registry.c back to a140f3bcdd79ef3c90cb2bed79fd9237, mining.c to
# 5213b00a8ffbb448713dfd8f0577c6a0, block.h to 2f159a6a1d1ddccdf284689a408c596e). Green arm
# before and after the whole run: "PASS 44 checks, 0 failed", exit 0.
#
#  * BLOCK_STONE's .hardness put to 44 in registry.c's kCoreDefs -> "FAIL 44 checks, 2 failed":
#    "L67 stone is 45 ticks (2.25 s)" and "L101 stone takes 45 ticks". Exactly the two checks
#    written for stone's value, and nothing else -- the other nine rows and the whole rounding
#    probe stay GREEN, which is what says these checks discriminate one row rather than
#    collapsing the fixture.
#  * miningCeilDiv() made to truncate (`? q : q` in place of `? q : q + 1`)
#    -> "FAIL 44 checks, 7 failed", first "L157 ceil of 1/1000000 is 1, not 0: a truncating
#    divide would make a break instant", then all six rounding cases (L175, L180-L184).
#    Every hardness read and every bare-hand break time stays GREEN, because at 1x every
#    division is exact -- which is precisely why the rounding claim has to be driven through
#    the helper and cannot be reached through breakTicksRequired() today.
#  * refreshView()'s `v->hardness = d->hardness;` deleted -> "FAIL 44 checks, 20 failed", first
#    "L65 grass is 12 ticks (0.60 s)". Every named hardness with a non-zero value, both
#    def-vs-view agreement checks, every non-zero break time, and both never-instant checks.
#    Air and water stay green (0 either way), and so does the entire rounding probe -- the
#    arithmetic is fine, the game simply cannot see the data.
#
# The controls that stayed green in EVERY arm, red ones included: "control: the registry still
# answers about solidity, which task 50 did not touch", "control: an undefined id reads back as
# air, so hardness 0", and the three exact-division controls in the rounding probe ("control: an
# exact 8/4 is 2, not 3", "control: an exact 12/4 is 3, not 4", "control: an exact 45x at 1x is
# 45, not 46") -- the ones that separate "rounds up" from "always adds one", and which the
# truncating arm also passes.
BHMN="build-host/run-$$-mining"
mkdir -p "$BHMN"

gcc -std=c11 -Wall -Wextra -Werror -O1 -g \
	-I source \
	source/world/block.c \
	source/world/registry.c \
	source/world/mining.c \
	source/world/mining_test.c \
	-o "$BHMN/mining_test"

"./$BHMN/mining_test"

rm -rf "$BHMN"

# cavewalk_test: the cave cull's per-frame ORCHESTRATOR -- source/scene/chunk_render.c's
# caveWalk(). The algorithm underneath it (world/visgraph.c) has had coverage in world_test.c
# since Phase 7; caveWalk itself had none, and it is the half that decides which chunks the
# algorithm is told about, how big a box to run it over, whether to re-run it, and whether the
# renderer may trust the answer at all. Three of those fail toward OVER-culling, which
# world/visgraph.h calls out in bold as a hole in the world rather than a missed optimisation.
#
# Extracted, not hand-copied, exactly like horizon_test above and for the same reason:
# chunk_render.c includes <3ds.h> and <citro3d.h> and cannot be compiled on the host, and a
# hand-copied function is the "a test that links nothing tests nothing" failure this project
# has already paid for twice. The awk below lifts three statics out by their signature lines --
# cullInvalidate, chunkOfFloat, caveWalk -- in file order, which is also the order they have to
# be defined in. The trailing #define is the anchor's own receipt: it is printed only when the
# caveWalk range both STARTED and CLOSED, and source/world/cavewalk_test.c has an #error on it.
# Break an anchor and the build stops at compile time naming the lost anchor; there is no
# fallback copy in the test to quietly pass against.
#
# Proof that it really is the extracted function and not a copy, measured: with the caveWalk
# anchor changed to match a name that does not exist, the compile fails with
#   error: #error "cavewalk_extract.inc did not carry caveWalk() out of source/scene/chunk_render.c"
# and, with that #error stubbed out to expose the underlying dependency,
#   error: implicit declaration of function 'caveWalk'
# Nothing named caveWalk exists in the test file itself.
#
# world/visgraph.c is NOT extracted -- it is host-clean and is linked for real, so the walk the
# checks observe is the walk the console runs. block/registry/chunk/budget are along only
# because visChunkConnectivity (which this suite never calls) references them; -lm for floorf.
#
# What each family guards is written at the top of source/world/cavewalk_test.c. Green arm,
# before and after every arm below: "PASS 90 checks, 0 failed", exit 0.
#
# READ THE TOTALS WITH THEIR DATE. Arms A-E below were measured when this suite was 81 checks
# and their "of 81" denominators are left as they were measured rather than rescaled, because
# a denominator nobody re-ran is a fabricated number. v1.8.2 took the suite to 90: the cache-
# key family was rewritten (see arm F, and testCacheKeyIsSound in the test file). The failure
# COUNTS in arms A-E are still the counts those sabotages produced; only the total moved.
#
# Sabotaged and measured. Arms A-E mutate the EXTRACT -- byte-for-byte the text awk lifted out
# of chunk_render.c seconds earlier, so it is that function's code that goes wrong -- because a
# parallel session owned chunk_render.c while this was written and editing it would have
# clobbered their work. Arms F-G mutate the REAL source/world/visgraph.c on disk, restored
# afterwards and confirmed byte-identical by md5 (back to f065bc84cc09efcb6aaabdd224260ff6);
# chunk_render.c was never written to at all (d89f6c0a007494e60ed3ac34b4dbb942 throughout).
#
#  * A. chunkOfFloat's floorf replaced by a truncating cast -> "FAILED - 6 of 81 checks":
#    the four negative-coordinate cases (L251-L255), the 2,049-position sweep against
#    visgraph's chunkOfBlock (L273), and "and the key is chunk -1, not chunk 0" (L522) -- the
#    cache failing to notice the player stepping west out of the origin chunk. Every positive
#    coordinate stays GREEN, which is what says these discriminate the sign and not the maths.
#  * B. the "used" test deleted from the box loop -> "FAILED - 7 of 81 checks":
#    L329/L330 (a released slot's stale coordinate blows the box past VIS_BOX_MAX_XZ and the
#    whole cull switches itself off), the three empty-pool refusal checks (L356-L358, an
#    unused slot now counts as content), and L703. The cull's correct answers on a clean pool
#    all stay GREEN -- the bug is invisible until the pool has been released from.
#  * C. the empty-pool early return made to set s_cave_ran first -> "FAILED - 1 of 81 checks",
#    L356 alone. One check, one line, nothing else disturbed: the narrowest arm here, and the
#    one that proves the refusal family is not just riding on the others.
#  * D. visWalkSet's drawable argument forced from the index_count test to a literal true ->
#    "FAILED - 1 of 81 checks", L422 "the empty chunk is reached but not drawable, so it is
#    not 'visible'". The mask half of the same insert stays GREEN, which separates "fed in at
#    all" from "fed in with the right drawable flag".
#  * E. the trailing cache validation deleted -> "FAILED - 13 of 81 checks", first L385, then
#    every cache-hit and re-run count (L490-L509 and on). The walk still computes the right
#    answer everywhere -- only its rate is wrong -- so the whole cull-correctness half of the
#    suite stays GREEN and it is the counting checks that go red.
#  * F. visgraph.c's faceLeadsAway FACE_EAST case made strict, its <= turned into < ->
#    "FAILED - 4 of 90 checks", L643, L644, L687 and L705, i.e. the cache-key family and
#    nothing else in the file. That family is the one covering the property caveWalk's frame
#    cache bets on: the cache is keyed on the camera's CHUNK, so the walk's answer must not
#    depend on where inside that chunk the camera stands. Read the note on
#    testCacheKeyIsSound (source/world/cavewalk_test.c:537) before touching a red here,
#    because this arm is what proves the family is about that comparison. The name matters:
#    through v1.8.1 this family was testCacheKeyIsUnsound, written to PIN a property that was
#    false; v1.8.2 made it true and renamed the family. Nothing called testCacheKeyIsUnsound
#    exists any more, so an older note pointing at it points at nothing.
#  * G. visWalkBegin's mask prefill changed from 0xFF to 0x00, so an unmeshed coordinate reads
#    as a WALL -> "FAILED - 1 of 81 checks", L468. This arm is why testHolesAreSeeThrough
#    exists: on the first pass it came back "PASS 77 checks, 0 failed", because every other
#    fixture in the file fills its box completely and the unmeshed sky -- the case
#    visgraph.h's header warns about by name -- was never in one.
#
# The controls that stayed green in EVERY arm, red ones included: "control: CHUNK_DIM is still
# 16", "control: the pool sentinels are intact after the widest legal box", "control:
# cullInvalidate drops the frustum cache too", "control: it blocks sight either way, so the
# mask reached the walk both times", "control: a hole the sealed chunk hides is still culled",
# "control: the sentinels survived the determinism runs", "control: the sentinels are intact
# after the far-negative box", "control: a coordinate outside the box answers visible",
# "control: visPairIndex numbers all 15 face pairs uniquely and symmetrically" and "control:
# the 'assume the worst' sentinel is still all-bits-set".
BCW="build-host/run-$$-cavewalk"
mkdir -p "$BCW"

awk '
/^static void cullInvalidate\(void\)$/ { inf=1 }
/^static int chunkOfFloat\(float v\)$/ { inf=1 }
/^static void caveWalk\(void\)$/       { inf=1; saw=1 }
inf { print; if ($0 == "}") { if (saw) done=1; inf=0 } }
END { if (saw && done) print "#define BS_CAVEWALK_EXTRACT_OK 1" }
' source/scene/chunk_render.c > "$BCW/cavewalk_extract.inc"

gcc -std=c11 -Wall -Wextra -Werror -O1 -g \
	-I source -I "$BCW" \
	source/world/visgraph.c \
	source/world/block.c \
	source/world/registry.c \
	source/world/chunk.c \
	source/world/budget.c \
	source/world/cavewalk_test.c \
	-lm \
	-o "$BCW/cavewalk_test"

"./$BCW/cavewalk_test"

rm -rf "$BCW"

# The CIA banner chime (roadmap task 44).
#
# tools/make_banner_audio.py synthesises the track the 3DS Home Menu plays out of the CIA
# banner, replacing the 0.1 s of printf'd silence tools/make_cia.sh used to hand bannertool.
# It is checked here rather than by ear because most of its failure modes are silent ones: a
# wrong sample rate still plays (at the wrong speed), a non-zero last sample still plays
# (with a click every time the Home Menu wraps), a drifting generator still produces a file
# (a different one every build, so a release stops being reproducible), and a chime whose
# tone has been lost still produces a perfectly well-formed WAV -- of four clicks.
#
# That last one is why the pitch arm exists. Without it this suite passed a file with every
# harmonic set to zero: the noise transients alone normalise to -3 dBFS and satisfy every
# header and endpoint check. A Goertzel at each note's own frequency, against a control
# 3 semitones below it, is what makes "there is a tune in here" a claim that can go red.
#
# What CANNOT be checked here, stated so the gap is not found later: whether it sounds
# right, and whether the Home Menu plays it at all. Azahar has no 3DS Home Menu, so a banner
# track never plays there. That one is hardware, and it is steve's.
#
# The determinism arm compares two fresh runs rather than a hardcoded hash, on purpose: the
# tune is an open decision (t44 blueprint 6.3) and a baked-in md5 would go red for a
# deliberate edit instead of for a bug. For the record, the current figure hashes to
# 37c42dfc9bd9b14050151dff4a3ed80f identically under Windows CPython 3.13.14 and WSL CPython
# 3.14.4 -- two different libm implementations, the same 70604 bytes.
#
# Sabotaged in the real generator and measured, each arm restored afterwards and the restore
# confirmed by md5 (make_banner_audio.py back to 48074164e6f9faa9dd8472fea946924a every
# time). Green arm before and after the whole run: "PASS 12 checks, 0 failed", exit 0.
#
#  * RATE 22050 -> 16000 -> "FAIL 12 checks, 1 failed", "sample rate is 22050 Hz (got
#    16000)". Note what stays GREEN, because it is a real limit of this suite: the duration
#    check does NOT go red, since frames scale with the rate and 1.6 s stays 1.6 s. It can
#    only ever catch a change to DURATION_S.
#  * END_FADE_S 0.030 -> 0.0 -> "FAIL 12 checks, 1 failed", "last sample is 0 (no click at
#    the loop point), got 20". Small, because the final note has decayed a long way by
#    1.6 s -- but 20 is still a step to zero, and an uncompressed banner track has nothing
#    to hide it behind.
#  * the LCG swapped for an unseeded random.randrange -> "FAIL 12 checks, 1 failed",
#    "two runs are byte-identical (got 70604 and 70604 bytes)". Same length, different
#    contents: every header, endpoint and pitch check stays GREEN.
#  * PEAK_DBFS -3.0 -> -6.0 -> "FAIL 12 checks, 1 failed", "peak normalised to -3 dBFS (got
#    -6.00 dBFS, 16422 of 32767)".
#  * HARMONICS reduced to ((1, 0.0),), i.e. the tone deleted and only the noise strikes left
#    -> "FAIL 12 checks, 4 failed", one per note, first "note at +0 semitones rings at
#    220.00 Hz, not at a control 3 semitones below it (ratio 0.70, want > 4)". Every header
#    and endpoint check stays GREEN -- this is the arm the suite used to pass.
#  * DEGREES (0, 7, 12, 4) -> (0, 5, 12, 4), one note moved a whole tone -> "FAIL 12 checks,
#    1 failed", the +7 note only. The other three notes stay GREEN, which is what says the
#    pitch arm discriminates one note rather than collapsing the fixture.
#
# $BS_PYTHON overrides the PATH search, the same knob and the same rule as
# tools/make_cia.sh: set-but-not-executable is fatal rather than a fall back to
# PATH, because testing a different interpreter than the one that was named
# would make this arm green about something nobody asked about. Unset, the PATH
# search below is unchanged, and a missing interpreter is still a SKIP.
if [ -n "${BS_PYTHON:-}" ]; then
	if [ ! -x "$BS_PYTHON" ]; then
		echo "banner chime: FAILED - \$BS_PYTHON is set to '$BS_PYTHON', which is not an executable file. Point it at a real Python interpreter, or unset it to search \$PATH instead."
		exit 1
	fi
	PY_CHIME="$BS_PYTHON"
else
	PY_CHIME="$(command -v python3 || command -v python || true)"
fi
if [ -z "$PY_CHIME" ]; then
	echo "banner chime: SKIPPED - no python3/python on PATH and \$BS_PYTHON is not set. This is a GAP, not a pass:"
	echo "  tools/make_banner_audio.py was not run, so the .wav make_cia.sh feeds bannertool is unchecked."
else
	BHCH="build-host/run-$$-chime"
	mkdir -p "$BHCH"

	"$PY_CHIME" tools/make_banner_audio.py "$BHCH/a.wav" > /dev/null
	"$PY_CHIME" tools/make_banner_audio.py "$BHCH/b.wav" > /dev/null

	"$PY_CHIME" - "$BHCH/a.wav" "$BHCH/b.wav" <<'CHIME_PY'
import math
import struct
import sys
import wave

a, b = sys.argv[1], sys.argv[2]
checks = 0
failed = 0


def check(ok, what):
    global checks, failed
    checks += 1
    if not ok:
        failed += 1
        print("banner chime: FAILED - %s" % what)


with open(a, "rb") as fa, open(b, "rb") as fb:
    ba, bb = fa.read(), fb.read()
check(ba == bb, "two runs are byte-identical (got %d and %d bytes)" % (len(ba), len(bb)))

with wave.open(a, "rb") as w:
    rate, ch, width, frames = w.getframerate(), w.getnchannels(), w.getsampwidth(), w.getnframes()
    data = w.readframes(frames)

check(rate == 22050, "sample rate is 22050 Hz (got %d)" % rate)
check(ch == 1, "mono (got %d channels)" % ch)
check(width == 2, "16-bit (got %d-bit)" % (width * 8))
check(abs(frames / rate - 1.6) < 1e-6, "1.6000 s long (got %.4f s)" % (frames / rate))

samples = struct.unpack("<%dh" % frames, data)
check(samples[0] == 0, "first sample is 0 (no click on start), got %d" % samples[0])
check(samples[-1] == 0, "last sample is 0 (no click at the loop point), got %d" % samples[-1])

peak = max(abs(s) for s in samples)
db = -999.0 if peak == 0 else 20.0 * math.log10(peak / 32767.0)
check(abs(db + 3.0) < 0.05, "peak normalised to -3 dBFS (got %.2f dBFS, %d of 32767)" % (db, peak))


def goertzel(sig, f):
    """Magnitude of one frequency bin. Standard library only, like the generator."""
    k = 2.0 * math.cos(2.0 * math.pi * f / rate)
    q1 = q2 = 0.0
    for x in sig:
        q0 = k * q1 - q2 + x
        q2, q1 = q1, q0
    return math.sqrt(abs(q1 * q1 + q2 * q2 - k * q1 * q2)) / len(sig)


# Each note, measured in its own 100 ms window starting 8 ms after its onset (past the
# attack, before the next onset at +160 ms). The control is 3 semitones below the note: not
# a partial of anything else playing, so it reads the noise floor rather than the tune.
# Measured margins on the current figure: 11.95, 17.42, 70.32, 13.92 against a threshold of 4.
ROOT_HZ = 220.0
for degree, onset in zip((0, 7, 12, 4), (0.00, 0.16, 0.32, 0.48)):
    f = ROOT_HZ * 2.0 ** (degree / 12.0)
    lo = int((onset + 0.008) * rate)
    win = samples[lo:lo + int(0.10 * rate)]
    here = goertzel(win, f)
    ctrl = goertzel(win, f * 2.0 ** (-3.0 / 12.0))
    ratio = here / ctrl if ctrl > 0 else float("inf")
    check(ratio > 4.0, "note at +%d semitones rings at %.2f Hz, not at a control 3 semitones "
                       "below it (ratio %.2f, want > 4)" % (degree, f, ratio))

if failed:
    print("banner chime: FAIL %d checks, %d failed" % (checks, failed))
    sys.exit(1)
print("banner chime: PASS %d checks, 0 failed" % checks)
CHIME_PY

	rm -rf "$BHCH"
fi

# world/water_alpha_test.c -- SEE-THROUGH water (v1.8.2, blueprint ask 4). Own binary, own
# main(), appended rather than merged into the water-mesh stanza above it for the reason every
# stanza in this file is appended: an append is the one edit shape that cannot drop another
# session's work, and this file is shared.
#
# Separate from water_mesh_test.c on purpose, and the split is the same one that suite already
# makes against water_test.c: water_test owns where the water GOES, water_mesh_test owns the
# SHAPE the mesher gives it, and this one owns what colour comes out the other end. The link
# sets differ -- this binary links nothing but block.c, registry.c and a header -- and a failure
# in one must not be able to hide behind another's totals.
#
# What it settles, none of which is geometry and none of which can fail loudly on its own:
#
#   * meshNrmAlpha (world/mesher.h) gives 1.0 to the eight faceShade rows at drop 0 and
#     WATER_ALPHA to the 56 rows above them. The REAL function is linked, not a copy of it --
#     WATER_ALPHA lives in that header precisely because scene/chunk_render.c includes <3ds.h>
#     and no host test can link it -- and the renderer is then checked BY TEXT to call
#     meshNrmAlpha rather than to carry its own copy of the ternary. Without that second half
#     this suite would be proving things about dead code, the exact defect recorded against
#     app/battery.c and app/sleep.c further up this file.
#   * 0.70 clears the alpha TEST, with ALPHA_CUTOFF parsed out of scene/chunk_render.c rather
#     than restated. This is the check that matters most in practice: an alpha under the cutoff
#     does not make water faint, it makes water VANISH and the seabed show through a hole. A
#     leaf's cutout texel (tex.a 0) is asserted to still be discarded, as the control.
#   * BOTH source/shaders/world.v.pica and world_dynamic.v.pica read the alpha out of the same
#     table row (`mov outclr.w, r3.wwww`) and neither still nails it to 1.0. world_dynamic is
#     the program actually bound on both console models since the v1.8.0 shader split, and
#     world.v.pica is compiled and never bound, so an edit to only one of them is invisible both
#     on the hardware and to the next person reading the file. Same drift world/
#     atlas_uv_shader_test.c guards uvScale against, and each file's uvScale literal is
#     re-checked here as a control.
#   * scene/chunk_render.c owns the blend unit at BOTH ends -- ONE/ZERO armed in pipelineBind
#     for the opaque pass, SRC_ALPHA/ONE_MINUS_SRC_ALPHA armed inside the transparent pass and
#     put back to ONE/ZERO after it. Before v1.8.2 the world draw never called C3D_AlphaBlend at
#     all and gfx/sprite.c's spriteBegin was the only caller in the tree, so the world had been
#     drawing with whatever the last HUD batch left standing in citro3d's persistent context --
#     harmless only while every vertex alpha in the game was 255. The ordering (armed after the
#     alpha test, restored before scene/highlight.c and scene/crackoverlay.c draw) is asserted
#     by line number, not just by presence.
#
# Sabotaged in the REAL production sources, each restored afterwards and every restore verified
# byte-identical by md5. Every FAIL line of every red run was read, not just the first:
#
#   * WATER_ALPHA 0.70f -> 0.40f in source/world/mesher.h:140 -> "FAILED - 1 of 34 checks",
#     "FAIL L237  water is 102 > 127, so the surface DRAWS (under it, water vanishes entirely)".
#     Everything else stays GREEN, including all 64 table rows -- which is the point: the table
#     is perfectly self-consistent at 0.40 and the water is invisible anyway. That one check is
#     the only thing standing between a plausible-looking constant and a sea you fall into.
#   * `mov outclr.w, r3.wwww` reverted to `mov outclr.w, ones` in world_dynamic.v.pica ONLY ->
#     "FAILED - 3 of 34 checks", first "FAIL L261  source/shaders/world_dynamic.v.pica writes
#     the vertex alpha from the table exactly once (found 0)". world.v.pica's three checks stay
#     green, which is the whole reason this test reads both files.
#   * the transparent pass's C3D_AlphaBlend deleted from scene/chunk_render.c -> "FAILED - 2 of
#     34 checks", "FAIL L307  three C3D_AlphaBlend calls: two ONE/ZERO and one SRC_ALPHA (got 2,
#     2, 0)". The table half stays green: nothing about the uniform changed, the GPU simply
#     never blends it.
#
# Controls green in EVERY arm, red ones included: "control: a leaf's cutout texel is 0 and is
# still discarded", both files' "control: ... uvScale literal is untouched", and "control:
# gfx/sprite.c still sets SRC_ALPHA blending for its own batch".
#
# ── The other half of v1.8.2, which lands in the water_mesh_test.c stanza above ──────────────
#
# Ask 2 (the recessed surface) had to ship in the same change as ask 4 and its checks went into
# world/water_mesh_test.c rather than here, so its numbers are recorded here to keep them with
# their sibling: 44 checks -> 69, three new probes (testOceanSurfaceIsShort,
# testOceanSurfaceStillMerges, testNoGapAtTheColumn, plus testScratchFillFlagsWater on the real
# scratchFill). Two arms, both against real production source:
#
#   * dropBuild's surface floor reverted to `s_cell_drop[i] = nat;` in world/mesher.c:1039 ->
#     "FAILED - 10 of 63 checks", first "FAIL   line 267: a source with sky above it is drawn
#     7/8 tall, not flush with the block beside it".
#   * the has_water half of dropBuild's gate deleted (`if (!s->water_any) return;`) ->
#     "FAILED - 6 of 63 checks". The staircase probes stay green at faces=47 hash=0xac0babdd
#     because a staircase has flow levels; only the SOURCE fixtures go red, which is exactly the
#     case the flag exists for.
#   * the memchr in world/scratch.c:86 neutered -> "FAILED - 2 of 69 checks", "FAIL line 894:
#     scratchFill raises the flag on a window full of sources".
#
# NOTE for whoever reads the water_mesh_test.c stanza above: its closing sentence used to claim
# "a world of SOURCES and a world of ordinary terrain still mesh byte-identically to v1.7.1",
# which went half stale in v1.8.2. That stanza HAS now been corrected in place -- the TERRAIN
# half is still the control and is still pinned (faces=88 hash=0x9f47e02f), the SOURCES half
# deliberately moved because a source with sky above it is now 7/8 tall, and testSourceUnchanged
# asserts the new height instead. Recorded here as well because this is where its arms live.
BHWA="build-host/run-$$-wateralpha"
mkdir -p "$BHWA"

gcc -std=c11 -Wall -Wextra -Werror -O1 -g \
	-I source \
	source/world/block.c \
	source/world/registry.c \
	source/world/water_alpha_test.c \
	-o "$BHWA/water_alpha_test"

"./$BHWA/water_alpha_test"

rm -rf "$BHWA"

# world/region_growth_test.c -- region files grew without bound, and now do not (v1.8.2). Own
# binary, own main(), appended rather than merged into any stanza above it for the reason every
# stanza in this file is appended: an append is the one edit shape that cannot drop another
# session's work, and this file is shared.
#
# It links the REAL source/world/region.c and drives 960 column saves through it, stat()ing the
# file on disk as it goes, because the bug this covers is a number on a card and not a property
# of a diff. tests/net_stub.c is in the link only because world.c's worldSet calls
# networldOnColumnLoad. This is the suite that made the case for wiring regionCompact() up; the
# header paragraph near the top of this file that said it was "still not wired to anything" has
# been corrected to match, and so has source/world/region.h.
#
# HOW BAD IT WAS. regionCompact() had no caller -- confirmed by grep over the whole tree, not by
# reading. 24 distinct columns inside region (0,0), each re-saved 40 times with payloads that
# lengthen the way a column being built in lengthens (real chunkEncode, real regionEncodeColumn,
# scattered single-block edits on top of generated terrain), 960 saves in all, stat()ing
# r.0.0.bsr as it goes:
#
#   saves    file bytes   live bytes   dead
#   24       10773        4597         0.0 %
#   264      78259        8338         88.4 %
#   624      247633       13910        94.2 %
#   960      482885       19452        95.9 %
#
# Still climbing linearly at the end of the run; an append-only arena has no plateau. With
# regionMaintain() wired into app/worker.c's workerWriteSave the same workload finished at 43883
# bytes -- 11x smaller, oscillating around twice-live instead of climbing -- having compacted 36
# times across 960 saves. 43883 is a stat() of r.0.0.bsr taken AFTER the 960th save and after the
# regionMaintain that follows it -- final, not peak; in this arm peak and final are both 43883.
# Bit-identical on three consecutive runs (2026-08-25), so the single figure is safe to quote.
#
# 2026-08-25 RECONCILIATION. source/world/region.h and source/world/region.c said 21763 bytes,
# 22x, 56 compactions for what they called this same workload. That figure is NOT this workload:
# it is the first-draft fixture, cx=(i*7)&15 / cz=(i*5)&15, whose 16-period aliased columns 16..23
# onto the slots of 0..7, leaving only 16 columns live -- so regionCompact's half-dead gate fired
# sooner and the file settled smaller. Reproduced on purpose from a build-dir COPY of
# region_growth_test.c with the old placement restored, region.c untouched: 482885 / 21763 / 56
# compactions, control_readback 8 of 24 columns wrong in BOTH arms, exit 1. It was never a sound
# run. The no-compaction arm is identical either way (the same 960 payloads get appended), which
# is why 482885 and 19452 were right the whole time and only the compacted figure moved. region.h
# has been corrected to 43883 / 11x / 36 and now states its workload and its measurement point.
# source/world/region.c's header block carried the same stale figures and has been corrected to
# match, comment-only -- see HASH CHANGED below for why its md5 moved and the proof that the
# generated code did not. source/app/worker.c carried the same error in derived form -- its
# "sixteen saves out of seventeen" is 960/56 -- and is now twenty-six out of twenty-seven (960/36),
# also comment-only, also recorded under HASH CHANGED. Its 482885 / 19452 / 95.9 % figures were
# never wrong and are untouched. A FOURTH copy of the same derived ratio turned up in the final
# sweep at source/world/region.h:200, on regionMaintain's own declaration -- "sixteen saves out of
# seventeen" again -- and is corrected the same way. It was found by grepping for the DERIVED
# wording (sixteen/seventeen) rather than for the headline figures, which is the only reason it
# surfaced: a wrong number does not have to contain any of the digits you are searching for.
# After that pass the tree holds exactly ONE set of figures for
# this workload: 482885 uncompacted, 43883 compacted, 11x, 36 compactions, 19452 live. Every
# remaining occurrence of 21763 / 22x / 56 in source/ or tools/ sits inside a paragraph whose
# subject is that those numbers were a broken fixture and must not be reused.
#
# The fix also changed regionCompact's swap from remove(.bsr)+rename(.tmp,.bsr) to
# rename(.bsr,.bak)+rename(.tmp,.bsr)+remove(.bak), which is what made wiring it up safe: the old
# sequence left an instant with no file at the path and 256 columns living only in a .tmp that
# nothing here fsyncs. regionRecover() now resolves .bak first. REGION_VERSION is untouched and
# the .bsr bytes are unchanged.
#
# HASH CHANGED 2026-08-25, and here is the reason, because a hash that moves with no recorded
# cause is indistinguishable from tampering to the next reader. source/world/region.c went from
#   old  09386ae6ae3bd457a3f177a85e7193c5   (the value the sabotage arms below were restored to)
#   new  603d3d786315746ef3dffdf7716bc5b1   (current)
# for a COMMENT-ONLY correction: its header block carried the same wrong 21763/22x/56 figures the
# reconciliation above describes, and they are now 43883/11x/36 with the workload and measurement
# point spelled out. No code, no whitespace reflow. PROVED comment-only rather than asserted --
# region.c compiled before and after with `gcc -std=c11 -Wall -Wextra -Werror -O1 -I source`, both
# to assembly (-S) and to an object (-c), and both outputs are byte-identical across the edit:
#   region.s  d3b484f407a5302965142aae3d440898  (before == after)
#   region.o  6f9449f9ab5bd6649945f8c78c5bcfff  (before == after)
#
# source/app/worker.c changed on the same date, for the same reason, and went from
#   old  f4518839240fa8465ad13f4244979882
#   new  1b9c24d455536dd6d3d9779ee7e749ad   (current)
# Its workerWriteSave comment said the rewrite costs "two file probes and a directory read on
# sixteen saves out of seventeen", which is 960/56 off the bad fixture; it now reads twenty-six
# out of twenty-seven and carries its own derivation (960/36) so it cannot drift from the
# compaction count again. The 482885 / 19452 / 95.9 % figures in that same comment were never
# wrong and are untouched. Comment text only, no code, no whitespace reflow.
# worker.c includes <3ds.h>, so host gcc cannot compile it and the proof above could not be run
# with it. It was compiled instead with devkitARM -- the compiler the console build itself uses --
# at the ARCH and INCLUDE flags out of the project Makefile (lines 28 and 45), compile only:
#   arm-none-eabi-gcc -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -mword-relocations
#     -Wall -Wextra -O3 -D__3DS__ -I source -I deps/libhydrogen -I deps/blocksmith-server
#     -I $DEVKITPRO/libctru/include -I $DEVKITPRO/portlibs/3ds/include
# Both outputs byte-identical across the edit, -g excluded for the same reason as region.c:
#   worker.s  0d6f0ea42af6905eb8ef5664ac6f16d2  (before == after)
#   worker.o  eacf8ef2a12931b4e2ae0b3de4894acc  (before == after)
# Note this is a compile of ONE translation unit for the hash comparison and nothing more -- it
# is not a console build, and the standing rule that console builds run only from devkitPro MSYS2
# is unaffected.
# -g was deliberately NOT used for that comparison: the corrected comment is longer than the one
# it replaced, so every line number below it shifts, and DWARF would record that difference for a
# reason that has nothing to do with generated code. The assembly and the non-debug object carry
# no line table and no timestamp, so they isolate exactly the thing being claimed.
#
# Sabotaged in the REAL source/world/region.c and measured, each arm restored afterwards and the
# restore confirmed by md5 (region.c back to 09386ae6ae3bd457a3f177a85e7193c5 -- the pre-2026-08-25
# value, see HASH CHANGED above). Green control on
# both sides of every arm: "region growth: PASS  2494 checks", exit 0. The named control that
# must stay GREEN in every arm is control_readback -- every column reads back byte for byte --
# because it is a correctness claim and not a size one.
#
#  * regionMaintain neutered so compaction never runs. Written as
#    `if (world_dir == NULL || rx == INT32_MIN || rz == INT32_MIN)` around the regionCompact
#    call, which reads all three parameters -- an `if (0)` is rejected outright by
#    -Werror=unused-parameter, and a sabotage that does not compile proves nothing (the same
#    note the meshq and task-48 arms at the top of this file record).
#      -> "FAILED - 1 of 2494 checks", "FAIL L404 bounded < ceiling", summary line read:
#      no-compaction 482885 bytes, compaction-wired 482885 bytes. control_readback GREEN in both
#      arms (0 of 24 wrong), bak_state1 and bak_state2 GREEN. Exactly the one claim, nothing else.
#
#    RE-RUN when this stanza was wired into the suite, and the same arm re-measured through the
#    RUNNER rather than by hand, to prove a red here reaches the script's exit code and is not
#    swallowed by set -e running on past it: "region growth: FAILED - 1 of 2494 checks",
#    "FAIL L410 bounded < ceiling", "bound: compacted file must be under 64532 bytes", summary
#    "no-compaction 482885 bytes, compaction-wired 482885 bytes, live at end 19452",
#    "peak file bytes: 482885   final: 482885   compactions: 0", and RUNNER_EXIT=1. The three
#    named controls stayed green in that run -- control_readback 0 of 24 columns wrong,
#    bak_state1 0 of 24, bak_state2 0 of 24. The failing line is L410 and not the L404 recorded
#    above it: region_growth_test.c moved between the two measurements, and the line number is
#    quoted from the run that produced it rather than carried forward.
#
#    The sabotage was applied WITHOUT writing to source/world/region.c, which a parallel session
#    owned at the time: this stanza was temporarily pointed at a sed'd copy of region.c in its
#    own build directory, wrapping the regionCompact call in
#    `if (world_dir == NULL || rx == INT32_MIN || rz == INT32_MIN)`. region.c was never opened
#    for writing and was still 09386ae6ae3bd457a3f177a85e7193c5 at the end of that arm. It is
#    603d3d786315746ef3dffdf7716bc5b1 today, changed only by the comment-only correction recorded
#    under HASH CHANGED above, whose generated code is byte-identical -- so this arm's result
#    stands unchanged against the current file.
#  * regionRecover's .bak restore turned into a drop (`rename(bak, src)` -> `remove(bak)`), i.e.
#    a cut between the two renames loses the region.
#      -> "FAILED - 2 of 2494 checks", bak_state1 (bak, no bsr, torn tmp): 24 of 24 columns
#      wrong, "FAIL L332 readAll() == 0" and "FAIL L333 fileBytes(bsr) == good".
#      control_readback GREEN, bak_state2 GREEN, the size bound GREEN. So the size claim and the
#      crash-safety claim are separable, which is the point of having both.
#
# A fixture bug caught on the way and worth recording: the first draft placed the 24 columns at
# cx=(i*7)&15, cz=(i*5)&15, whose period in i is 16, so columns 16..23 sat on the same directory
# slots as 0..7. control_readback reported "8 of 24 columns wrong" in BOTH arms -- a fixture
# defect that looked exactly like a region.c defect. The distinctness of the 24 slots is now
# asserted in spreadColumns() rather than assumed.
#
# NOT MEASURED, and it is the honest half. Every number above is a host number on ext4 through
# WSL. A rewrite of a fully built 256-column region moves on the order of 200 KB each way and
# nobody has timed that on a FAT32 SD card behind libctru's devoptab at 268 MHz. If it is slow
# enough, three column saves landing inside one rewrite would fill worker.c's two-slot save ring
# and make the MAIN thread wait in workerSubmitSave; workerSaveWaits() in the debug overlay is
# where that would show. rename() on that devoptab is also still unverified -- but it now fails
# SAFE: if it does not work at all, the first rename fails, nothing has been touched, and the old
# .bsr is still there. The old sequence had already removed it by then.
BHRG="build-host/run-$$-regiongrowth"
mkdir -p "$BHRG"

gcc -std=c11 -Wall -Wextra -Werror -O1 -g \
	-I source \
	source/world/block.c \
	source/world/registry.c \
	source/world/chunk.c \
	source/world/chunk_codec.c \
	source/world/crc32.c \
	source/world/region.c \
	source/world/world.c \
	source/world/budget.c \
	tests/net_stub.c \
	source/world/region_growth_test.c \
	-lm \
	-o "$BHRG/region_growth_test"

"./$BHRG/region_growth_test"

rm -rf "$BHRG"

# tests/light_luminance_test.c -- BlockDef.luminance reaching world/light.c (v1.8.2). Own binary,
# own main(), appended for the same reason every stanza here is appended.
#
# The field has existed since v1.6.0 and has always crossed the wire (registry.c packs it at byte
# 24 of a DEFS record and unpacks it straight back), but nothing on the receiving side ever fed
# it to the lighting engine -- light.c read a private table whose only writer was
# lightSetLuminanceForTest. A server that registered a glowing block at join therefore sent its
# luminance, the client stored it in the def, and the block rendered pitch dark with no error and
# no log line. Same class of bug as the anyLuminance() loop bound recorded at the top of this
# file, one layer further out: there the table was too narrow, here nothing ever wrote it.
#
# Same link list as scratch_light_test above -- world.c #includes light.c (see its line 16), so
# light.c must NOT be listed separately or it is defined twice.
#
# Five arms, 107 checks. Arms 1 and 2 register an emitter into the DYNAMIC id space through
# registryRegister and registryRemoteApply -- the two calls the join path ends in -- and measure
# the falloff; nothing adds a luminous row to the shipped core table, so the pinned core crc16
# (0x4066, registry_test) cannot move. Arm 0 asserts no core row declares a luminance and an
# ordinary block still lights nothing; arm 3 asserts a defined but unplaced emitter lights
# nothing.
#
# Arm 4 is coverage the shipped suite never had: world_test.c's testLightBlockChannel and
# testLightBlockChannelFromDynamicId both place an emitter, assert the lit values and tear the
# world down -- neither ever BREAKS the block and re-reads, and un-lighting is the classic
# failure mode of a light engine. It drives the engine through lightSetLuminanceForTest rather
# than the registry, which also makes it the named control for the sabotage run: with the
# registry read deleted from light.c's syncLuminance(), arms 1 and 2 go red with 20 verbatim
# "FAIL L159: [register|defs] block light at distance N = 0, want M" lines and arms 0, 3 and 4
# stay green (87 of 107).
#
# RE-RUN through the RUNNER when this stanza was wired in, to prove a red here reaches the
# script's exit code rather than being swallowed: the same sabotage gave exactly those 20 FAIL
# lines, "light luminance self-test: FAILED - 87 of 107 checks", and RUNNER_EXIT=1. Arm 3 (a
# defined but unplaced emitter) and arm 4 (the test-hook control: placed 7 6 5 4 3 2, broken
# 0 0 0 0 0 0, replaced 7 6 5 4 3 2) both stayed green, which is what says the arms discriminate
# the registry read rather than the whole engine collapsing.
#
# READ THAT SUMMARY LINE CAREFULLY. tests/light_luminance_test.c:349 prints s_checks - s_fail,
# so "FAILED - 87 of 107 checks" means 87 PASSED and 20 failed. It is not 87 failures. Count the
# FAIL lines, not the number in the summary -- the other suites in this file print the failure
# count there and the two conventions are one glance apart.
#
# The sabotage was applied WITHOUT writing to source/world/light.c, which a parallel session
# owned at the time. world.c pulls the engine in with `#include "world/light.c"` (see world.c
# line 16), so a sed'd copy was written to this stanza's own build directory under world/ and
# reached with a -I placed ahead of -I source. light.c was never opened for writing and is still
# 6602977812703fa64c986d47e814b7dd.
BHLL="build-host/run-$$-luminance"
mkdir -p "$BHLL"

gcc -std=c11 -Wall -Wextra -Werror -O1 -g \
	-I source \
	source/world/world.c \
	source/world/block.c \
	source/world/registry.c \
	source/world/chunk.c \
	source/world/budget.c \
	source/world/scratch.c \
	tests/net_stub.c \
	tests/light_luminance_test.c \
	-o "$BHLL/light_luminance_test"

"./$BHLL/light_luminance_test"

rm -rf "$BHLL"

# world/tick_test.c -- the 20 TPS simulation clock (v1.8.3). Own binary, own main(), appended
# rather than merged into the world stanza for the reason every stanza in this file is appended:
# an append is the one edit shape that cannot drop another session's work, and this file is
# shared. The link is world/tick.c and nothing else -- tick.c includes only its own header and
# has no dependencies at all, which is what lets both the console client and the Linux dedicated
# server compile it from the same source.
#
# world/tick.c IS MIRRORED. deps/blocksmith-server/tools/sync-world-sources.sh vendors it as one
# of eleven byte-identical copies into deps/blocksmith-server/game/world/, and the Makefile's
# check-world-drift target fails the build for everyone the moment the two diverge. That is why
# untested logic in this file is worse than untested client-only logic: a disagreement here does
# not produce a visible bug, it desyncs players.
#
# WHY THIS EXISTS WHEN world_test.c ALREADY HAD A TICK BLOCK. It did, and it is a good one --
# world_test.c:7176 testTickClock() owns the bank/remainder contract, the 20-ticks-per-second
# rate, a ten-second drift check, the server's sleep-exactly-as-long loop, one catch-up clamp
# event and its recovery, the backwards-clock rule, the max_catchup<=0 clamp, NULL safety on all
# five clock entry points, and tickPeriodForDistSq's 575/576/577 boundary. None of that is
# repeated here. What was MEASURED to be missing, and is what this file covers:
#
#   * tickDue() at the period production actually uses. testTickClock exercises periods 0, 1 and
#     TICK_FAR_PERIOD (10). The dedicated server calls tickDue(t, BS_POS_BROADCAST_PERIOD, 0)
#     once per tick at deps/blocksmith-server/game/bsgame.c:1306, and BS_POS_BROADCAST_PERIOD is
#     TICK_HZ / 10 = 2. Period 2 is the ONLY period tickDue is called with anywhere in shipped
#     code and it had no coverage at all. It gates the 10 Hz position broadcast to every client
#     in the room.
#   * The stagger as a PATTERN rather than as a count. testTickClock proves a decimated thing
#     fires twenty times in two hundred ticks and that ten consecutive ids partition one period.
#     Both survive a wrong PHASE. Ten hand-written literal bit strings pin which ticks are due.
#   * Unbounded ids and large tick numbers -- tick.h promises `id` need not be "dense or
#     bounded"; nothing tested an id above 12345 or a tick above 200.
#   * The conservation law: count + dropped == the whole ticks the forward time fed in was worth,
#     across a stream that clamps repeatedly. Time must be either simulated or explicitly counted
#     as lost, never quietly evaporated.
#   * Naked-literal pins for the seven constants. Every existing assertion about the clamp is
#     written in terms of TICK_MAX_CATCHUP_DEFAULT itself, which is this project's recorded
#     case-3 trap -- a check parameterised by the value under test cannot detect that value
#     changing.
#
# THE MEASUREMENT THAT MOTIVATED THE FILE, and the reason it is quoted here rather than
# summarised. tickDue's `== 0` was changed to `== 1` -- a pure PHASE SHIFT, which leaves every
# firing count and every partition property in the existing suite exactly as it was. Built at
# HEAD, linked against a private byte-identical copy of the sabotaged tick.c (the working tree's
# world_test.c/worldgen*.c were mid-edit by a parallel session and did not compile):
#
#     healthy    world self-test: PASS  5184 checks
#     sabotaged  world self-test: PASS  5184 checks
#
# `cmp` on the two captured outputs was SILENT -- byte-identical, zero FAIL lines, 5184 both
# ways -- while `cmp` on the two linked ELFs showed they DIFFER, so the mutation reached the
# build and not merely the file. The same shape as the shader case: a large green suite over a
# broken shared behaviour. TICK_MAX_CATCHUP_DEFAULT 4 -> 12 (applied as a shadow header on a
# -I ahead of -I source, so the real tick.h was never written to) also gave PASS 5184.
#
# ARMS. Every one against the REAL, MIRRORED source, restored immediately afterwards, with md5
# back to baseline (tick.c 55360674be6c1df5ace9ef9ce4c2bd7d, tick.h
# 8e81df022b1f5c2b570dba14034650ce) and `cmp` against deps/blocksmith-server/game/world/ SILENT
# for both files after every arm. Which checks went red was read, not just how many, and the
# count stayed 52 in all four -- nothing was deleted:
#
#   A  tickDue `== 0` -> `== 1` (the phase shift above)        FAIL 52 checks, 14 failed
#      All ten pattern rows plus the four tick-10^12 phase checks. The period-10 PARTITION check
#      stayed GREEN, which is what says the arm is a phase shift and not a wholesale break -- and
#      is precisely why a partition-shaped check could not catch it.
#   B  the `c->dropped += ...` line deleted from the clamp     FAIL 52 checks, 6 failed
#      "twenty-two ticks were dropped across the stream", both conservation checks, both
#      max_catchup drop counts, and "falsifiability: the stream clamped on more than 20 steps".
#      Every count-of-ticks-RUN check stayed green: the clock still runs the right ticks, it just
#      stops admitting what it threw away.
#   C  `c->accum_us -= n * TICK_PERIOD_US;` -> `c->accum_us = 0;`   FAIL 52 checks, 8 failed
#      first "FAIL the seven advances return 4, 4, 1, 0, 0, 1, 1", then the 74,999/25,001 split
#      tick, then all four drift checks. tickDue stayed entirely green.
#   D  TICK_MAX_CATCHUP_DEFAULT 4 -> 12 in the real tick.h     FAIL 52 checks, 1 failed
#      "FAIL TICK_MAX_CATCHUP_DEFAULT is 4 -- the death-spiral clamp, pinned nowhere else in the
#      tree". Exactly one check, which is the point: that constant had no naked-literal pin
#      anywhere in the tree, and the 5184-check world suite does not move for it either.
#
# A stagger-DELETING arm (`(tick + id) % period` -> `tick % period`) was tried first and is NOT
# usable: it makes `id` unused and -Wextra -Werror rejects the build. A sabotage that does not
# compile is not an arm. The phase shift is the stronger mutation anyway.
#
# CONTROL, green in all four arms: "control: a freshly initialised clock is at tick 0, has
# dropped nothing, and has banked nothing". Proved NOT vacuous by pointing its expected tick
# count at 99 on an otherwise healthy tree, which gave "FAIL 52 checks, 1 failed" naming that
# control. Reverted.
#
# The check-count pin was also made to fail on purpose before being trusted -- a checker that has
# never been red is an untested function that runs at the worst possible moment. The pattern-table
# loop bound was cut from 10 to 5, deleting five checks rather than failing them:
# "FAIL 47 checks, 2 failed", "FAIL CHECK COUNT: 5 check(s) WENT MISSING - expected 52, ran 47."
# The in-table falsifiability counter went red independently on the same run, so a deletion there
# has two unrelated detectors.
#
# RE-RUN THROUGH THE STANZA ITSELF once it was wired in, to prove a red here reaches the script's
# exit code rather than being swallowed. The stanza was sliced back out of THIS file (not
# retyped -- a retyped copy proves nothing about the committed text), run under `set -e` as the
# real script is, with arm D applied: "FAIL 52 checks, 1 failed", RUNNER_EXIT=1, and the echo
# placed after the stanza never printed. Healthy, the same slice gives PASS 52 checks, 0 failed.
BHTK="build-host/run-$$-tick"
mkdir -p "$BHTK"

gcc -std=c11 -Wall -Wextra -Werror -O1 -g \
	-I source \
	source/world/tick.c \
	source/world/tick_test.c \
	-o "$BHTK/tick_test"

"./$BHTK/tick_test"

rm -rf "$BHTK"
