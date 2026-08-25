#---------------------------------------------------------------------------------
# Blocksmith — Nintendo 3DS block-survival game
#
# Based on the devkitPro 3ds GPU example Makefile (the standard homebrew build),
# with SOURCES extended to cover the per-subsystem source folders.
#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

#---------------------------------------------------------------------------------
# TARGET       output name
# BUILD        intermediate object dir
# SOURCES      dirs scanned for .c / .cpp / .s / .v.pica
# INCLUDES     dirs scanned for headers
# GRAPHICS     dirs scanned for .t3s (texture atlases — used from Phase 1)
#---------------------------------------------------------------------------------
TARGET		:=	blocksmith
BUILD		:=	build
SOURCES		:=	source source/app source/gfx source/debug source/net source/scene source/shaders source/world deps/libhydrogen
DATA		:=	data
INCLUDES	:=	source deps/libhydrogen deps/blocksmith-server
GRAPHICS	:=	gfx
GFXBUILD	:=	$(BUILD)
# RomFs exists for exactly one file: romfs/cacert.pem, the CA bundle the in-app updater
# (source/app/updater.c) needs to trust github.com. The console's own root store predates
# every CA in use today, so without this the update check fails with a TLS error. Same
# reasoning and the same single file as the sibling project's Makefile
# (3ds-project-folder/model-making) — see that file's own comment on its ROMFS line.
ROMFS		:=	romfs

APP_TITLE	:=	Blocksmith
APP_DESCRIPTION	:=	Block survival, built for the 3DS
APP_AUTHOR	:=	steve

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH	:=	-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

# -O3 rather than devkitPro's default -O2, because it was measured rather than assumed:
# on the Phase 3 remesh stress (1000 edits, 3520 chunk rebuilds) -O2 gave 5602 ms and
# -O3 gave 5175 ms, an 8% win, for 4,576 more bytes of .3dsx (210,852 -> 215,428). Code
# size is a real constraint on an Old 3DS, so revisit this if the executable ever gets
# close to the budget — but 2% of 210 KB is not that day.
CFLAGS	:=	-g -Wall -Wextra -O3 -mword-relocations \
			-ffunction-sections \
			$(ARCH)

CFLAGS	+=	$(INCLUDE) -D__3DS__

# For one-off instrumented builds, e.g.
#   make EXTRA_CFLAGS=-DBS_GEO_START=3
CFLAGS	+=	$(EXTRA_CFLAGS)

# The network PSK is a shared secret and is NOT committed — this repository is
# public. Supply it on the command line for a release build:
#
#   make cia BS_PSK=<64 hex characters>
#
# Without it the build still compiles and runs; Connect just reports that no PSK
# is configured, which is the honest state for a clone nobody has pointed at a
# server. The `cia` target refuses to package without it, because a release CIA
# that cannot reach any server is a much worse failure than a loud one here.
ifneq ($(BS_PSK),)
CFLAGS	+=	-DBS_NETWORK_PSK_HEX=\"$(BS_PSK)\"
endif

CXXFLAGS	:= $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

# curl and its mbedtls backend come first and in this order, same reasoning as the sibling
# project's Makefile (3ds-project-folder/model-making): the linker resolves left to right,
# so putting them after -lctru leaves the TLS symbols undefined; -lz last of the four
# because curl is built with zlib support and pulls inflate out of it. These four are the
# only reason $(PORTLIBS) is on LIBDIRS below, and they exist only for
# source/app/updater.c.
LIBS	:= -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz -lcitro3d -lctru -lm

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
LIBDIRS	:= $(PORTLIBS) $(CTRULIB)

#---------------------------------------------------------------------------------
# libhydrogen (Noise XX handshake, shared with server/gateway) — mirrors
# server/gateway/Makefile: pinned to an exact commit (a mismatch with the server
# is an unexplained handshake failure, not a compile error), safe.directory set
# per-command rather than globally for the same mounted-filesystem reason.
#
# Run `make deps` once before the first build. This is a plain file-existence
# rule, so on a fresh checkout it must run (and finish) before `make` reads
# this Makefile for the SOURCES wildcard scan below to pick up hydrogen.c —
# GNU Make expands that scan at parse time, before any recipe (including this
# one) has run, so `make deps && make` (or two plain `make` invocations) is
# required the first time, same as the gateway's documented workflow.
#---------------------------------------------------------------------------------
# The `deps` target below is the first target in this file, and GNU Make makes the first
# target the default goal. That silently turned a bare `make` — and, worse, the goal-less
# sub-make inside the `all` recipe further down — into "check that libhydrogen is cloned,
# then stop", so a full build produced nothing and said only "Nothing to be done for
# 'deps'". Pinning the default goal back is the fix; the two branches need different ones
# because this Makefile re-invokes itself inside $(BUILD), where `all` does not exist.
ifneq ($(BUILD),$(notdir $(CURDIR)))
.DEFAULT_GOAL	:=	all
else
.DEFAULT_GOAL	:=	$(OUTPUT).3dsx
endif

HYDRO_REPO	:=	https://github.com/jedisct1/libhydrogen.git
HYDRO_COMMIT	:=	617036a353cd4f6478ab6c3f98c36dd31e23ce8e
HYDRO		:=	deps/libhydrogen

#---------------------------------------------------------------------------------
# bs_proto.h — the wire format, shared verbatim with the gateway. It lives in the
# server repo because the server is where the protocol is defined; the client only
# has to agree with it. Fetching it pinned, rather than keeping a copy here, is the
# whole point: two editable copies of a wire format drift silently, and the symptom
# of drift is a handshake that fails on real hardware for no visible reason.
#
# Pinned to a commit for the same reason libhydrogen is — the checkout is verified
# against the pinned SHA, so a force-push or a redirected remote cannot quietly
# change the protocol this build was compiled against.
#
# It is a separate clone from any server checkout a developer may already have
# beside this one: deps/ is build-fetched and disposable, and nothing in this repo
# is allowed to reach into a sibling repository's working tree.
#---------------------------------------------------------------------------------
PROTO_REPO	:=	https://github.com/stevenjc2009-byte/blocksmith-server.git
# v1.5.0 of the server — the commit that added BS_APP_PLAYER_STATE / PLAYER_REPORT.
# Bumped from 10111dfb (v1.3.0, which added BS_APP_CHUNK_SUB / CHUNK_DIFFS /
# CHUNK_UNSUB) because net/networld.c now decodes PLAYER_STATE: a clone still pinned
# to 10111dfb fetches a bs_proto.h with none of the PLAYER_* ids, flags or sizes in
# it and fails to compile.
#
# This pin is the PROTOCOL the client is built against, not the server it will meet.
# The running server must be updated to v1.5.0 as well and BEFORE this client ships:
# this client's PLAYER_REPORT is capability-gated on having heard PLAYER_STATE (see
# net/networld.h), so an old server simply never hears it — but the pose-restore
# feature needs a server that sends PLAYER_STATE at JOIN.
#
# Bumped again to 05c14fd6 (server v1.6.0). bs_proto.h itself is byte-identical to
# 25ae9674's — `git diff 25ae9674 05c14fd6 -- proto/` is empty — so this is not a wire
# change and a clone pinned at either commit compiles the same. It moves so the pin
# names a RELEASED server rather than an unreleased mid-branch commit. v1.6.0 is the
# first server that accepts the registry's dynamic block ids (0x80..0xFD) instead of
# silently dropping every edit placing one, so against an older server a dynamic block
# still cannot be placed — a server-side ceiling, not a protocol difference, which is
# why no capability gate can paper over it.
#
# Bumped again to 632d1998, head of the server branch docs/registry-crc-correction.
# NOT a wire change. bs_proto.h's only edit since 05c14fd6 is that one commit, and it
# renames literals without moving a byte: the same probe compiled against the old and
# the new header reports BS_APP_REGISTRY_DEFS_MAX_N 36, a full batch packing 1012
# bytes and declaring 1012, identically both times. A clone pinned at either commit
# emits the same packets, and an already-deployed v1.6.0+ server needs no update.
#
# It moves because bs_proto.h had to be edited at all, and this pin verifies the
# header by SHA -- leaving it at 05c14fd6 red-lines check-proto-drift on every build
# in this repo, which is why the bump is part of the same change and not a follow-up.
#
# What was edited: BS_APP_REGISTRY_DEFS_MAX_N and BS_APP_REGISTRY_DEFS_BYTES() each
# spelled the registry wire record size as a bare `28u`. That number belongs to
# world/registry.h (REGISTRY_WIRE_RECORD_BYTES = id byte + sizeof(BlockDef)), and the
# server's bsgame.c packs a REGISTRY_DEFS batch with the registry constant while
# declaring the packet's length with the literal. One extra BlockDef field takes the
# record 28 -> 29 and the literal does not follow: measured, 36 records then pack 1048
# bytes behind a declared length of 1012 -- 36 bytes short on every batch -- with
# MAX_N still 36 when 35 fit, 1048 overrunning BS_MAX_PAYLOAD (1024), and gcc exiting
# 0 with no warning. It is now BS_REGISTRY_WIRE_RECORD_BYTES, static-asserted against
# REGISTRY_WIRE_RECORD_BYTES in the server's game/validate.c, the one translation
# unit that sees both headers.
#
# Caveat worth stating: this pin now names a working BRANCH head rather than a
# released server, which is the opposite of what the 05c14fd6 bump above was for.
# The fix is server-side-only and no released server carries it yet. This should move
# again to whichever release absorbs docs/registry-crc-correction.
PROTO_COMMIT	:=	632d19989b49ac1d38f2a466a7a0c91551d0881c
PROTO		:=	deps/blocksmith-server

.PHONY: deps
deps: $(HYDRO)/hydrogen.h $(PROTO)/proto/bs_proto.h

$(PROTO)/proto/bs_proto.h:
	@mkdir -p deps
	git clone -q $(PROTO_REPO) $(PROTO)
	git -C $(PROTO) -c safe.directory='*' checkout -q $(PROTO_COMMIT)
	@test "$$(git -C $(PROTO) -c safe.directory='*' rev-parse HEAD)" = "$(PROTO_COMMIT)" \
	  || (echo "blocksmith-server checkout is not at the pinned commit"; exit 1)
	@echo "bs_proto.h pinned at $(PROTO_COMMIT)"

# ---------------------------------------------------------------- drift guard
#
# The rule above is a FILE target, so make runs it only when
# deps/blocksmith-server/proto/bs_proto.h does not exist. Once the clone is on
# disk it never runs again, and the "is the checkout at the pinned commit?"
# test inside it never runs again either. That made the pin a first-fetch
# formality rather than a guard: on any working dev tree — including this one,
# where deps/blocksmith-server sits on branch sync/client-v1.8.2 at 1767061,
# which is NOT PROTO_COMMIT — the build compiled against whatever bytes
# happened to be in the working tree and said nothing. It only stayed correct
# by luck: 1767061's proto/bs_proto.h happens to be byte-identical to
# 05c14fd6's, so nothing was actually broken, but nothing was checking either.
#
# bs_proto.h is the wire contract between this client and the Linux server.
# Drift here does not fail to compile; it produces a client and a server that
# disagree about ids, flags or struct sizes and desync at runtime, which is the
# hardest class of bug this project has to diagnose. So the check has to look
# at CONTENT, and it has to run on every build.
#
# Shape copied deliberately from deps/blocksmith-server/game/Makefile's
# check-world-drift (a phony `cmp`-on-bytes target wired into `all`) rather
# than inventing a second, different mechanism for the same job. That comment
# also argues, at length and from a real shipped bug, why a _Static_assert on a
# protocol constant is not a substitute: the number was never what drifted.
#
# Truth source is the pinned commit's own blob, streamed out of the clone's
# object store — NOT a sha256 recorded here beside PROTO_COMMIT. A recorded
# digest is a second statement of the same fact and can drift from the pin it
# describes: bump PROTO_COMMIT without bumping the digest and the guard goes
# red on a correct tree, whose obvious "fix" is to paste in whatever the disk
# says — training the reader to make the check green instead of to check.
# Deriving the expected bytes from PROTO_COMMIT keeps one source of truth, and
# because a commit SHA is content-addressed the bytes it names cannot change
# under it; a force-push that replaces the commit makes the SHA unresolvable,
# which this fires on, which is exactly the threat the pin comment claims.
#
# `cmp` on the streamed blob, not a `git hash-object` comparison, because the
# server repo's .gitattributes says `* text=auto eol=lf`: hash-object applies
# that clean filter, so a worktree file rewritten to CRLF would hash EQUAL to
# the pin while differing on disk. cat-file | cmp has no filter on either side.
# (The hash-object line in the failure message below is diagnostic only — if it
# prints equal while cmp failed, the drift is line endings and nothing else.)
#
# This guard reports; it never mutates. It does not re-run `git checkout
# $(PROTO_COMMIT)`, because deps/blocksmith-server is a separate repo a
# developer may have deliberately parked on a sync branch, and a build that
# silently moves your checkout is a worse bug than the one it is fixing.
.PHONY: check-proto-drift
check-proto-drift:
	@if [ ! -e $(PROTO)/proto/bs_proto.h ]; then \
		echo "Makefile: $(PROTO)/proto/bs_proto.h is not there."; \
		echo "  the vendored protocol header has not been fetched yet."; \
		echo "  fetch it with:  make deps"; \
		exit 1; \
	fi; \
	pinned=$$(git -C $(PROTO) -c safe.directory='*' rev-parse -q --verify $(PROTO_COMMIT):proto/bs_proto.h 2>/dev/null); \
	if [ -z "$$pinned" ]; then \
		echo "Makefile: cannot read proto/bs_proto.h as of the pinned commit."; \
		echo "  PROTO_COMMIT = $(PROTO_COMMIT)"; \
		echo "  $(PROTO) has no such object. Either the clone is on an unrelated"; \
		echo "  history, or that commit was force-pushed away upstream."; \
		echo "  the build cannot prove which protocol it is compiling against, so it stops."; \
		echo "  re-fetch it with:  git -C $(PROTO) fetch origin $(PROTO_COMMIT)"; \
		exit 1; \
	fi; \
	if ! git -C $(PROTO) -c safe.directory='*' cat-file blob $$pinned | cmp -s - $(PROTO)/proto/bs_proto.h; then \
		echo "Makefile: $(PROTO)/proto/bs_proto.h has drifted from the pinned commit."; \
		echo "  pinned   PROTO_COMMIT = $(PROTO_COMMIT)"; \
		echo "           blob         = $$pinned"; \
		echo "  on disk  blob         = $$(git -C $(PROTO) -c safe.directory='*' hash-object proto/bs_proto.h 2>/dev/null)"; \
		echo "           HEAD         = $$(git -C $(PROTO) -c safe.directory='*' rev-parse HEAD 2>/dev/null)"; \
		echo "  bs_proto.h is the wire contract between this client and the Linux server."; \
		echo "  building against a header the pin does not name yields a client that"; \
		echo "  disagrees with the server about ids, flags or struct sizes, and that"; \
		echo "  disagreement surfaces as a runtime desync, never as a compile error."; \
		echo "  see what changed:  git -C $(PROTO) diff $(PROTO_COMMIT) -- proto/bs_proto.h"; \
		echo "  restore the pin:   git -C $(PROTO) checkout $(PROTO_COMMIT) -- proto/bs_proto.h"; \
		echo "  or, if the protocol really did move, bump PROTO_COMMIT above and say why"; \
		echo "  in the comment beside it, as every previous bump does."; \
		exit 1; \
	fi

# ------------------------------------------------------- vendored world guard
#
# deps/blocksmith-server/tools/sync-world-sources.sh mirrors eleven of this
# client's source/world/ files into that repo's game/world/, one way, with
# `cp -f`: this client is the source of truth and the server carries verbatim
# copies because the DEPLOYED server is a standalone clone of that repo alone,
# with no client tree beside it to include from. Those eleven files are the
# block ids, the registry rows, the inventory rules and the tick model. A
# client and a server that disagree about them do not fail to compile; they
# desync at runtime, the same failure mode check-proto-drift above exists for.
#
# That mirror was guarded on ONE side only. The server's game/Makefile has its
# own check-world-drift in `all:` and `test:`, but only under
# ifeq ($(BS_HAVE_CLIENT_WORLD_HEADERS),1) — that is, only when somebody builds
# the SERVER with a client tree sitting beside it. Nothing here checked it, so
# editing source/world/inventory.c, building this client and shipping it
# reported nothing at all, and the drift surfaced later, if ever, on whoever
# next happened to build the server. v1.8.3 Phase 3 edits inventory.c,
# registry.c and block.h — three of the eleven — so "later" is this week.
#
# Shape taken from check-proto-drift above and from the server's own
# check-world-drift: a phony target, `cmp` on bytes, wired into `all`. A third
# mechanism for the same job is deliberately not invented here.
#
# The file list is READ out of sync-world-sources.sh's FILES=( ... ) line, not
# restated below. It is already written down twice — in that script and in the
# server Makefile's VENDORED_WORLD_FILES — and a third hand-copy would be a
# third place to forget when a twelfth file joins the mirror. If that line ever
# stops matching, this stops the build rather than quietly checking an empty
# list, and the success line prints the count it actually compared so a list
# that silently shrank is visible in the build log instead of inferred from a
# guard that said nothing.
#
# `cmp` on bytes, never `git hash-object`. The server repo's .gitattributes is
# `* text=auto eol=lf` and this repo's pins `* -text`, so the two sides run
# through DIFFERENT clean filters: a vendored copy rewritten to LF would hash
# EQUAL to the client's CRLF original while differing on every line ending on
# disk. (That repo carves out `game/world/** -text` last in its .gitattributes
# precisely so the mirrored bytes survive a checkout; this guard is what would
# notice if that carve-out were ever dropped.) A line-ending-only difference is
# still reported AS drift: the mirror's contract is byte-identical, the server's
# own cmp-on-bytes guard will fail on it too, and the fix is the same single
# command — while a guard that normalised endings to be tolerant would be
# reintroducing the very filter that hides the real thing. The message below
# says when a difference is endings-only, so the reader is not sent hunting for
# changed logic that is not there.
#
# A missing deps/blocksmith-server is a HARD failure here, not a skip. The
# server's guard skips when no client tree sits beside it because a standalone
# server IS a supported, shipped configuration over there. The mirror image is
# not true on this side: this client cannot build without that clone at all —
# proto/bs_proto.h comes out of it, and check-proto-drift above already stops
# the build with the same `make deps` instruction when it is absent. So "deps/
# is not there yet" is not a fresh clone this guard breaks; it is a fresh clone
# that was already stopping one prerequisite earlier. Skipping would buy that
# tree nothing and would leave behind a guard that passes when it checked
# nothing, which this project has been bitten by before. Both branches say out
# loud which one they took.
#
# It reports; it never mutates. It does not run sync-world-sources.sh for you:
# deps/blocksmith-server is a separate repo a developer may have deliberately
# parked on a sync branch, and a build that rewrites another checkout behind
# your back is a worse bug than the one it is fixing.
WORLD_SRC	:=	source/world
WORLD_VENDOR	:=	$(PROTO)/game/world
WORLD_MIRROR	:=	$(PROTO)/tools/sync-world-sources.sh

.PHONY: check-world-drift
check-world-drift:
	@if [ ! -e $(WORLD_MIRROR) ]; then \
		echo "Makefile: $(WORLD_MIRROR) is not there."; \
		echo "  deps/blocksmith-server has not been fetched, so the vendored copies"; \
		echo "  of $(WORLD_SRC)/ cannot be compared against their originals here."; \
		echo "  this guard FAILS rather than skips: a check that passes when it had"; \
		echo "  nothing to check is not a check. check-proto-drift stops on the same"; \
		echo "  missing clone, so this is not new breakage for a fresh tree."; \
		echo "  fetch it with:  make deps"; \
		exit 1; \
	fi; \
	files=`sed -n 's/^FILES=(//p' $(WORLD_MIRROR) | sed 's/).*$$//'`; \
	if [ -z "$$files" ]; then \
		echo "Makefile: cannot read the mirrored-file list out of $(WORLD_MIRROR)."; \
		echo "  this target reads that script's FILES=( ... ) line so the names live"; \
		echo "  in one place instead of three. No such line matched, so the script's"; \
		echo "  shape changed and this guard no longer knows what it is meant to"; \
		echo "  compare. The build stops rather than compare nothing."; \
		echo "  re-point the sed in check-world-drift at wherever the list now lives."; \
		exit 1; \
	fi; \
	ok=1; n=0; \
	for f in $$files; do \
		n=`expr $$n + 1`; \
		if [ ! -e $(WORLD_VENDOR)/$$f ]; then \
			echo "Makefile: $(WORLD_VENDOR)/$$f is missing."; \
			echo "  the mirror lists world/$$f but the vendored copy is not on disk:"; \
			echo "  that copy is incomplete, not merely stale."; \
			ok=0; continue; \
		fi; \
		if [ ! -e $(WORLD_SRC)/$$f ]; then \
			echo "Makefile: $(WORLD_SRC)/$$f is missing, but the mirror lists it."; \
			echo "  the client is the source of truth, so there is nothing to mirror"; \
			echo "  from. Either restore it or drop it from $(WORLD_MIRROR)."; \
			ok=0; continue; \
		fi; \
		cmp -s $(WORLD_SRC)/$$f $(WORLD_VENDOR)/$$f && continue; \
		echo "Makefile: $(WORLD_VENDOR)/$$f has drifted from $(WORLD_SRC)/$$f."; \
		if [ "`tr -d '\r' < $(WORLD_SRC)/$$f | cksum`" = "`tr -d '\r' < $(WORLD_VENDOR)/$$f | cksum`" ]; then \
			echo "  the two differ ONLY in line endings — do not go hunting for changed"; \
			echo "  logic. Still drift: the mirror's contract is byte-identical and the"; \
			echo "  server's own cmp-on-bytes guard fails on it too."; \
		else \
			echo "  content differs. See it with:"; \
			echo "    diff -u $(WORLD_VENDOR)/$$f $(WORLD_SRC)/$$f"; \
		fi; \
		ok=0; \
	done; \
	if [ $$n -eq 0 ]; then \
		echo "Makefile: check-world-drift compared 0 files, which cannot be right."; \
		exit 1; \
	fi; \
	if [ $$ok -eq 0 ]; then \
		echo "Makefile: the vendored world sources in $(PROTO) are out of date."; \
		echo "  this client is the source of truth for them and the copy is one-way."; \
		echo "  they carry the block ids, the registry rows, the inventory rules and"; \
		echo "  the tick model: a server built from a stale copy does not fail to"; \
		echo "  compile, it disagrees with this client at runtime and desyncs."; \
		echo "  re-sync them with:  $(WORLD_MIRROR)"; \
		echo "  then review and commit the result in that repo yourself —"; \
		echo "    git -C $(PROTO) diff -- game/world"; \
		echo "  this guard reports; it never edits another checkout for you."; \
		exit 1; \
	fi; \
	echo "world mirror: $$n file(s) in $(WORLD_VENDOR) match $(WORLD_SRC) byte for byte"

# ------------------------------------------------------- diff-store capacity guard
#
# net/blockdiff.h's BLOCKDIFF_MAX_PENDING (65536) and the server's BS_DIFF_MAX
# (deps/blocksmith-server/game/diffstore.h, 131072) are deliberately NOT equal:
# the server's b95f980 raised its own cap precisely so the two ceilings could
# come apart, per-column CHUNK_SUB having removed the coupling that forced this
# console's inbox to be as large as the server's whole store. So this guard does
# NOT check that they match. Making them match is the wrong fix, and this target
# must never be "fixed" by editing BLOCKDIFF_MAX_PENDING to whatever the server
# happens to say today.
#
# What it checks is that the server value the client's REASONING was written
# against is still the server value on disk. blockdiff.h records it as
# BLOCKDIFF_SERVER_DIFF_MAX and spends a long comment on what the gap costs,
# measured: "replayed 131072, accepted 65536, refused 65536". Every one of those
# numbers is true only for a particular BS_DIFF_MAX. If the server moves again,
# that comment silently becomes fiction again — which is exactly what happened
# between b95f980 and now: the old comment asserted the server was 65536 and
# that a join sync therefore "can never overflow this store at all", and nothing
# noticed, for the same reason nothing ever notices prose.
#
# Shape taken from check-proto-drift and check-world-drift above rather than
# inventing a third mechanism: phony target, reports and never mutates, wired
# into `all` so it runs on every build instead of only on a fresh clone.
#
# It reads the header's TEXT rather than asserting in C, because a
# _Static_assert cannot see both values. diffstore.h is a server GAME header,
# not the shared wire contract in proto/bs_proto.h, and it is on no client
# build's include path. Measured, not assumed:
#   gcc -E -dM -I source -I deps/blocksmith-server   over a TU that includes
#   net/blockdiff.h reports BS_DIFF_MAX in 0 lines, with or without the deps -I.
# bs_proto.h does mention BS_DIFF_MAX, but only in prose explaining why
# CHUNK_SUB exists; it defines nothing, so including it would not help either.
DIFFSTORE_H	:=	$(PROTO)/game/diffstore.h
BLOCKDIFF_H	:=	source/net/blockdiff.h

.PHONY: check-diffcap-drift
check-diffcap-drift:
	@if [ ! -e $(DIFFSTORE_H) ]; then \
		echo "Makefile: $(DIFFSTORE_H) is not there."; \
		echo "  deps/blocksmith-server has not been fetched, so the server's diff-store"; \
		echo "  capacity cannot be read and net/blockdiff.h's recorded copy of it cannot"; \
		echo "  be checked. This guard FAILS rather than skips: a check that passes when"; \
		echo "  it had nothing to check is not a check, which is the same stance"; \
		echo "  check-world-drift above takes on the same missing clone."; \
		echo "  fetch it with:  make deps"; \
		exit 1; \
	fi; \
	srv=`sed -n 's/^[[:space:]]*#define[[:space:]][[:space:]]*BS_DIFF_MAX[[:space:]][[:space:]]*\([0-9][0-9]*\).*$$/\1/p' $(DIFFSTORE_H) | head -1`; \
	cli=`sed -n 's/^[[:space:]]*#define[[:space:]][[:space:]]*BLOCKDIFF_SERVER_DIFF_MAX[[:space:]][[:space:]]*\([0-9][0-9]*\).*$$/\1/p' $(BLOCKDIFF_H) | head -1`; \
	if [ -z "$$srv" ]; then \
		echo "Makefile: cannot read BS_DIFF_MAX out of $(DIFFSTORE_H)."; \
		echo "  no '#define BS_DIFF_MAX <number>' line matched, so that header's shape"; \
		echo "  changed and this guard no longer knows what it is comparing. The build"; \
		echo "  stops rather than compare nothing."; \
		echo "  re-point the sed in check-diffcap-drift at wherever the value now lives."; \
		exit 1; \
	fi; \
	if [ -z "$$cli" ]; then \
		echo "Makefile: cannot read BLOCKDIFF_SERVER_DIFF_MAX out of $(BLOCKDIFF_H)."; \
		echo "  that macro is this client's record of the server capacity its pending-diff"; \
		echo "  store was reasoned against. Without it there is nothing to check against,"; \
		echo "  so the build stops rather than pass having compared nothing."; \
		exit 1; \
	fi; \
	if [ "$$srv" != "$$cli" ]; then \
		echo "Makefile: the server's diff-store capacity has moved."; \
		echo "  $(DIFFSTORE_H): BS_DIFF_MAX               = $$srv"; \
		echo "  $(BLOCKDIFF_H): BLOCKDIFF_SERVER_DIFF_MAX = $$cli"; \
		echo ""; \
		echo "  DO NOT fix this by changing BLOCKDIFF_MAX_PENDING to match. The two caps"; \
		echo "  are deliberately different - see blockdiff.h and the server's b95f980."; \
		echo "  What has gone stale is the client's RECORD of the server number, and the"; \
		echo "  measured overflow figures in blockdiff.h's comment that depend on it."; \
		echo ""; \
		echo "  to fix, in this order:"; \
		echo "    1. re-read blockdiff.h's capacity comment and decide whether the gap is"; \
		echo "       still the right call at the new server number;"; \
		echo "    2. update BLOCKDIFF_SERVER_DIFF_MAX to $$srv;"; \
		echo "    3. re-run the host suite so test_server_replay_overflow re-measures"; \
		echo "       against it, and paste its new [measured] line into that comment:"; \
		echo "         tools/run_host_tests.sh"; \
		exit 1; \
	fi; \
	echo "diff-store caps: server BS_DIFF_MAX = $$srv, client records $$cli (client's own cap stays smaller, deliberately)"

$(HYDRO)/hydrogen.h:
	@mkdir -p deps
	git clone -q $(HYDRO_REPO) $(HYDRO)
	git -C $(HYDRO) -c safe.directory='*' checkout -q $(HYDRO_COMMIT)
	@test "$$(git -C $(HYDRO) -c safe.directory='*' rev-parse HEAD)" = "$(HYDRO_COMMIT)" \
	  || (echo "libhydrogen checkout is not at the pinned commit"; exit 1)
	@echo "libhydrogen pinned at $(HYDRO_COMMIT)"
	@#
	@# Upstream has no 3DS entropy backend — impl/random.h's dispatch chain ends in
	@# "#error Unsupported platform" for __3DS__, so a fresh checkout does not compile
	@# at all. patches/libhydrogen/ carries the backend (PS_GenerateRandomBytes, the
	@# console's hardware CSPRNG) and the one-hunk dispatch entry that reaches it.
	@#
	@# Applied here rather than committed as a forked copy of libhydrogen for the same
	@# reason the checkout is pinned: the crypto stays upstream's, byte for byte, and
	@# the delta this project owns is 3 lines plus one new file, reviewable on its own.
	@# git apply, not patch(1) — devkitPro's MSYS2 ships no patch(1), and this recipe
	@# already requires git.
	cp patches/libhydrogen/n3ds.h $(HYDRO)/impl/random/n3ds.h
	git -C $(HYDRO) -c safe.directory='*' apply $(CURDIR)/patches/libhydrogen/0001-3ds-random-backend.patch
	@grep -q "random/n3ds.h" $(HYDRO)/impl/random.h \
	  || (echo "libhydrogen 3DS random backend did not apply"; exit 1)
	@echo "libhydrogen patched for 3DS entropy"

# libhydrogen is third-party; do not subject it to this project's warning set.
hydrogen.o: CFLAGS := $(filter-out -Wall -Wextra,$(CFLAGS)) -w

#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(GRAPHICS),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
PICAFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.v.pica)))
SHLISTFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.shlist)))
GFXFILES	:=	$(foreach dir,$(GRAPHICS),$(notdir $(wildcard $(dir)/*.t3s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
#---------------------------------------------------------------------------------
	export LD	:=	$(CC)
#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
	export LD	:=	$(CXX)
#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------

#---------------------------------------------------------------------------------
ifeq ($(GFXBUILD),$(BUILD))
#---------------------------------------------------------------------------------
export T3XFILES :=  $(GFXFILES:.t3s=.t3x)
#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
export ROMFS_T3XFILES	:=	$(patsubst %.t3s, $(GFXBUILD)/%.t3x, $(GFXFILES))
export T3XHFILES		:=	$(patsubst %.t3s, $(BUILD)/%.h, $(GFXFILES))
#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------

# world/light.c is included by world/world.c rather than compiled standalone —
# tools/run_host_tests.sh names its files explicitly and predates the module, so
# the engine's symbols must arrive inside world.o for every host link. Filtered
# out here so the console build does not define them a second time. See the
# comment at the include site in world/world.c; edit the two halves together.
CFILES := $(filter-out light.c,$(CFILES))

export OFILES_SOURCES 	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES)) \
			$(PICAFILES:.v.pica=.shbin.o) $(SHLISTFILES:.shlist=.shbin.o) \
			$(addsuffix .o,$(T3XFILES))

export OFILES := $(OFILES_BIN) $(OFILES_SOURCES)

export HFILES	:=	$(PICAFILES:.v.pica=_shbin.h) $(SHLISTFILES:.shlist=_shbin.h) \
			$(addsuffix .h,$(subst .,_,$(BINFILES))) \
			$(GFXFILES:.t3s=.h)

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export _3DSXDEPS	:=	$(if $(NO_SMDH),,$(OUTPUT).smdh)

ifeq ($(strip $(ICON)),)
	icons := $(wildcard *.png)
	ifneq (,$(findstring $(TARGET).png,$(icons)))
		export APP_ICON := $(TOPDIR)/$(TARGET).png
	else
		ifneq (,$(findstring icon.png,$(icons)))
			export APP_ICON := $(TOPDIR)/icon.png
		endif
	endif
else
	export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_SMDH)),)
	export _3DSXFLAGS += --smdh=$(CURDIR)/$(TARGET).smdh
endif

ifneq ($(ROMFS),)
	export _3DSXFLAGS += --romfs=$(CURDIR)/$(ROMFS)
endif

.PHONY: all clean cia

#---------------------------------------------------------------------------------
# check-proto-drift and check-world-drift are listed FIRST so both contract
# checks run before any compiling: a build that is going to be rejected for
# protocol drift, or for a vendored copy of the world sources that no longer
# matches source/world/, should not spend two minutes producing objects nobody
# may use. `cia: all` inherits it,
# so release packaging is covered by the same one wiring. This is the only
# always-runs entry point in this Makefile — there is no `test` target here to
# hang it off as well (the host suites run from tools/run_host_tests.sh, not
# from make), which is the one asymmetry with check-world-drift's `all: test:`
# pair on the server side.
all: check-proto-drift check-world-drift check-diffcap-drift $(BUILD) $(GFXBUILD) $(DEPSDIR) $(ROMFS_T3XFILES) $(T3XHFILES)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
# Step 10.1. Depends on `all` rather than repeating its prerequisites: the packaging
# inputs makerom needs (blocksmith.elf, and the SMDH built from gfx/icon.png) are outputs
# of the normal build, and a `cia` target that could run against a stale or missing .elf
# would quietly produce a .cia that does not match the source it is named after.
#
# The work itself stays in tools/make_cia.sh rather than moving here. makerom and
# bannertool each take a dozen arguments and want real shell quoting; expressing that in
# make recipe syntax buys nothing and makes the failure messages worse. The script also
# runs standalone from a devkitPro MSYS2 prompt, which is how it actually gets debugged.
cia: all
	@if [ -z "$(BS_PSK)" ]; then \
	    echo "ERROR: BS_PSK is empty."; \
	    echo "       A release CIA built without the network PSK cannot connect to"; \
	    echo "       any server, and would fail silently in the player's hands."; \
	    echo "       Re-run:  make cia BS_PSK=<64 hex characters>"; \
	    exit 1; \
	fi
# Checked against the linked artifact, not against the flags, because make does not
# rebuild on a changed -D: after a plain `make`, `make cia BS_PSK=...` would relink
# stale objects that never saw the define and package a CIA that cannot connect.
# Prints a count, never the value.
	@if [ "$$(grep -c -F -- "$(BS_PSK)" $(TARGET).elf 2>/dev/null)" = "0" ]; then \
	    echo "ERROR: BS_PSK is set but does not appear in $(TARGET).elf."; \
	    echo "       The objects predate the flag — make does not rebuild on a"; \
	    echo "       changed -D. Run 'make clean' and build again:"; \
	    echo "       make clean && make cia BS_PSK=<64 hex characters>"; \
	    exit 1; \
	fi
	@tools/make_cia.sh

$(BUILD):
	@mkdir -p $@

ifneq ($(GFXBUILD),$(BUILD))
$(GFXBUILD):
	@mkdir -p $@
endif

ifneq ($(DEPSDIR),$(BUILD))
$(DEPSDIR):
	@mkdir -p $@
endif

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(OUTPUT).smdh $(TARGET).elf $(GFXBUILD)

#---------------------------------------------------------------------------------
$(GFXBUILD)/%.t3x	$(BUILD)/%.h	:	%.t3s
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@tex3ds -i $< -H $(BUILD)/$*.h -d $(DEPSDIR)/$*.d -o $(GFXBUILD)/$*.t3x

#---------------------------------------------------------------------------------
else

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
$(OUTPUT).3dsx	:	$(OUTPUT).elf $(_3DSXDEPS)

$(OFILES_SOURCES) : $(HFILES)

$(OUTPUT).elf	:	$(OFILES)

#---------------------------------------------------------------------------------
# you need a rule like this for each extension you use as binary data
#---------------------------------------------------------------------------------
%.bin.o	%_bin.h :	%.bin
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

#---------------------------------------------------------------------------------
.PRECIOUS	:	%.t3x %.shbin
#---------------------------------------------------------------------------------
%.t3x.o	%_t3x.h :	%.t3x
#---------------------------------------------------------------------------------
	$(SILENTMSG) $(notdir $<)
	$(bin2o)

#---------------------------------------------------------------------------------
%.shbin.o %_shbin.h : %.shbin
#---------------------------------------------------------------------------------
	$(SILENTMSG) $(notdir $<)
	$(bin2o)

#---------------------------------------------------------------------------------
# gfx/atlas.t3s names gfx/atlas.png, and nothing in the build knows it.
#
# The texture is built by the %.t3x rule in $(DEVKITARM)/3ds_rules, whose only prerequisite
# is the .t3s. tex3ds does write a dep file naming the .png — but it writes it as $*.d, so
# for this project it is build/atlas.d, which is also where the C compiler writes the deps
# for source/gfx/atlas.c. Both stems are "atlas". The compiler always runs second, so the
# dep file that names the .png is destroyed on every single build and never survives to be
# read back by the -include below.
#
# The effect is that editing the atlas does not rebuild the texture. The .t3s is untouched,
# make has nothing to do, and the console goes on rendering the previous atlas while the
# .png on disk shows the new one — which looks like a rendering bug, not a build one.
# Found the hard way when step 5.3 added wood and leaves: the trees drew solid black,
# because their UVs pointed at atlas cells that existed only in the .png. build/atlas.t3x
# was 1,957 bytes dated 11:28 against a gfx/atlas.png of 3,636 bytes dated 11:35.
#
# Stated as an explicit prerequisite rather than by overriding the pattern rule with a
# renamed dep file: this is one atlas built from one image, and a line that says so plainly
# cannot be defeated by whichever rule happens to win. atlas.png is found through VPATH,
# which already covers $(GRAPHICS).
#---------------------------------------------------------------------------------
atlas.t3x: atlas.png

# Step 8.3. The same trap, the same fix: gfx/font.t3s names gfx/font.png, build/font.d is
# written by tex3ds and then overwritten by the compiler's deps for source/gfx/font.c, and
# without this line editing the font would leave the console drawing the old glyphs.
# tools/make_font.py regenerates font.png; this is what makes that regeneration reach the
# build. Not folded into one rule with atlas because two separate one-line facts are easier
# to be right about than one clever pattern.
font.t3x: font.png

# v1.8.1 task 50. The same trap a third time, and it bites exactly as hard: gfx/crackatlas.t3s
# names gfx/crackatlas.png, tex3ds writes build/crackatlas.d, and the compiler's deps for
# source/gfx/crackatlas.c overwrite it — both stems are "crackatlas". tools/make_crack_atlas.py
# regenerates the .png; without this line that regeneration would never reach the build, and the
# console would go on drawing the previous eight stages. On a progressive animation that reads
# as the break TIMING being wrong rather than as the texture being stale, which is a much longer
# way round to the same one-line fix.
crackatlas.t3x: crackatlas.png

-include $(DEPSDIR)/*.d

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
