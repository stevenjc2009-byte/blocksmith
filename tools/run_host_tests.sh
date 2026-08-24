#!/bin/sh
# Compiles the world data code with the host compiler and runs its self-test.
#
# Everything under source/world is plain C with no 3DS dependency, so it can be
# tested here in a second instead of through a devkitARM build and an emulator.
# The file list is explicit rather than a glob because block_tiles_check.c includes
# <3ds.h> — it is the console-only bridge that checks the two duplicated enums.
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
	source/world/visgraph.c \
	source/world/worldgen.c \
	source/scene/render_dist.c \
	source/world/world_test.c \
	-lm \
	-o "$BH/world_test"

"./$BH/world_test"

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
# a skip: skipping would leave the atlas's art unchecked, which is where the defect lived.
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
gcc -std=c11 -Wall -Wextra -Werror -O1 -g -DBS_CLIENT_HAS_METERS=1	-I source -I deps/blocksmith-server 	source/world/world.c 	source/world/block.c 	source/world/registry.c 	source/world/chunk.c 	source/world/budget.c 	source/net/blockdiff.c 	source/net/networld.c 	source/world/mesher.c 	source/world/scratch.c 	source/app/session.c 	source/net/networld_test.c 	-o "$BH/networld_test"

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

gcc -std=c11 -Wall -Wextra -Werror -O1 -g 	-I source -I deps/blocksmith-server 	source/world/world.c 	source/world/block.c 	source/world/registry.c 	source/world/chunk.c 	source/world/budget.c 	source/net/blockdiff.c 	source/net/networld.c 	source/world/mesher.c 	source/world/scratch.c 	source/net/interop_test.c 	-o "$BH/interop_test"

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
