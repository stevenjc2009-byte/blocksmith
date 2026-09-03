#!/bin/sh
# Builds and runs tools/bench_worldgen.c — lane OPT-WORLDGEN's host timing harness for world
# generation. Same host compiler and same kind of file list as tools/run_greedy_probe.sh and
# tools/run_host_tests.sh, so the numbers describe the real generator, not a re-implementation.
set -e

cd "$(dirname "$0")/.."
mkdir -p build-host

gcc -std=c11 -Wall -Wextra -Werror -O2 -g \
	-I source \
	tools/bench_worldgen.c \
	source/world/block.c \
	source/world/registry.c \
	source/world/crc32.c \
	source/world/chunk.c \
	source/world/world.c \
	source/world/scratch.c \
	source/world/noise.c \
	source/world/budget.c \
	source/world/worldgen.c \
	source/world/worldgen_density.c \
	source/world/cave_carve.c \
	source/world/genversion.c \
	tests/net_stub.c \
	-lm \
	-o build-host/bench_worldgen

./build-host/bench_worldgen
