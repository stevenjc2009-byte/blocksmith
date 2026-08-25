#!/bin/sh
# Fails if README.md has fallen behind the release it is supposed to describe.
#
# Why this exists. README.md went stale across two consecutive releases (it still
# advertised v1.7.0 while v1.7.1, v1.8.0 and v1.8.1 shipped), because nothing in the
# release convention ever read it. Every other release asset has a gate --
# tools/make_whatsnew.sh checks the change notes against source/app/whatsnew.h, the
# Makefile's cia target refuses to package without BS_PSK, tools/make_qr.py decodes its
# own QR back before writing it -- and the README alone had none. This is that gate.
#
# Pure POSIX sh plus coreutils on purpose. The release is cut from a devkitPro MSYS2
# shell, where there is no `python` and no `strings`, so a gate written in either would
# be skipped exactly when it matters.
#
# What it does NOT do: it cannot tell whether the prose is true. It checks the things a
# machine can check -- that the version README names is the version the tree declares,
# that every release URL and asset filename in it points at that same version, and that
# every file path it mentions still exists. Prose accuracy is still a human read.
#
# Usage:  sh tools/check_readme_current.sh
# Exit:   0 all checks pass, 1 at least one check failed, 2 could not run.

set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd) || exit 2
cd "$ROOT" || exit 2

README=README.md
CHANGELOG=CHANGELOG.md
VERSION_H=source/version.h

fails=0

fail() {
	printf 'FAIL: %s\n' "$1" >&2
	fails=$((fails + 1))
}

for f in "$README" "$CHANGELOG" "$VERSION_H"; do
	[ -f "$f" ] || { printf 'ERROR: %s does not exist\n' "$f" >&2; exit 2; }
done

# README.md is CRLF (.gitattributes pins `* -text`, so the bytes are whatever is
# committed). Strip CR once, up front, or every anchored pattern below silently
# misses and the gate passes by accident.
readme_text=$(tr -d '\r' < "$README")

# --- 1. The three places a version is written down ------------------------------

ver_h=$(tr -d '\r' < "$VERSION_H" \
	| sed -n 's/^#define[[:space:]][[:space:]]*BLOCKSMITH_VERSION[[:space:]][[:space:]]*"\([0-9][0-9.]*\)".*/\1/p' \
	| head -n 1)

ver_cl=$(tr -d '\r' < "$CHANGELOG" \
	| sed -n 's/^##[[:space:]]*\[\([0-9][0-9.]*\)\].*/\1/p' \
	| head -n 1)

ver_rm=$(printf '%s\n' "$readme_text" \
	| sed -n 's/^<!--[[:space:]]*readme-version:[[:space:]]*\([0-9][0-9.]*\)[[:space:]]*-->[[:space:]]*$/\1/p' \
	| head -n 1)

if [ -z "$ver_h" ]; then
	fail "could not read BLOCKSMITH_VERSION out of $VERSION_H"
fi
if [ -z "$ver_cl" ]; then
	fail "could not read the newest version heading out of $CHANGELOG (expected a line like '## [1.2.3] - date')"
fi
if [ -z "$ver_rm" ]; then
	fail "$README has no version marker. Add a line reading exactly: <!-- readme-version: $ver_h -->"
fi

# BLOCKSMITH_VERSION is deliberately blank between releases (see source/version.h), so
# an empty value there is not a failure of this gate -- but then there is no version to
# check the README against and the rest of the comparisons cannot mean anything.
if [ -n "$ver_h" ] && [ -n "$ver_rm" ] && [ "$ver_h" != "$ver_rm" ]; then
	fail "$README says version $ver_rm but $VERSION_H says $ver_h -- the README is stale (or version.h was bumped without it)"
fi
if [ -n "$ver_h" ] && [ -n "$ver_cl" ] && [ "$ver_h" != "$ver_cl" ]; then
	fail "$VERSION_H says $ver_h but the newest $CHANGELOG entry is $ver_cl -- one of them was not bumped"
fi

ver="$ver_rm"

# --- 2. Every version-stamped release reference in the README -------------------

if [ -n "$ver" ]; then
	# Asset filenames: blocksmith1.8.1.cia
	for got in $(printf '%s\n' "$readme_text" \
		| grep -oE 'blocksmith[0-9]+\.[0-9]+\.[0-9]+\.cia' \
		| sed -e 's/^blocksmith//' -e 's/\.cia$//' | sort -u); do
		[ "$got" = "$ver" ] || fail "$README names asset blocksmith$got.cia but this release is $ver"
	done

	# Release URLs: releases/tag/v1.8.1 and releases/download/v1.8.1
	for got in $(printf '%s\n' "$readme_text" \
		| grep -oE 'releases/(tag|download)/v[0-9]+\.[0-9]+\.[0-9]+' \
		| sed -e 's#.*/v##' | sort -u); do
		[ "$got" = "$ver" ] || fail "$README links a release URL for v$got but this release is $ver"
	done

	# The QR image's alt text, which is prose a reader trusts and nothing else checks.
	for got in $(printf '%s\n' "$readme_text" \
		| grep -oE 'alt="[^"]*v[0-9]+\.[0-9]+\.[0-9]+[^"]*"' \
		| grep -oE 'v[0-9]+\.[0-9]+\.[0-9]+' | sed 's/^v//' | sort -u); do
		[ "$got" = "$ver" ] || fail "$README's QR alt text says v$got but this release is $ver"
	done

	# The change-notes asset the in-app updater fetches for this release.
	[ -f "whatsnew$ver.txt" ] \
		|| fail "whatsnew$ver.txt does not exist -- run: sh tools/make_whatsnew.sh $ver"
fi

# --- 3. Every path the README points at ------------------------------------------

# Backticked spans, with fenced code blocks removed first so a shell command inside a
# fence is not mistaken for a path.
#
# Each span is then split on whitespace and every WORD is considered separately. Splitting
# matters: a span like `python tools/make_qr.py <version>` is a command, not a path, but
# the path is in there and an early version of this gate dropped the whole span for having
# spaces in it -- which silently disarmed this entire check. Deleting tools/make_qr.py left
# it green. Do not put the space filter back.
#
# A word counts as a repo-relative path only if it is made of path characters and contains
# a slash. That drops URLs and sdmc:/... (colon), /releases/latest (leading slash, a
# github.com endpoint and never a file here), and <owner>/<repo> (angle brackets).
paths=$(printf '%s\n' "$readme_text" \
	| awk '/^```/ { fence = !fence; next } !fence' \
	| grep -oE '`[^`]+`' \
	| sed -e 's/^`//' -e 's/`$//' \
	| tr -s '[:space:]' '\n' \
	| grep -E '^[A-Za-z0-9_.-]+(/[A-Za-z0-9_.-]+)+/?$' \
	| sort -u)

for p in $paths; do
	[ -e "$p" ] || fail "$README mentions \`$p\` but no such file or directory exists"
done

# The QR image is referenced from an <img src=...>, not a backticked span.
for p in $(printf '%s\n' "$readme_text" \
	| grep -oE 'src="[^"]+"' | sed -e 's/^src="//' -e 's/"$//' \
	| grep -vE '^https?:' | sort -u); do
	[ -e "$p" ] || fail "$README's <img src=\"$p\"> points at a file that does not exist"
done

# --- 4. Verdict -------------------------------------------------------------------

if [ "$fails" -ne 0 ]; then
	printf '\ncheck_readme_current.sh: %d check(s) failed. README.md is out of date.\n' "$fails" >&2
	exit 1
fi

printf 'check_readme_current.sh: OK -- README.md, %s and %s all agree on %s.\n' \
	"$VERSION_H" "$CHANGELOG" "$ver"
exit 0
