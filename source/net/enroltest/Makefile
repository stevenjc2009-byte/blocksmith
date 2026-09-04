# Makefile.enrol — host build for the invite-code enrolment tests.
#
# Builds the SAME source/net/*.c files that ship to the 3DS (SOCU swapped for
# the host branch of bsnet_sock.c) and runs them against two servers:
#
#   Part A  a stub gateway written in this harness, which speaks the server
#           side of the Noise XX handshake with libhydrogen directly. It exists
#           so the wire can be inspected byte by byte — which type went out,
#           what the plaintext decrypted to, how long the client waited — and
#           so a scenario the real gateway will not perform on demand (say
#           nothing at all for twelve seconds) can be staged exactly.
#
#   Part B  a real bsgate, forked from the pinned server checkout in
#           deps/blocksmith-server, with a real invite armed through its own
#           --arm-invite CLI. A stub that is written to agree with the client
#           proves only that the harness agrees with the client; this is the
#           arm that can actually fail.
#
# Neither tree is written to. The gateway sources are compiled out of
# deps/blocksmith-server into this directory's own build dir, so the vendored
# checkout stays exactly as `make deps` left it.
#
# Not part of the 3DS build: the project Makefile scans source/net
# non-recursively, so this subdirectory is invisible to it.
#
# `make run`    build everything and run the suite
# `make clean`

CC ?= cc

SRC     := ../..
NET_DIR := ..
PROTO   := ../../../deps/blocksmith-server
GATEWAY := $(PROTO)/gateway

# libhydrogen at the commit both sides are pinned to. The copy under deps/ in
# this repo has the 3DS random backend patched into it (see the project
# Makefile's patches/libhydrogen step), so it is not the one to build a Linux
# binary from; server/gateway/.deps holds the unpatched checkout at the same
# commit. Read only — nothing here writes into it.
HYDRO   := ../../../server/gateway/.deps/libhydrogen

BUILD   := build-enrol

# Fixed, not per-run: bsnet_transport.c resolves BS_NET_DIR at compile time,
# the same way the 3DS build resolves the "sdmc:/" prefix, so the harness
# clears these itself at the start of main().
WORKDIR    := /tmp/bsnet_enroltest
CLIENT_DIR := $(WORKDIR)/client

# Deliberately WRONG baked-in keys. Every handshake in the enrolment suite runs
# against keys written to the SD directory, so those 37 checks only pass if the
# SD files beat these — which makes the whole suite the override test. Swap the
# precedence in netTransportInit() and it goes red.
FAKE_PK  := beefbeefbeefbeefbeefbeefbeefbeefbeefbeefbeefbeefbeefbeefbeefbeef
FAKE_PSK := feedfeedfeedfeedfeedfeedfeedfeedfeedfeedfeedfeedfeedfeedfeedfeed

CFLAGS := -std=c11 -O0 -g -Wall -Wextra \
          -I$(SRC) -I$(PROTO) -I$(HYDRO) \
          -DBS_NET_DIR=\"$(CLIENT_DIR)\" \
          -DENROL_WORKDIR=\"$(WORKDIR)\" \
          -DADDR_WORKDIR=\"/tmp/bsnet_addrtest\" \
          -DBSGATE_PATH=\"$(CURDIR)/$(BUILD)/bsgate\" \
          -DBS_SERVER_PK_HEX=\"$(FAKE_PK)\" \
          -DBS_NETWORK_PSK_HEX=\"$(FAKE_PSK)\"


# The gateway is third-party-to-this-repo source built for a purpose its own
# Makefile does not cover; its warning set is that Makefile's business, not
# this one's.
GATE_CFLAGS := -std=c11 -O1 -g -I$(PROTO) -I$(HYDRO)

SHARED_OBJS := $(BUILD)/bsnet_transport.o $(BUILD)/bsnet_sock.o \
               $(BUILD)/bsnet_replay.o $(BUILD)/bsnet.o \
               $(BUILD)/networld.o $(BUILD)/blockdiff.o \
               $(BUILD)/world.o $(BUILD)/chunk.o $(BUILD)/budget.o \
               $(BUILD)/hydrogen.o

CLIENT_OBJS := $(SHARED_OBJS) $(BUILD)/enrol_hosttest.o

# Second binary over the same objects: how bsnet.c finds its server address.
# Separate from the enrolment suite because it chdir()s into a scratch tree and
# calls netInit()/netExit() five times, which is not a state the enrolment
# tests should have to tolerate.
# Two flavours, because "what does an unconfigured build do" and "what does a
# shipping build do" are different questions and BS_SERVER_ADDR is resolved at
# compile time. NOADDR swaps in a bsnet.o built with an empty address; BAKED
# uses bsnet.c's own default, i.e. exactly what a released CIA contains.
NOADDR_OBJS := $(filter-out $(BUILD)/bsnet.o,$(SHARED_OBJS)) \
               $(BUILD)/bsnet_noaddr.o $(BUILD)/addr_hosttest.o

BAKED_OBJS  := $(SHARED_OBJS) $(BUILD)/baked_hosttest.o

GATE_OBJS := $(BUILD)/gate/bsgate.o $(BUILD)/gate/allowlist.o \
             $(BUILD)/gate/invite.o $(BUILD)/gate/proxyproto.o \
             $(BUILD)/gate/ratelimit.o $(BUILD)/gate/replay.o \
             $(BUILD)/gate/hydrogen.o

# Every compile rule above carries -MMD -MP, so each object writes a .d beside itself
# listing the headers it actually included. Read back by the -include at the foot.
#
# WHY: this Makefile had NO header dependencies. Editing bsnet.h, networld.h, world.h or
# any gateway header rebuilt NOTHING and `make` exited 0, so every header-only change was
# verified against the PREVIOUS binary. A skipped rebuild and a passing run produce
# byte-identical output, which is why nothing ever caught it.
#
# The gate objects are listed too, not only the client ones. This suite forks a REAL bsgate
# and enrols against it, so a change to allowlist.h, invite.h or replay.h that did not get
# compiled in would leave the suite testing the old daemon while reporting on the new one.
#
# -MMD not -MD, and -MP, for the same two reasons as the gateway Makefile: libhydrogen and
# libc are not the drift risk and listing them churns every .d on a toolchain update, and a
# phony target per header makes DELETING a header a clean recompile rather than make
# aborting with "No rule to make target".
DEPS_MK := $(SHARED_OBJS:.o=.d) $(GATE_OBJS:.o=.d)
DEPS_MK += $(BUILD)/enrol_hosttest.d $(BUILD)/addr_hosttest.d
DEPS_MK += $(BUILD)/baked_hosttest.d $(BUILD)/bsnet_noaddr.d

.PHONY: all run run-addr clean

all: $(BUILD)/bsgate $(BUILD)/enrol_hosttest $(BUILD)/addr_hosttest $(BUILD)/baked_hosttest

$(BUILD) $(BUILD)/gate:
	mkdir -p $@

# ------------------------------------------------------------ the gateway ---

$(BUILD)/gate/hydrogen.o: $(HYDRO)/hydrogen.c | $(BUILD)/gate
	$(CC) $(GATE_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/gate/%.o: $(GATEWAY)/%.c | $(BUILD)/gate
	$(CC) $(GATE_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/bsgate: $(GATE_OBJS)
	$(CC) $(GATE_OBJS) -o $@

# ------------------------------------------------------------- the client ---

$(BUILD)/hydrogen.o: $(HYDRO)/hydrogen.c | $(BUILD)
	$(CC) -std=c11 -O1 -g -I$(HYDRO) -MMD -MP -c $< -o $@

$(BUILD)/%.o: $(NET_DIR)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.o: $(SRC)/world/%.c | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/enrol_hosttest.o: enrol_hosttest.c | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/enrol_hosttest: $(CLIENT_OBJS)
	$(CC) $(CLIENT_OBJS) -o $@

$(BUILD)/bsnet_noaddr.o: $(NET_DIR)/bsnet.c | $(BUILD)
	$(CC) $(CFLAGS) -DBS_SERVER_ADDR=\"\" -MMD -MP -c $< -o $@

$(BUILD)/addr_hosttest.o: addr_hosttest.c | $(BUILD)
	$(CC) $(CFLAGS) -DBS_SERVER_ADDR=\"\" -MMD -MP -c $< -o $@

$(BUILD)/baked_hosttest.o: addr_hosttest.c | $(BUILD)
	$(CC) $(CFLAGS) -DBAKED_ADDR_TEST=1 -MMD -MP -c $< -o $@

$(BUILD)/addr_hosttest: $(NOADDR_OBJS)
	$(CC) $(NOADDR_OBJS) -o $@

$(BUILD)/baked_hosttest: $(BAKED_OBJS)
	$(CC) $(BAKED_OBJS) -o $@

# Absolute paths: the address tests chdir() into their own scratch tree, so a
# relative path would stop resolving after the first case.
run: all
	./$(BUILD)/enrol_hosttest
	$(CURDIR)/$(BUILD)/addr_hosttest
	$(CURDIR)/$(BUILD)/baked_hosttest

run-addr: $(BUILD)/addr_hosttest $(BUILD)/baked_hosttest
	$(CURDIR)/$(BUILD)/addr_hosttest
	$(CURDIR)/$(BUILD)/baked_hosttest

clean:
	rm -rf $(BUILD)

# Read back the .d files written by the -MMD -MP above. The leading dash is required, not
# defensive: on a clean tree no .d exists yet and a bare include would be a fatal error --
# the first build compiles everything regardless, and it is that build which writes them.
#
# Worth knowing before testing this: a tree built BEFORE this line existed has no .d files,
# so the first make after adopting it still cannot see header deps for objects it already
# considers up to date. Run make clean once, or the fix looks like it did not work.
# `clean` removes $(BUILD) wholesale, so the .d files go with the objects that wrote them.
-include $(DEPS_MK)
