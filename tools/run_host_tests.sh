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
gcc -std=c11 -Wall -Wextra -Werror -O1 -g 	-I source -I deps/blocksmith-server 	source/world/world.c 	source/world/chunk.c 	source/world/budget.c 	source/net/blockdiff.c 	source/net/networld.c 	source/net/networld_test.c 	-o "$BH/networld_test"

"./$BH/networld_test"

# interop_test.c, source/net's real client (networld.c) speaking to a REAL bsgame daemon over a
# real Unix socket — see that file's own header comment for why networld_test.c and bsgame_test.c
# (deps/blocksmith-server/game/) each passing on their own proves nothing about whether the two
# processes agree with each other, only with themselves. `make -C .../game bsgame` first, so the
# binary interop_test spawns is freshly built from what is actually checked out here rather than
# whatever a previous session last built by hand — a stale daemon would make this suite validate
# nothing. Eleventh binary, own main(), same reason as every one above it.
make -C deps/blocksmith-server/game bsgame

gcc -std=c11 -Wall -Wextra -Werror -O1 -g 	-I source -I deps/blocksmith-server 	source/world/world.c 	source/world/chunk.c 	source/world/budget.c 	source/net/blockdiff.c 	source/net/networld.c 	source/net/interop_test.c 	-o "$BH/interop_test"

"./$BH/interop_test"

# Only reached if every binary above exited 0 (set -e stops the script on the first
# non-zero exit), so a failed run's directory is left behind for inspection rather than
# silently deleted along with the evidence of what failed.
rm -rf "$BH"
