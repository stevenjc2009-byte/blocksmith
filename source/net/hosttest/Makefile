# bsnet_transport hosttest — builds source/net/bsnet_transport.c (the SAME
# file that ships to the 3DS) for Linux, with SOCU swapped for the host branch
# of bsnet_sock.c and a real bsgate forked from server/gateway/ for it to
# handshake against. See bsnet_transport_hosttest.c for what is actually
# exercised.
#
# Not part of the 3DS build: source/net is scanned non-recursively by the
# project Makefile ($(wildcard $(dir)/*.c)), so this hosttest/ subdirectory is
# invisible to it and needs no exclusion.
#
# `make`      builds ./bsgate (from server/gateway) and ./bsnet_transport_hosttest
# `make run`  builds, then runs the end-to-end suite
# `make clean`

CC ?= cc

NET_DIR   := ..
GATEWAY   := ../../../server/gateway
HYDRO     := $(GATEWAY)/.deps/libhydrogen
BUILD     := build

# A fixed path, not a per-run temp dir: bsnet_transport.c resolves BS_NET_DIR
# at compile time (mirrors how the 3DS build resolves the "sdmc:/" prefix), so
# the harness cleans this directory itself at the start of main() instead.
CLIENT_DIR := /tmp/bsnet_hosttest_client

CFLAGS  := -std=c11 -O0 -g -Wall -Wextra \
           -I$(HYDRO) \
           -DBS_NET_DIR=\"$(CLIENT_DIR)\" \
           -DBSNET_TRANSPORT_DEBUG_LOG=1

OBJS := $(BUILD)/bsnet_transport.o $(BUILD)/bsnet_sock.o $(BUILD)/bsnet_replay.o \
        $(BUILD)/hydrogen.o $(BUILD)/bsnet_transport_hosttest.o

# Every rule below carries -MMD -MP, so each object writes a .d beside itself listing the
# headers it actually included. Read back by the -include at the foot of this file.
#
# WHY: without it this Makefile had NO header dependencies at all. Editing bsnet_transport.h,
# bsnet_replay.h or bs_proto.h rebuilt NOTHING and `make` exited 0, so every header-only
# change verified here was verified against the PREVIOUS binary. A skipped rebuild and a
# passing run produce byte-identical output, which is why nothing ever caught it.
#
# That matters most for exactly this suite. It is the one that drives the real transport
# against a real forked bsgate, so it is where a protocol or replay-window header change is
# supposed to be caught -- and it was the one place such a change could not be seen.
#
# -MMD not -MD: libhydrogen and libc are not the drift risk here, and listing them churns
# every .d on a toolchain update. -MP emits a phony target per header, so DELETING a header
# gives a clean recompile instead of make aborting with "No rule to make target".
# Same idiom, same reasoning, as the gateway Makefile's own dep generation.
DEPS_MK := $(OBJS:.o=.d)

.PHONY: all run clean gate-deps

all: $(BUILD) gate-deps bsgate bsnet_transport_hosttest

$(BUILD):
	mkdir -p $(BUILD)

# Builds the real gateway daemon from its own Makefile (fetches libhydrogen at
# the pinned commit if server/gateway/.deps is not already populated). Nothing
# under server/gateway/ is modified by this file — it only invokes the build
# rules already committed there.
gate-deps:
	$(MAKE) -C $(GATEWAY) deps bsgate

bsgate: gate-deps
	cp $(GATEWAY)/bsgate ./bsgate

$(BUILD)/hydrogen.o: $(HYDRO)/hydrogen.c | $(BUILD)
	$(CC) -std=c11 -O2 -g -I$(HYDRO) -MMD -MP -c $< -o $@

$(BUILD)/bsnet_transport.o: $(NET_DIR)/bsnet_transport.c | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/bsnet_sock.o: $(NET_DIR)/bsnet_sock.c | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/bsnet_replay.o: $(NET_DIR)/bsnet_replay.c | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/bsnet_transport_hosttest.o: bsnet_transport_hosttest.c | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

bsnet_transport_hosttest: $(OBJS)
	$(CC) $(OBJS) -o $@

run: all
	./bsnet_transport_hosttest

clean:
	rm -rf $(BUILD) bsnet_transport_hosttest bsgate

# Read back the .d files written by the -MMD -MP above. The leading dash is required, not
# defensive: on a clean tree no .d exists yet and a bare include would be a fatal error --
# the first build compiles everything regardless, and it is that build which writes them.
#
# Worth knowing before testing this: a tree built BEFORE this line existed has no .d files
# at all, so the first make after adopting it still cannot see header deps for objects it
# already considers up to date. Run make clean once, or the fix looks like it did not work.
# `clean` removes $(BUILD) wholesale, so the .d files go with the objects that wrote them.
-include $(DEPS_MK)
