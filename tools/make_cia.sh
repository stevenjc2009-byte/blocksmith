#!/usr/bin/env bash
# Packages the already-built Blocksmith homebrew into blocksmith.cia.
#
# Step 10.1: .3dsx -> makerom + bannertool + SMDH.
#
# This script does NOT run `make`. It only consumes what `make` already
# produced (blocksmith.elf / blocksmith.3dsx at the project root) plus the
# CIA-specific inputs this project owns (cia/blocksmith.rsf, gfx/icon.png,
# gfx/banner.png). Run `make` yourself first if those build outputs are
# missing - see the error messages below for the exact command.
#
# Approach copied from the working sibling project,
# 3ds-project-folder/model-making, whose Makefile `cia` target drives
# makerom + bannertool the same way: bannertool builds a banner from a PNG +
# an audio track, makerom takes -elf + -rsf + -icon + -banner and produces
# the .cia. The one deliberate difference: the sibling reuses the SMDH that
# 3dsxtool already baked into the .3dsx build (it has a root-level icon.png
# picked up automatically by the shared devkitARM Makefile). Editing
# Blocksmith's Makefile is out of scope for this step, so this script
# instead builds a small CIA-specific SMDH itself, with `bannertool
# makesmdh`, from gfx/icon.png - functionally the same end result (a
# Blocksmith-branded icon in the installed title), reached without touching
# any file this script doesn't own.
#
# Usage (from a devkitPro MSYS2 bash prompt):
#   tools/make_cia.sh
#
# Requires: bannertool and makerom from the devkitPro toolchain (installed
# already - see cia/README.md), plus a Python interpreter for the generated
# banner assets. devkitPro's MSYS2 ships no python at all, so point $BS_PYTHON
# at one rather than editing PATH:
#
#   BS_PYTHON=/c/Path/To/python.exe tools/make_cia.sh
#
# With $BS_PYTHON unset the script searches $PATH for python3 then python, as it
# always has.

set -u
set -o pipefail

die() {
    echo "make_cia.sh: FAILED - $1" >&2
    exit 1
}

# --- Locate the project root from this script's own path, not the caller's
# cwd, so this works no matter where it's invoked from. ---
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT" || die "could not cd to project root ($ROOT)"

TARGET=blocksmith
ELF="$ROOT/$TARGET.elf"
THREEDSX="$ROOT/$TARGET.3dsx"
RSF="$ROOT/cia/$TARGET.rsf"
ICON_PNG="$ROOT/gfx/icon.png"
BANNER_PNG="$ROOT/gfx/banner.png"
BUILD="$ROOT/cia/build"
CIA_SMDH="$BUILD/$TARGET-cia.smdh"
BANNER_BNR="$BUILD/$TARGET.bnr"
CHIME_WAV="$BUILD/chime.wav"
OUT_CIA="$ROOT/$TARGET.cia"

# --- 1. Find the tools. Full paths, same reasoning as the sibling Makefile:
# these ship inside $DEVKITPRO/tools/bin and are not on PATH in a plain
# msys2 login shell. ---
find_tool() {
    local name="$1"
    local candidates=()
    if [ -n "${DEVKITPRO:-}" ]; then
        candidates+=("$DEVKITPRO/tools/bin/$name" "$DEVKITPRO/tools/bin/$name.exe")
    fi
    candidates+=("/opt/devkitpro/tools/bin/$name" "/opt/devkitpro/tools/bin/$name.exe")
    candidates+=("/c/devkitPro/tools/bin/$name" "/c/devkitPro/tools/bin/$name.exe")
    for c in "${candidates[@]}"; do
        if [ -x "$c" ]; then
            echo "$c"
            return 0
        fi
    done
    # Last resort: PATH.
    if command -v "$name" >/dev/null 2>&1; then
        command -v "$name"
        return 0
    fi
    return 1
}

# --- Resolve the Python interpreter the asset generators below are run with.
# Sets PY_RESOLVED; returns non-zero if nothing could be found, so each caller
# keeps its own message about what it needed Python for.
#
# $BS_PYTHON exists because a PATH-only lookup made a clean clone unbuildable on
# a Windows machine: there is no `python` and no `python3` inside devkitPro's
# MSYS2, which is the only shell console builds may run from, so this script
# exited 1 at the banner-chime step until the operator happened to know to
# prepend a Windows interpreter to PATH by hand. With $BS_PYTHON unset the old
# PATH search is exactly what still runs.
#
# A set-but-broken $BS_PYTHON is fatal rather than a reason to fall back: an
# interpreter that was named explicitly must never be silently swapped for a
# different one, because the swap would only show up as a mystery failure inside
# make_banner.py or make_banner_audio.py much later. ---
resolve_python() {
    if [ -n "${BS_PYTHON:-}" ]; then
        [ -x "$BS_PYTHON" ] || die "\$BS_PYTHON is set to '$BS_PYTHON', which is not an executable file. Point it at a real Python interpreter, or unset it to search \$PATH instead."
        PY_RESOLVED="$BS_PYTHON"
        return 0
    fi
    PY_RESOLVED="$(command -v python3 || command -v python)" || return 1
    return 0
}

MAKEROM="$(find_tool makerom)" || die "makerom not found. Checked \$DEVKITPRO/tools/bin, /opt/devkitpro/tools/bin, /c/devkitPro/tools/bin and \$PATH. Install devkitPro's 3ds-tools (makerom ships with it) and re-run."
BANNERTOOL="$(find_tool bannertool)" || die "bannertool not found. Checked \$DEVKITPRO/tools/bin, /opt/devkitpro/tools/bin, /c/devkitPro/tools/bin and \$PATH. Install devkitPro's bannertool and re-run."

# --- 2. Verify the game build already ran. ---
[ -f "$ELF" ]      || die "$ELF is missing. Run the game build first: (from $ROOT) make"
[ -f "$THREEDSX" ] || die "$THREEDSX is missing. Run the game build first: (from $ROOT) make"

# --- 3. Verify the CIA's own inputs are present. ---
[ -f "$RSF" ] || die "$RSF is missing. This is a checked-in file, not a build output - it should not be missing."

if [ ! -f "$ICON_PNG" ] || [ ! -f "$BANNER_PNG" ]; then
    echo "make_cia.sh: gfx/icon.png and/or gfx/banner.png missing - generating them with tools/make_banner.py ..."
    resolve_python || die "gfx/icon.png / gfx/banner.png are missing and no python3/python is on PATH to generate them, and \$BS_PYTHON is not set. Set \$BS_PYTHON to a Python interpreter, or run 'python tools/make_banner.py' from a shell that has Pillow installed, then re-run this script."
    PY="$PY_RESOLVED"
    "$PY" "$ROOT/tools/make_banner.py" || die "tools/make_banner.py failed - see its output above."
    [ -f "$ICON_PNG" ]   || die "tools/make_banner.py ran but $ICON_PNG still doesn't exist."
    [ -f "$BANNER_PNG" ] || die "tools/make_banner.py ran but $BANNER_PNG still doesn't exist."
fi

mkdir -p "$BUILD" || die "could not create $BUILD"

# --- 4. Build the CIA-specific SMDH (icon + title strings) from gfx/icon.png.
# Title/description/author match the ones the Makefile passes to 3dsxtool
# for the .3dsx build (APP_TITLE / APP_DESCRIPTION / APP_AUTHOR), so the
# .3dsx and the .cia describe the game identically. bannertool requires the
# icon to be exactly 48x48 - verified directly against this bannertool
# build: a 32x32 test image was rejected with
# "[ERROR] Image must be exactly 48 x 48 in size."; a 48x48 image (what
# make_banner.py produces) is accepted. ---
echo "make_cia.sh: building SMDH from $ICON_PNG ..."
"$BANNERTOOL" makesmdh \
    -s "Blocksmith" \
    -l "Blocksmith - Block survival, built for the 3DS" \
    -p "steve" \
    -i "$ICON_PNG" \
    -o "$CIA_SMDH" \
    || die "bannertool makesmdh failed - see its output above."
[ -f "$CIA_SMDH" ] || die "bannertool makesmdh reported success but $CIA_SMDH does not exist."

# --- 5. Build the banner. bannertool's makebanner refuses to run without an
# audio track (-a/-ca), and up to task 44 this script satisfied that with 0.1s
# of printf'd silence. It now generates the real Home Menu chime instead, with
# tools/make_banner_audio.py - synthesised from scratch with the Python
# standard library, deterministic, nothing sampled. The WAV is still build
# output rather than a checked-in asset: the script is the source, the .wav is
# what it prints, exactly like gfx/*.png and their generators.
#
# Regenerated on every run rather than only-if-missing (the pattern used for
# the PNGs at step 3), because the generator is cheap and a stale chime left
# over from an edited script would ship silently.
#
# The track is played by the 3DS Home Menu out of the banner, not by the game:
# it needs no audio code in Blocksmith and does not depend on roadmap task 43.
#
# Verified bannertool's banner-image size requirement directly: a 100x100 test
# image was rejected with "[ERROR] Image must be exactly 256 x 128 in size.";
# a 256x128 image (what make_banner.py produces) is accepted. ---
echo "make_cia.sh: generating the banner chime with tools/make_banner_audio.py ..."
resolve_python \
    || die "no python3/python on PATH to generate the banner chime, and \$BS_PYTHON is not set. Set \$BS_PYTHON to a Python interpreter, or install Python on PATH (the generator needs only the standard library - no Pillow, no numpy), then re-run this script."
PY_AUDIO="$PY_RESOLVED"
"$PY_AUDIO" "$ROOT/tools/make_banner_audio.py" "$CHIME_WAV" \
    || die "tools/make_banner_audio.py failed - see its output above."
[ -f "$CHIME_WAV" ] || die "tools/make_banner_audio.py ran but $CHIME_WAV still doesn't exist."

echo "make_cia.sh: building banner from $BANNER_PNG ..."
"$BANNERTOOL" makebanner \
    -i "$BANNER_PNG" \
    -a "$CHIME_WAV" \
    -o "$BANNER_BNR" \
    || die "bannertool makebanner failed - see its output above."
[ -f "$BANNER_BNR" ] || die "bannertool makebanner reported success but $BANNER_BNR does not exist."

# --- 6. makerom: elf + rsf + icon + banner -> .cia. Same flag set as the
# sibling Makefile's cia target (-exefslogo -target t), because that is the
# proven-working combination, not a new invention. ---
echo "make_cia.sh: running makerom ..."
"$MAKEROM" -f cia -o "$OUT_CIA" \
    -elf "$ELF" \
    -rsf "$RSF" \
    -icon "$CIA_SMDH" \
    -banner "$BANNER_BNR" \
    -exefslogo -target t \
    || die "makerom failed - see its output above. The half-built file, if any, was NOT treated as a valid .cia; check $OUT_CIA by hand before trusting it."

[ -f "$OUT_CIA" ] || die "makerom reported success but $OUT_CIA does not exist."

SIZE="$(wc -c < "$OUT_CIA" | tr -d ' ')"
echo "make_cia.sh: built $OUT_CIA ($SIZE bytes)"
