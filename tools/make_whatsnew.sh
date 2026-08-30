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
# The check REFUSES a file that still carries the template's placeholder items, so a freshly
# created file fails until someone actually writes it. Before that refusal existed this
# script signed off a wholly unedited template with "OK — attach it to the release", and it
# is release gate 3 of 8: nothing downstream looks at the contents, so a placeholder that
# got past here would have reached players on the top screen.
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

# The template's two placeholder items, defined ONCE and then both written into the template
# and handed to the checker below. They are the whole basis of the "you never edited this"
# refusal, so they must not be able to drift apart: a checker looking for a sentence the
# template no longer writes would wave the template straight through, which is the exact bug
# this refusal exists to stop.
#
# Matched as whole lines rather than by a keyword. Rejecting anything containing "Something"
# would refuse a perfectly good note that happens to open with the word; a whole-line match
# can only fire on text nobody wrote. It is also per-line, which is the case that actually
# happens: someone writes the feature and ships the fix placeholder underneath it.
#
# A marker comment for the operator to delete was the alternative and was rejected. The
# console reader does ignore comments (app/whatsnew.c's handleLine returns on '#'), so a
# marker would have been safe to put in the file — but it fails open. Deleting the marker
# without touching the text passes, and editing the text without deleting the marker fails,
# so it tests the operator's memory instead of the file's contents.
TEMPLATE_FEATURE='Something new the player can now do.'
TEMPLATE_FIX='Something that was broken and now is not.'

if [ ! -f "$FILE" ]; then
	# Unquoted heredoc, so the two placeholders above land in the file. Nothing else in it may
	# contain a '$', a backtick or a backslash.
	cat > "$FILE" <<TEMPLATE
# Release notes shown on the 3DS top screen before the update downloads.
#
# Two sections, both optional, one item per line. Plain English, no markdown: this is read
# by a player on a 400 px screen, not by a developer. A leading "- " is optional.
#
# Long lines wrap on their own, so write a sentence rather than fighting the width.
#
# The two lines below are placeholders. This script REFUSES the file while either of them is
# still here, so replace each with a real sentence about this release, or delete it if that
# section has nothing in it.

[features]
$TEMPLATE_FEATURE

[fixes]
$TEMPLATE_FIX
TEMPLATE
	echo "created $FILE from the template — edit it, then run this again to check it"
	echo
fi

echo "checking $FILE against source/app/whatsnew.h"
echo "  byte cap $BYTES_MAX, item cap $ITEMS_MAX, $ITEM_MAX characters an item"

awk -v file="$FILE" -v bytes_max="$BYTES_MAX" -v items_max="$ITEMS_MAX" \
    -v item_max="$ITEM_MAX" -v cols="$COLS" -v visible="$VISIBLE" \
    -v tpl_feature="$TEMPLATE_FEATURE" -v tpl_fix="$TEMPLATE_FIX" '
	function trim(s) { sub(/^[ \t]+/, "", s); sub(/[ \t\r]+$/, "", s); return s }

	BEGIN {
		# Folded once here rather than per line. Case-insensitive because re-capitalising a
		# placeholder is not writing one, and an exact-sentence match cannot false-positive
		# on either casing anyway.
		tpl[tolower(tpl_feature)] = 1
		tpl[tolower(tpl_fix)]     = 1
	}

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

		# line has been trimmed and had any bullet stripped, so this fires whether or not the
		# placeholder was left with a "- " in front of it, and on a CRLF file too: trim() eats
		# the CR, which is what a file edited in Notepad on this machine always carries.
		# (No apostrophes anywhere in this awk program — it is inside a single-quoted string.)
		if (tolower(line) in tpl) {
			printf "  ERROR  line %d: still the template placeholder — \"%s\"\n", NR, line
			placeholders++
			bad++
		}

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
		if (placeholders > 0) {
			printf "  %d of the %d item(s) above %s text this script wrote, not text anyone wrote.\n", \
			       placeholders, items, (placeholders == 1 ? "is" : "are")
			printf "         Replace each flagged line in %s with a real player-facing sentence\n", file
			printf "         about this release, or delete it if that section has nothing in it. Players\n"
			printf "         read this file on the top screen before they download, so a placeholder\n"
			printf "         goes straight to them.\n"
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
