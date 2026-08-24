#!/bin/sh
# Builds and runs step 7.4's greedy-meshing measurement. See tools/greedy_probe.c.
#
# Same host compiler and same file list as run_host_tests.sh, because the point is to
# measure the mesher the game actually ships, not a re-implementation of it.
#
# 2026-08-23: this script had stopped building at all and nobody noticed, because nothing
# runs it by default. Measured failure, verbatim:
#
#     source/world/block.c:11: undefined reference to `registryView'
#     source/world/world.c:219: undefined reference to `networldOnColumnLoad'
#
# Both are drift, not probe bugs: v1.6.0 Phase A moved blockInfo() onto the block registry
# (source/world/registry.c) and worldSet() gained the networld hook that run_host_tests.sh
# already satisfies with tests/net_stub.c. Adding those two files to the link is the whole
# fix — the probe's own code is unchanged, and the numbers it prints are the real mesher's.
set -e

cd "$(dirname "$0")/.."
mkdir -p build-host

gcc -std=c11 -Wall -Wextra -Werror -O2 -g \
	-I source \
	tools/greedy_probe.c \
	source/world/block.c \
	source/world/registry.c \
	source/world/chunk.c \
	source/world/world.c \
	source/world/scratch.c \
	source/world/mesher.c \
	source/world/noise.c \
	source/world/budget.c \
	source/world/worldgen.c \
	tests/net_stub.c \
	-lm \
	-o build-host/greedy_probe

./build-host/greedy_probe
