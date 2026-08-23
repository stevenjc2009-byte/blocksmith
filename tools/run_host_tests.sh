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

# Guards source/shaders/world.v.pica's uvScale constant against source/world/atlas_uv.h's
# ATLAS_PX ever drifting apart again — the exact bug step 9.3c left behind (shader left at
# 1/256 after the atlas shrank from 256x256 to 64x64, so every tile's UV collapsed into one
# cell and the whole world rendered flat brown). A .pica file has no #include and the
# picasso shader compiler has no _Static_assert, so the world/block_tiles_check.c trick
# (duplicate the value on both sides, then _Static_assert the two copies agree) does not
# apply here; this test parses the shader source text directly and checks the literal
# against ATLAS_PX instead. Sixth binary for the same reason ui_layout_test is a fifth one:
# it carries its own main().
gcc -std=c11 -Wall -Wextra -Werror -O1 -g \
	-I source \
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
# -DBS_CLIENT_HAS_METERS=1 flips this build's copy of net/networld.h's compile-honest meter
# gate so test_send_player_report_encodes() can reach the PLAYER_REPORT encoder at all: in
# every other build (console included) the gate is 0 and the encoder does not exist, because
# sending zeros would wipe a returning player's saved state — see that header. Flipping it
# HERE and only here is what lets the encode path stay tested while production keeps it off.
gcc -std=c11 -Wall -Wextra -Werror -O1 -g -DBS_CLIENT_HAS_METERS=1 	-I source -I deps/blocksmith-server 	source/world/world.c 	source/world/block.c 	source/world/registry.c 	source/world/chunk.c 	source/world/budget.c 	source/net/blockdiff.c 	source/net/networld.c 	source/world/mesher.c 	source/world/scratch.c 	source/net/networld_test.c 	-o "$BH/networld_test"

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

# Only reached if every binary above exited 0 (set -e stops the script on the first
# non-zero exit), so a failed run's directory is left behind for inspection rather than
# silently deleted along with the evidence of what failed.
rm -rf "$BH"
