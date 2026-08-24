#!/bin/sh
# Creates and checks the release-notes file every release has to carry (v1.6.0 task 14b).
#
# Each GitHub release publishes two assets: blocksmith<version>.cia, and whatsnew<version>.txt
# next to it. The in-app updater fetches the second one from
# https://github.com/<owner>/<repo>/releases/download/v<version>/whatsnew<version>.txt and
# shows it on the TOP screen before the player commits to the download, so they know what
# they are getting. A release without one still updates perfectly — the screen just says
# there are no notes — but every release from v1.6.0 on is meant to have one.
#
# This script exists so producing next release's file is mechanical rather than remembered.
#
# Usage, from the project root:
#
#   sh tools/make_whatsnew.sh 1.6.0     # writes whatsnew1.6.0.txt if it does not exist,
#                                        # then checks it and reports
#
# A leading "v" on the version is accepted and stripped, the same way the updater tolerates
# one on a tag (app/updater_version.c's tagNumber).
#
# The caps below are READ OUT OF source/app/whatsnew.h rather than repeated here, because a
# checker that disagrees with the parser is worse than no checker: it would sign off a file
# the console then truncates.
set -e

cd "$(dirname "$0")/.."

VERSION="$1"
if [ -z "$VERSION" ]; then
	echo "usage: sh tools/make_whatsnew.sh <version>    e.g. 1.6.0" >&2
	exit 2
fi

VERSION="${VERSION#v}"
VERSION="${VERSION#V}"
FILE="whatsnew${VERSION}.txt"

HEADER="source/app/whatsnew.h"
capOf() {
	value=$(sed -n "s/^#define $1[[:space:]]\{1,\}\([0-9]\{1,\}\).*/\1/p" "$HEADER")
	if [ -z "$value" ]; then
		echo "could not read $1 out of $HEADER" >&2
		exit 3
	fi
	echo "$value"
}

BYTES_MAX=$(capOf WHATSNEW_BYTES_MAX)
ITEMS_MAX=$(capOf WHATSNEW_ITEMS_MAX)
ITEM_CHARS=$(capOf WHATSNEW_ITEM_CHARS)
ITEM_MAX=$((ITEM_CHARS - 1))

# The top screen's text column is 366 px at gfx/font.h's FONT_ADVANCE of 6, and an item's
# first line loses two of those to the bullet. 21 lines are on screen at once; past that the
# player scrolls, which is fine and is what the scrollbar is for — it is reported, not
# refused. Both numbers mirror scene/title.c's NOTES_TEXT_W / NOTES_VISIBLE.
COLS=61
VISIBLE=21

if [ ! -f "$FILE" ]; then
	cat > "$FILE" <<'TEMPLATE'
# Release notes shown on the 3DS top screen before the update downloads.
#
# Two sections, both optional, one item per line. Plain English, no markdown: this is read
# by a player on a 400 px screen, not by a developer. A leading "- " is optional.
#
# Long lines wrap on their own, so write a sentence rather than fighting the width.

[features]
Something new the player can now do.

[fixes]
Something that was broken and now is not.
TEMPLATE
	echo "created $FILE from the template — edit it, then run this again to check it"
	echo
fi

echo "checking $FILE against source/app/whatsnew.h"
echo "  byte cap $BYTES_MAX, item cap $ITEMS_MAX, $ITEM_MAX characters an item"

awk -v file="$FILE" -v bytes_max="$BYTES_MAX" -v items_max="$ITEMS_MAX" \
    -v item_max="$ITEM_MAX" -v cols="$COLS" -v visible="$VISIBLE" '
	function trim(s) { sub(/^[ \t]+/, "", s); sub(/[ \t\r]+$/, "", s); return s }

	{
		bytes += length($0) + 1
		line = trim($0)

		if (line == "" || substr(line, 1, 1) == "#") next

		if (substr(line, 1, 1) == "[" && substr(line, length(line), 1) == "]") {
			name = tolower(substr(line, 2, length(line) - 2))
			if (name == "features" || name == "fixes") { section = name; next }
			printf "  ERROR  line %d: unknown section [%s] — only [features] and [fixes] are read\n", NR, name
			bad++
			section = ""
			next
		}

		if (section == "") {
			printf "  ERROR  line %d: item before any [features] or [fixes] header — the console drops it\n", NR
			bad++
			next
		}

		sub(/^[-*][ \t]+/, "", line)
		if (line == "") next

		items++
		if (length(line) > item_max) {
			printf "  ERROR  line %d: %d characters, cap is %d — the console cuts it and shows \"...\"\n", \
			       NR, length(line), item_max
			bad++
		}

		# Rough wrap count, the same greedy rule app/whatsnew.c uses, for the report only.
		rest = line
		width = cols - 2
		while (length(rest) > width) {
			cut = width
			while (cut > 0 && substr(rest, cut + 1, 1) != " ") cut--
			if (cut == 0) cut = width
			rest = substr(rest, cut + 1)
			sub(/^ +/, "", rest)
			drawn++
		}
		drawn++
	}

	END {
		if (items > 0) drawn += 2   # the section headings, roughly

		printf "  %d bytes, %d items, about %d lines on screen\n", bytes, items, drawn

		if (bytes > bytes_max) {
			printf "  ERROR  %d bytes is over the %d byte cap — the console reads only the front of it\n", \
			       bytes, bytes_max
			bad++
		}
		if (items > items_max) {
			printf "  ERROR  %d items is over the %d item cap — the console drops the rest\n", \
			       items, items_max
			bad++
		}
		if (items == 0) {
			printf "  ERROR  no items at all — the console would show \"No change notes for this version.\"\n"
			bad++
		}
		if (drawn > visible)
			printf "  note   more than the %d lines that fit at once, so the player scrolls (that is fine)\n", visible

		if (bad > 0) {
			printf "  %d problem(s) — fix them before attaching this to the release\n", bad
			exit 1
		}
		printf "  OK — attach %s to the release alongside the .cia\n", file
	}
' "$FILE"
