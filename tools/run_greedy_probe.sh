#!/bin/sh
# Builds and runs step 7.4's greedy-meshing measurement. See tools/greedy_probe.c.
#
# Same host compiler and same file list as run_host_tests.sh, because the point is to
# measure the mesher the game actually ships, not a re-implementation of it.
set -e

cd "$(dirname "$0")/.."
mkdir -p build-host

gcc -std=c11 -Wall -Wextra -Werror -O2 -g \
	-I source \
	tools/greedy_probe.c \
	source/world/block.c \
	source/world/chunk.c \
	source/world/world.c \
	source/world/scratch.c \
	source/world/mesher.c \
	source/world/noise.c \
	source/world/budget.c \
	source/world/worldgen.c \
	-lm \
	-o build-host/greedy_probe

./build-host/greedy_probe
