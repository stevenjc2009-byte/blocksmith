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
#
# 2026-09-02: it had drifted AGAIN, in exactly the same way and for exactly the same reason
# — nothing runs this by default, so nothing notices. Measured failure, verbatim:
#
#     source/world/worldgen.c:...: undefined reference to `wgdColumnTops'
#     source/world/worldgen.c:...: undefined reference to `wgdColumn'
#     source/world/worldgen.c:...: undefined reference to `wgdHeight'
#
# Same shape of drift once more: v1.8.7 split the density field out of worldgen.c into
# source/world/worldgen_density.c; v1.8.11 added the worm carver in source/world/cave_carve.c,
# which worldgen_density.c now calls; and source/world/genversion.c carries the save-version
# resolver they consult. Three more files on the link line, and again not one line of the
# probe's own code changes.
#
# The standing lesson, which this file has now demonstrated twice: this script lists its
# objects EXPLICITLY while the Makefile GLOBS source/world/*.c. Every module split silently
# breaks the explicit list and silently does NOT break the console build. If you split a
# module, grep tools/ for the old filename before you call it done.
#
# 2026-09-03: a third time, same shape, mid-session — worldgen_density.c grew a call into a
# new source/world/ore_gen.c (an ore-placement mask, landed by a different lane while this one
# was running). Measured failure, verbatim:
#
#     source/world/worldgen_density.c:844: undefined reference to `oreGenMaskGet'
#     source/world/worldgen_density.c:653: undefined reference to `oreGenBuildMask'
#
# One file added to the link line, again not one line of the probe's own code.
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
	source/world/worldgen_density.c \
	source/world/cave_carve.c \
	source/world/genversion.c \
	source/world/crc32.c \
	source/world/ore_gen.c \
	tests/net_stub.c \
	-lm \
	-o build-host/greedy_probe

./build-host/greedy_probe
