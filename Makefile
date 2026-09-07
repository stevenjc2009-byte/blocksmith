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
SOURCES		:=	source source/app source/audio source/entity source/gfx source/debug source/net source/scene source/shaders source/world deps/libhydrogen
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
# -Werror, same as the host build (tools/run_host_tests.sh) and the server build already
# carry. Without it the console build has shipped a .3dsx from a compile that emitted two
# -Wformat= diagnostics and still exited 0, because make only fails on a non-zero compiler
# exit and a warning is exit 0. A warning here is a defect that reached a release; make it
# stop the build instead. If a warning ever cannot be fixed, fix the code — do not add
# -Wno-* and do not exclude the file.
CFLAGS	:=	-g -Wall -Wextra -Werror -O3 -mword-relocations \
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
# Caveat worth stating: this pin names a working BRANCH head rather than a released
# server, which is the opposite of what the 05c14fd6 bump above was for. It should
# move again to whichever release absorbs docs/registry-crc-correction.
#
# It moves to 533aee1 for v1.8.3 Phase 4, and this bump is NOT the server-side-only
# kind the paragraph above describes. 533aee1 adds two app ids -- BS_APP_REGISTRY_DEFS
# 0x0E and BS_APP_WORLD_GEN 0x0F, the latter S->C only, carrying {gen_version u16 LE}
# -- so the header genuinely changed shape, and the server's VERSION went 1.8.1 ->
# 1.9.0 rather than to a patch. A minor bump is this repo's precedent for a wire
# change (v1.6.0, v1.8.0); v1.5.1 is the fix-only precedent.
#
# Release ORDER does not matter here, and that was measured rather than assumed. A
# probe built against a pristine v1.8.2 client -- whose pinned bs_proto.h, 05c14fd6,
# contains zero mentions of WORLD_GEN -- was driven through its real networldUpdate()
# drain with 0x0F wedged between WORLD_INFO and WORLD_SYNC: seed unchanged, the packet
# BEHIND the unknown id still parsed, nothing sent back. That holds because the S->C
# dispatch ends in `default: break;` and the framing is message-oriented, not a byte
# stream -- bsnet_transport.c hands networldApplyPayload() a length taken from the
# queue slot, never from parsing the type byte, so an unknown id cannot advance a
# cursor wrongly because there is no cursor. So the server may ship first.
#
# The asymmetry is real and one-directional: a new C->S id would meet
# handle_app_payload()'s `default: send_kick()`, so that direction is server-first
# ONLY. And none of this licenses WIDENING an existing message -- networld.c's
# `if (len != BS_WORLD_INFO_BYTES) return;` is strict equality, so BS_APP_WORLD_INFO
# still cannot grow.
#
# 2026-09-02, 533aee14 -> bce14b65 (blocksmith-server v1.9.1, a PUSHED tag). Bumped for a
# COMMENT-ONLY change, and that was checked before bumping rather than after:
#
#     git -C deps/blocksmith-server diff 533aee14 bce14b65 -- proto/bs_proto.h
#     proto/bs_proto.h | 11 +++++++----   (7 insertions, 4 deletions)
#
# Every one of those lines is inside the block comments on BS_INV_OP_PICKUP and
# BS_INV_OP_CONSUME. The values did NOT move -- PICKUP is still 0x05, CONSUME still 0x06,
# BS_INV_OP_COUNT unchanged -- and no struct, field, size or id changed anywhere in the
# file. The prose now says the server validates `a` through its block registry
# (inventoryCanHold) instead of `a < BS_BLOCK_COUNT`, which is v1.9.1's actual change and
# lives in game/bsgame.c and game/validate.h, not in the wire format.
#
# So this bump is a no-op for the built client: the header it compiles against is
# byte-equivalent in every declaration. It is recorded anyway because the guard hashes the
# whole file on purpose. A guard that ignored comments would have to parse C to know which
# hunks were "only" comments, and a guard that can be argued with is not a guard -- the
# same strictness that makes this bump feel like paperwork is what makes it catch the real
# thing. Noise here is the price of the alarm working.
#
# NOTE for whoever widens the client's wire span: v1.9.1's server-side half is what makes
# that safe, but the client half is NOT done. world/inventory.h's inventoryItemOnWire()
# still reads `item < BLOCK_COUNT`. See the dated block above that function.
#
# 2026-09-03, bce14b65 -> 32795b9a (blocksmith-server v1.9.3, a PUSHED tag). This one is NOT
# the comment-only kind above it. The diff is 35 insertions and 1 deletion, and the single
# deleted line is the one that matters:
#
#     -#define BS_RECIPE_COUNT      4u   /* mirrors RECIPE_COUNT (world/crafting.h) */
#     +#define BS_RECIPE_COUNT      5u   /* mirrors RECIPE_COUNT (world/crafting.h) */
#
# The other 34 lines are the block comment explaining it. v1.8.12 adds a fifth recipe,
# RECIPE_COAL_ORE_TO_TORCH -- one coal ore to four torches -- and bs_proto.h RESTATES the
# recipe count for the wire. It is NOT one of the eleven files tools/sync-world-sources.sh
# mirrors, so the sync ran completely clean (every one of the eleven `synced` or `unchanged`,
# exit 0) and the server's host suite still went red at SUITE_EXIT=2:
#
#     validate.c:57:1: error: static assertion failed: "recipe count must track world/crafting.h"
#
# which is that assert doing exactly the job it exists for. Recorded because "the mirror script
# said unchanged and exited 0" is NOT the same statement as "client and server agree" -- the
# second time this tree has proved that, the first being the registry wire record size two
# paragraphs up.
#
# Worth correcting one plausible-sounding reading of this constant before someone acts on it:
# BS_RECIPE_COUNT does not gate anything at runtime. A whole-tree grep returns five lines and
# exactly ONE that is code -- the assert. The runtime bound is the server's game/bsgame.c:1332,
# `if (a < RECIPE_COUNT)`, using the MIRRORED constant out of world/crafting.h, which the sync
# script does carry. So this line is a pin, not a gate. Its whole job is to fail the build when
# the two drift, and that is what it just did.
#
# RELEASE ORDER, and unlike the 533aee1 paragraph above this one is NOT free. Read that
# paragraph's rule first: a new C->S id is server-first ONLY because it would meet
# handle_app_payload()'s `default: send_kick()`. This change adds no id, so nothing gets
# kicked -- but bsgame.c's `if (a < RECIPE_COUNT)` has **no else**. A daemon still running
# v1.9.2 was compiled with 4 baked in, so a v1.8.12 client on it can craft everything EXCEPT
# the torch, and the request is dropped in silence: no refusal packet, nothing on screen, just
# a recipe that does nothing when tapped. The one recipe this release is named for is the one
# that fails, and it fails invisibly. Server v1.9.3 is therefore released and tagged BEFORE
# this bump, not after it, and CT 105 has to be updated for multiplayer crafting to work.
#
# Verified rather than assumed, before bumping: `git ls-remote origin` shows both
# refs/heads/v1.9.3 and refs/tags/v1.9.3 present on the remote, the tag dereferences to
# 32795b9a, and 32795b9a:proto/bs_proto.h is blob 8303be83 -- the same blob the drift guard
# was reporting on disk. A pin naming a commit that exists only locally would break a fresh
# clone, which is the failure this check is meant to prevent, so it is checked here and not
# taken on trust.
#
# BUMPED 2026-09-03 for v1.8.14 "Animals", 32795b9a -> a22eea3a (server v1.9.4). This one is
# the OPPOSITE case to the v1.9.3 bump above, and the difference is worth stating because it
# would otherwise read as the same event:
#
#   proto/bs_proto.h DID NOT CHANGE. Its blob is 8303be83 at 32795b9a, at a22eea3a and on
#   disk -- checked all three ways, not assumed -- so check-proto-drift was green before this
#   bump and is green after it. Nothing in v1.9.4 touches the wire header: BS_RECIPE_COUNT
#   stays 5u because v1.8.14 adds no recipe, and BS_BLOCK_COUNT stays 8u because appending
#   core rows past the frozen wire item span does not move it.
#
# So this line moves to NAME THE DEPLOYED SERVER, not to unbreak a build. What v1.9.4 actually
# carries is four raw meat rows (ids 34..37) vendored into the mirrored world sources, which
# moves the registry core count 34 -> 38 and the crc16 0xE15E -> 0x9610.
#
# RELEASE ORDER, and this one is NOT free either -- but for the registry reason rather than
# the recipe reason. networld.c's registryMatchesInfo() is a join-time lockstep on
# rev/count/crc16, so a v1.8.14 client meeting a daemon still on the 34-row table is refused
# outright. That failure is loud and recoverable; the reverse ordering is the quiet, worse one.
# Server v1.9.4 is therefore released and tagged BEFORE this bump, exactly as v1.9.3 was.
#
# Verified rather than assumed, before bumping, the same three ways as the entry above:
# `git ls-remote origin` shows refs/heads/v1.9.4 and refs/tags/v1.9.4 both present on the
# remote, refs/tags/v1.9.4^{} dereferences to a22eea3a (an annotated tag, and only a TAG is
# deployable), and a22eea3a:proto/bs_proto.h is blob 8303be83 -- the same blob the drift guard
# reads on disk. A pin naming a commit that exists only locally would break a fresh clone,
# which is the failure this check exists to prevent.
#
# BUMPED 2026-09-03 for v1.8.15 "Furnace", a22eea3a -> 11caee37 (server v1.9.6). This one is
# the FIRST case of the three: proto/bs_proto.h ACTUALLY CHANGED, so this bump unbreaks a
# build rather than merely naming a newer deployed server. The v1.9.3 entry moved the pin
# because the header moved; the v1.9.4 entry moved it although the header had not; this one
# is the v1.9.3 shape again, and the distinction is the whole reason those two entries were
# written to read differently.
#
#   WHAT MOVED: BS_RECIPE_COUNT 5u -> 6u, and nothing else. blob 8303be83 -> 5a9886c2.
#   v1.8.15 adds RECIPE_STONE_TO_FURNACE (8 stone -> 1 furnace) to world/crafting.h, taking
#   RECIPE_COUNT 5 -> 6, and the server carries a compiled-in
#   _Static_assert(RECIPE_COUNT == BS_RECIPE_COUNT) that pins the two together. Leaving the
#   wire constant at 5u does not produce a subtle desync -- it fails the server's own build,
#   loudly, which is what that assert is for. BS_BLOCK_COUNT stays 8u: the five new core rows
#   (38..42) land past the frozen wire item span, exactly as v1.9.4's four meat rows did.
#
#   WHY THE RECIPE IS APPENDED LAST, since it is this bump's entire content: bsgame.c bounds
#   an incoming wire recipe INDEX against RECIPE_COUNT, so recipes are identified by position
#   over the wire and nothing else. Inserting anywhere but the end silently renumbers every
#   recipe above it, and an old client would craft the wrong thing against a new server
#   without either side noticing. Appending only ever adds an index neither side had before.
#
# RELEASE ORDER, and it is not free here either. A v1.8.15 client meeting a daemon still on
# the 38-row table is refused at networld.c's registryMatchesInfo() lockstep on
# rev/count/crc16 (43 rows, crc16 0x9610 -> 0xE486), which is loud and recoverable. The
# reverse ordering -- new daemon, old client -- is the quiet one. Server v1.9.6 is therefore
# released and tagged BEFORE this bump, exactly as v1.9.3 and v1.9.4 were.
#
# Verified rather than assumed, before bumping, the same three ways as both entries above:
# `git ls-remote origin` shows refs/heads/v1.9.6 and refs/tags/v1.9.6 both present on the
# remote, refs/tags/v1.9.6^{} dereferences to 11caee37 (an annotated tag, and only a TAG is
# deployable), and 11caee37:proto/bs_proto.h is blob 5a9886c2 -- byte-identical to what
# `git hash-object proto/bs_proto.h` reports on disk. A pin naming a commit that exists only
# locally would break a fresh clone, which is the failure this check exists to prevent.
#
# BUMPED 2026-09-05, 11caee37 -> 2c822a09 (server v1.9.7). A THIRD shape, and worth naming
# because it is neither of the two above: proto/bs_proto.h changed, but not one opcode, id,
# flag, length or struct moved with it. blob 5a9886c2 -> d6f597df, and the whole delta is one
# comment on BS_APP_WORLD_GEN. So this bump unbreaks a build the way the v1.9.6 entry did,
# while costing an old client exactly nothing the way the v1.9.4 entry did.
#
#   WHAT MOVED: the paragraph that said declaring a generator above 1 needs water on the wire
#   first. Measured this session and withdrawn -- the observation it rests on is right and is
#   kept, but water cannot reach this wire at all. On this side, water writes blocks only
#   through worldSet; worldSet's edit hook does exactly one thing with them (a local
#   waterNotify) and is not a sender; networldSendBlockEdit has exactly one non-test caller,
#   scene/interact.c's own break/place; and source/world/water_test.c links water.c WITHOUT
#   net/networld.c and builds, so a send from water.c would be an undefined symbol. What two
#   clients can disagree about is flow, not the world.
#
#   The server change behind it is real, though it is not on the wire: a state-dir with no
#   block_diffs.bin now mints generator 5 rather than 1, and --world-gen refuses to contradict
#   a stored value on a state-dir that has edits unless --world-gen-force says so.
#
#   RELEASE ORDER still ran server-first, and here that is a formality rather than a
#   correctness argument: with no wire constant moving, neither ordering can desync anything.
#   It was done anyway so the pin never names a commit a fresh clone cannot fetch, which is
#   the failure check-proto-drift exists to prevent.
#
# Verified the same three ways as every entry above, before bumping: `git ls-remote origin`
# shows refs/heads/v1.9.7 and refs/tags/v1.9.7 both present on the remote, refs/tags/v1.9.7^{}
# dereferences to 2c822a09 (an annotated tag, and only a TAG is deployable), and
# 2c822a09:proto/bs_proto.h is blob d6f597df -- byte-identical to what `git hash-object
# proto/bs_proto.h` reports on disk.
#
# v1.8.20 -> 84b1400d4920b3c23feccaf9936aae12cbc4e255, server v1.9.8, which adds
# BS_APP_TIME_SYNC (0x10, S->C only, one uint64 LE) so time of day is server-authoritative.
# The wire change is purely ADDITIVE and one-directional, so the usual lockstep does not
# apply in both directions: an old client against a new server ignores 0x10 at
# net/networld.c's `default: break;`, and a new client against an old server simply never
# receives one and keeps running its own clock. What is NOT safe, and must never be added,
# is the reverse direction -- the server's handle_app_payload ends in send_kick(), so a new
# client-to-server opcode would disconnect every player on an older server.
#
# Verified the same three ways as every entry above, and one further way that no entry above
# needed. The three: `git ls-remote origin` shows refs/heads/v1.9.8 and refs/tags/v1.9.8 both
# present on the remote; refs/tags/v1.9.8^{} dereferences to 84b1400d (an annotated tag, and
# only a TAG is deployable); and 84b1400d:proto/bs_proto.h is blob bd50364e -- byte-identical
# to what `git hash-object proto/bs_proto.h` reports on disk.
#
# The fourth was added because this pin was, for a while, exactly the thing the three checks
# above cannot catch. It was first written against af993644, a commit that existed only in
# the local deps/ clone: every one of the three checks reads that same local clone, so all
# three passed while a fresh clone running `make deps` could not have fetched the commit at
# all. So the pin is now also verified by actually taking one -- `git clone --branch v1.9.8
# --depth 1` from the remote into a scratch directory, whose HEAD is 84b1400d, whose VERSION
# reads 1.9.8, and whose proto/bs_proto.h compares byte-identical to the one on disk. If this
# pin is ever moved again, take the clone; local hashes agreeing with each other proves only
# that the local clone is self-consistent.
#
# af993644 does not exist on the remote and never will. It was amended into 84b1400d before
# anything was pushed, because it had shipped VERSION still reading 1.9.7 on a commit
# labelled v1.9.8 -- not cosmetic, since tools/bs-update reads that file as CURRENT_VERSION
# and checks out refs/tags/v${CURRENT_VERSION} from it, so a v1.9.8 server would have
# identified itself as v1.9.7 to its own updater. Every release commit before it bumps
# VERSION; that one did not.
#
# The commit also landed on branch v1.9.7 while being labelled v1.9.8, which would have
# pushed the v1.9.7 branch one commit past its own v1.9.7 tag. It now has its own v1.9.8
# branch, and refs/heads/v1.9.7 was moved back to 2c822a09 so that branch, tag and remote
# agree again.
#
# v1.9.0 -> 7454d0253f0bc59567e08018060bd0f3ea95f2d2, server v1.9.9, which carries the chest:
# the registry gains the chest block row and crafting.c gains RECIPE_PLANKS_TO_CHEST, so
# BS_RECIPE_COUNT moves 6u -> 7u. That constant is a hand-written restatement of
# world/crafting.h's RECIPE_COUNT, because proto/bs_proto.h is deliberately NOT one of the
# eleven files tools/sync-world-sources.sh mirrors; the server's own
# _Static_assert(RECIPE_COUNT == BS_RECIPE_COUNT) at game/validate.c:57 is what catches the
# restatement failing to follow, and it catches it as a build failure rather than a desync.
#
# RELEASE ORDER was server-first and here that is a correctness argument, not a formality,
# for a reason that is NOT the recipe count. The recipe direction is benign both ways: the
# server's craft bound at game/bsgame.c:1609 is symbolic (`if (a < RECIPE_COUNT)`), so a new
# client's chest craft against an old server is silently DROPPED, not kicked -- the kick path
# is handle_app_payload's unknown-opcode tail, and no new opcode ships here. What forces the
# order is the REGISTRY: the chest row changes the registry hash, and registryMatchesInfo()
# refuses a join outright when the two sides disagree. So an old client cannot join a v1.9.9
# server at all, which is the loud, correct failure -- but it means the server has to exist
# first, or a client shipping the chest row could join nothing.
#
# Verified the same four ways as the entry above, and a fifth. The four: `git ls-remote
# origin` shows refs/heads/v1.9.9 and refs/tags/v1.9.9 both present on the remote;
# refs/tags/v1.9.9 resolves to 7454d025; 7454d025:proto/bs_proto.h is blob e57b3fe0 --
# byte-identical to what `git hash-object proto/bs_proto.h` reports on disk; and a genuine
# fresh `git clone` of the remote into a scratch directory, checked out at 7454d025, whose
# proto/bs_proto.h `cmp`s IDENTICAL against the one on disk.
#
# The fifth was added because even a fresh clone and the local one are still both git, over
# the same protocol. VERSION and the header blob were also read straight off the GitHub API
# at ?ref=7454d025 -- a different transport entirely -- returning 1.9.9 and e57b3fe0. Note
# one trap found while doing it: reading VERSION out of a fresh clone BEFORE checking out the
# pinned commit reads the default branch instead, which here says 1.1.0 and looks alarming.
# Check out the commit first, or read the file at an explicit ref.
#
# v1.9.0 -> 13843c1cb940758143e347df54bacc9c50770146, server v1.9.10, which carries the chest
# WIRE: BS_APP_SERVER_CAPS (0x11), BS_APP_CHEST_STATE (0x12) and BS_APP_CHEST_ACTION (0x13),
# plus BS_CAP_CHESTS. proto/bs_proto.h gains those three opcode numbers and the caps bit and
# nothing else -- BS_PROTO_VERSION stays 1u and BS_RECIPE_COUNT stays 7u, so this bump moves
# neither the registry hash nor the recipe pin.
#
# RELEASE ORDER here is the OPPOSITE argument to the v1.9.9 entry above, and it is worth being
# explicit about since the two entries sit next to each other. v1.9.9 had to ship first because
# the chest registry row changes the registry hash and registryMatchesInfo() refuses the join
# outright. Nothing in v1.9.10 touches the registry, so there is no join gate on either side:
# a v1.9.0 client against a v1.9.9 server simply never sees BS_CAP_CHESTS, never sends
# CHEST_ACTION, and gets client-local chests instead of synced ones. Degraded, not refused, and
# not kicked -- which is precisely what the capability bit was added to buy, because the kick
# path is handle_app_payload's unknown-opcode tail and this release DOES ship new opcodes.
#
# Verified: refs/heads/v1.9.10 and refs/tags/v1.9.10 are both on the remote; the annotated tag
# dereferences (refs/tags/v1.9.10^{}) to 13843c1c, the same commit as the branch head; and the
# GitHub release at that tag is the one releases/latest 302-redirects to. The blob check that
# check-proto-drift itself performs is the binding one and is run below, not restated here.

PROTO_COMMIT	:=	13843c1cb940758143e347df54bacc9c50770146
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

# ------------------------------------------------------ chunk dimension guard
#
# CHUNK_DIM (source/world/chunk.h) and BS_CHUNK_DIM in the server's
# proto/bs_proto.h are the same number written twice, and until this target
# nothing anywhere compared them. bs_proto.h says so in prose -- "The client's CHUNK_DIM
# (source/world/chunk.h:50), restated here" -- and prose does not fail a build.
# That is exactly the shape of the hand-copied capacity constant that shipped
# v1.6.0 unbootable: a correct comment naming its source, wrong within one
# release, with no compiler anywhere in a position to notice.
#
# This one is worse than a capacity. BS_CHUNK_DIM is what bs_col_of() shifts by,
# so every CHUNK_SUB, CHUNK_UNSUB and CHUNK_DIFFS column coordinate on the wire
# is derived from it. A divergence would not run out of room; it would file
# every edit under a column nobody subscribed to, on an unordered transport that
# reports nothing, in a build that compiles clean on BOTH sides.
#
# A _Static_assert cannot do this job here. Measured, not assumed: no translation
# unit under source/ includes both headers -- of the .c files that include
# bs_proto.h, none also includes world/chunk.h -- and `grep -rn BS_CHUNK_DIM
# source/` matches 0 lines, so no client TU has the server's value in scope at
# all. chunk.h is also not one of the eleven files sync-world-sources.sh mirrors
# into the server tree, so game/validate.c -- where every other cross-repo static
# assert lives -- cannot see it from the other direction either. Vendoring a
# twelfth file to make it visible was rejected: it enlarges a sync surface that
# is maintained by hand, to buy one integer, and chunk.h includes world/block.h
# so it would not arrive alone.
#
# The shape is check-diffcap-drift's, immediately above, not a fourth mechanism:
# a phony target that reads the TEXT of both headers at build time, reports and
# never mutates, wired into `all` so it runs on every build rather than only on
# a fresh clone. Neither value is transcribed into this Makefile -- a copy here
# would be a third place to forget, which is the bug this is about. Both file
# names and both values are printed on failure, so a future failure explains
# itself without archaeology.
#
# BS_CHUNK_DIM_SHIFT is deliberately NOT checked here. game/validate.c already
# asserts (1u << BS_CHUNK_DIM_SHIFT) == BS_CHUNK_DIM in C, in a translation unit
# that sees both; text-matching it a second time would be a weaker copy of a
# guard that is already the build.
CHUNK_H		:=	source/world/chunk.h
PROTO_H		:=	$(PROTO)/proto/bs_proto.h

.PHONY: check-chunkdim-drift
check-chunkdim-drift:
	@if [ ! -e $(PROTO_H) ]; then \
		echo "Makefile: $(PROTO_H) is not there."; \
		echo "  deps/blocksmith-server has not been fetched, so BS_CHUNK_DIM cannot be"; \
		echo "  read and $(CHUNK_H)'s CHUNK_DIM cannot be checked against it. This guard"; \
		echo "  FAILS rather than skips: a check that passes when it had nothing to"; \
		echo "  check is not a check, the same stance check-world-drift and"; \
		echo "  check-diffcap-drift above take on the same missing clone."; \
		echo "  fetch it with:  make deps"; \
		exit 1; \
	fi; \
	if [ ! -e $(CHUNK_H) ]; then \
		echo "Makefile: $(CHUNK_H) is not there."; \
		echo "  that header is where this client defines CHUNK_DIM, the value the wire"; \
		echo "  contract's BS_CHUNK_DIM is a copy of. With it gone there is nothing to"; \
		echo "  compare, so the build stops rather than pass having compared nothing."; \
		exit 1; \
	fi; \
	cli=`sed -n 's/^[[:space:]]*#define[[:space:]][[:space:]]*CHUNK_DIM[[:space:]][[:space:]]*\([0-9][0-9]*\).*$$/\1/p' $(CHUNK_H) | head -1`; \
	srv=`sed -n 's/^[[:space:]]*#define[[:space:]][[:space:]]*BS_CHUNK_DIM[[:space:]][[:space:]]*\([0-9][0-9]*\).*$$/\1/p' $(PROTO_H) | head -1`; \
	if [ -z "$$cli" ]; then \
		echo "Makefile: cannot read CHUNK_DIM out of $(CHUNK_H)."; \
		echo "  no '#define CHUNK_DIM <number>' line matched, so that header's shape"; \
		echo "  changed and this guard no longer knows what it is comparing. The build"; \
		echo "  stops rather than compare nothing."; \
		echo "  re-point the sed in check-chunkdim-drift at wherever the value now lives."; \
		exit 1; \
	fi; \
	if [ -z "$$srv" ]; then \
		echo "Makefile: cannot read BS_CHUNK_DIM out of $(PROTO_H)."; \
		echo "  no '#define BS_CHUNK_DIM <number>' line matched. That macro is the wire"; \
		echo "  contract's copy of this client's CHUNK_DIM and is what bs_col_of()"; \
		echo "  shifts every column coordinate by. Without it there is nothing to check"; \
		echo "  against, so the build stops rather than pass having compared nothing."; \
		echo "  re-point the sed in check-chunkdim-drift at wherever the value now lives."; \
		exit 1; \
	fi; \
	if [ "$$cli" != "$$srv" ]; then \
		echo "Makefile: the client and the wire contract disagree about the chunk size."; \
		echo "  $(CHUNK_H): CHUNK_DIM    = $$cli"; \
		echo "  $(PROTO_H): BS_CHUNK_DIM = $$srv"; \
		echo ""; \
		echo "  These are one number written twice. BS_CHUNK_DIM is what bs_col_of()"; \
		echo "  shifts by, so with them apart every CHUNK_SUB/CHUNK_UNSUB/CHUNK_DIFFS"; \
		echo "  column coordinate on the wire names a different column at each end:"; \
		echo "  edits are filed under a column nobody subscribed to and simply never"; \
		echo "  arrive. Nothing else in either build reports this - both sides compile"; \
		echo "  clean, and the symptom is a silent desync at runtime."; \
		echo ""; \
		echo "  Decide which value the game should have, then set BOTH. Note that"; \
		echo "  CHUNK_DIM is not free to move on the client side either: chunk.h's own"; \
		echo "  header comment records that the cave-culling flood fill is defined on"; \
		echo "  cubic chunks, and BS_CHUNK_DIM_SHIFT in $(PROTO_H) is log2 of this"; \
		echo "  value and must move with it (game/validate.c asserts that pair)."; \
		echo "  Changing bs_proto.h also means bumping PROTO_COMMIT above to a pushed"; \
		echo "  server commit, or check-proto-drift stops the next build instead."; \
		exit 1; \
	fi; \
	echo "chunk dim: client CHUNK_DIM = $$cli, server BS_CHUNK_DIM = $$srv (equal, as the wire requires)"

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
all: check-proto-drift check-world-drift check-diffcap-drift check-chunkdim-drift $(BUILD) $(GFXBUILD) $(DEPSDIR) $(ROMFS_T3XFILES) $(T3XHFILES)
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

# v1.9.0 distance fog. The same trap a FOURTH time, with a wrinkle worth writing down: the stem
# collision here is with source/gfx/fogramp.c, whose compiler deps land in build/fogramp.d
# exactly where tex3ds writes the dep naming gfx/fogramp.png. Two files called fogramp, one a
# .png and one a .c, is the same collision the three rules above describe.
#
# Without this line, regenerating the ramp with tools/make_fog_ramp.py would leave the console
# sampling the previous curve while the .png on disk showed the new one. On a fog ramp that
# reads as the RENDER DISTANCE being wrong rather than as a stale texture — a stale ramp is a
# stale fade SHAPE, so half_vis would silently be whatever the old curve gave and no amount of
# reading source/gfx/fogramp.c would explain it. Same one-line fix, stated plainly for the same
# reason.
fogramp.t3x: fogramp.png

# v1.8.16 animal textures. The same trap a FIFTH time. gfx/animals.t3s names gfx/animals.png,
# tex3ds writes its dep file as build/animals.d, and the only reason that survives longer here
# than it does for atlas/font/crackatlas/fogramp is that no source file is called animals.c
# today — which is luck, not a guarantee, and is exactly the state gfx/atlas.png was in before
# something else claimed its stem. The line is added now rather than after the collision.
#
# Without it, re-running tools/make_animals.py would leave the console drawing the previous
# sheet while the .png on disk showed the new one. On four animals sharing one sheet by
# quadrant that reads as the ART being wrong — a pig with last revision's snout — rather than
# as a stale texture, which is a long way round to a one-line fix. animals.png is found through
# VPATH, which already covers $(GRAPHICS).
animals.t3x: animals.png

-include $(DEPSDIR)/*.d

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
