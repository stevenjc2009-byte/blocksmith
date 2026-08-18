#!/bin/sh
# Compiles the world data code with the host compiler and runs its self-test.
#
# Everything under source/world is plain C with no 3DS dependency, so it can be
# tested here in a second instead of through a devkitARM build and an emulator.
# The file list is explicit rather than a glob because block_tiles_check.c includes
# <3ds.h> — it is the console-only bridge that checks the two duplicated enums.
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
	source/world/worldgen.c \
	source/world/world_test.c \
	-o build-host/world_test

./build-host/world_test
