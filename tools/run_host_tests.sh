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
mkdir -p build-host

gcc -std=c11 -Wall -Wextra -Werror -O1 -g \
	-I source \
	tests/host_test.c \
	source/world/block.c \
	source/world/chunk.c \
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
	-o build-host/world_test

./build-host/world_test
