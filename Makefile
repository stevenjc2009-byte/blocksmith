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
INCLUDES	:=	source deps/libhydrogen
GRAPHICS	:=	gfx
GFXBUILD	:=	$(BUILD)

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

CXXFLAGS	:= $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS	:= -lcitro3d -lctru -lm

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
LIBDIRS	:= $(CTRULIB)

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

.PHONY: deps
deps: $(HYDRO)/hydrogen.h

$(HYDRO)/hydrogen.h:
	@mkdir -p deps
	git clone -q $(HYDRO_REPO) $(HYDRO)
	git -C $(HYDRO) -c safe.directory='*' checkout -q $(HYDRO_COMMIT)
	@test "$$(git -C $(HYDRO) -c safe.directory='*' rev-parse HEAD)" = "$(HYDRO_COMMIT)" \
	  || (echo "libhydrogen checkout is not at the pinned commit"; exit 1)
	@echo "libhydrogen pinned at $(HYDRO_COMMIT)"

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
all: $(BUILD) $(GFXBUILD) $(DEPSDIR) $(ROMFS_T3XFILES) $(T3XHFILES)
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

-include $(DEPSDIR)/*.d

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
