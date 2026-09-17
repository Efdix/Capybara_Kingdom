# SPDX-License-Identifier: CC0-1.0
#
# SPDX-FileContributor: Antonio Niño Díaz, 2023-2026

export BLOCKSDS			?= /opt/blocksds/core
export BLOCKSDSEXT		?= /opt/blocksds/external

export WONDERFUL_TOOLCHAIN	?= /opt/wonderful
ARM_NONE_EABI_PATH	?= $(WONDERFUL_TOOLCHAIN)/toolchain/gcc-arm-none-eabi/bin/

# Default goal
# ------------
# Declared explicitly because make otherwise uses the first target in the file,
# which would silently make an internal helper the default for plain `make`.
.DEFAULT_GOAL := all

# User config
# ===========

NAME		:= capybara_kingdom

GAME_TITLE	:= Capybara Kingdom
GAME_SUBTITLE	:= Happy Birthday
GAME_AUTHOR	:= Capy Heroes
GAME_ICON	:= icon.gif

# A compile_commands.json file is created if this is set to 1
COMPDB		:= 0

# DLDI and internal SD slot of DSi
# --------------------------------

# Root folder of the SD image
SDROOT		:= sdroot
# Name of the generated image it "DSi-1.sd" for no$gba in DSi mode
SDIMAGE		:= image.bin

# Source code paths
# -----------------

SOURCEDIRS	:= source
INCLUDEDIRS	:=
GFXDIRS		:=
BINDIRS		:= graphics data
AUDIODIRS	:= audio
# List of folders to combine into the root of NitroFS:
NITROFSDIR	:=

# Asset generation
# ----------------
# All graphics, the font atlas and the audio bank are produced by Python
# scripts in tools/. They are re-run automatically when the scripts change,
# which keeps the checked-in .bin files and the generators from drifting.
# Uses the dedicated conda environment so the host Python is never touched.
PYTHON		:= /d/Science/Miniforge/envs/Blocksds/python.exe
TOOLS		:= tools/gen_common.py tools/gen_heroes.py tools/gen_world.py \
		   tools/gen_cards.py tools/gen_font.py tools/gen_audio.py

# Integrity checks for the generated assets: file sizes against the geometry
# the C code assumes, palette indices in range, collision maps that match their
# area, and audio that is neither silent nor clipped. They run before the asset
# conversion step so a broken asset fails the build rather than shipping a game
# with blank text or missing sound.
# (the verify-assets rule lives below, where BUILDDIR is set)

# Defines passed to all files
# ---------------------------

DEFINES		:= -DCAPYBARA_KINGDOM

# Libraries
# ---------

# dswifi ARM7 core is required for local multiplayer + Maxmod for sound
ARM7ELF		:= $(BLOCKSDS)/sys/arm7/main_core/arm7_dswifi_maxmod.elf

LIBS		:= -ldswifi9 -lmm9 -lnds9
LIBDIRS		:= $(BLOCKSDS)/libs/dswifi \
		   $(BLOCKSDS)/libs/maxmod \
		   $(BLOCKSDS)/libs/libnds

# Build artifacts
# ---------------

BUILDDIR	:= build/$(NAME)
ELF		:= build/$(NAME).elf
DUMP		:= build/$(NAME).dump
MAP		:= build/$(NAME).map
ROM		:= $(NAME).nds

# Asset verification
# ------------------
# Runs the integrity checks in tools/verify_assets.py once per build, before the
# generated .bin files are converted. It must be defined here rather than with
# the generator settings above, because it depends on BUILDDIR.
#
# The sources are a dependency too, not just the generators: the font check
# reads every .c/.h under source/ and fails if any Chinese character used by the
# game is missing from the atlas. Keying only off the generators meant new text
# could ship as empty boxes, because nothing re-ran the check that catches it.
.PHONY: verify-assets
verify-assets: $(BUILDDIR)/.assets-verified

SOURCES_FOR_VERIFY := $(wildcard source/*.c source/*.h)

$(BUILDDIR)/.assets-verified: $(TOOLS) $(SOURCES_FOR_VERIFY)
	@echo "  VERIFY  assets"
	@$(MKDIR) -p $(BUILDDIR)
	@$(PYTHON) tools/verify_assets.py
	@touch $@

# If NITROFSDIR is set, the soundbank created by mmutil will be saved to NitroFS
SOUNDBANKINFODIR	:= $(BUILDDIR)/maxmod
ifeq ($(strip $(NITROFSDIR)),)
    SOUNDBANKDIR	:= $(BUILDDIR)/maxmod
else
    SOUNDBANKDIR	:= $(BUILDDIR)/maxmod_nitrofs
endif

# Tools
# -----

PREFIX		:= $(ARM_NONE_EABI_PATH)arm-none-eabi-
CC		:= $(PREFIX)gcc
CXX		:= $(PREFIX)g++
LD		:= $(PREFIX)gcc
OBJDUMP		:= $(PREFIX)objdump
MKDIR		:= mkdir
RM		:= rm -rf

# Verbose flag
# ------------

ifeq ($(VERBOSE),1)
V		:=
else
V		:= @
endif

# Source files
# ------------

ifneq ($(BINDIRS),)
    SOURCES_BIN	:= $(shell find -L $(BINDIRS) -name "*.bin")
    INCLUDEDIRS	+= $(addprefix $(BUILDDIR)/,$(BINDIRS))
endif
ifneq ($(GFXDIRS),)
    SOURCES_PNG	:= $(shell find -L $(GFXDIRS) -name "*.png")
    INCLUDEDIRS	+= $(addprefix $(BUILDDIR)/,$(GFXDIRS))
endif
ifneq ($(AUDIODIRS),)
    SOURCES_AUDIO	:= $(shell find -L $(AUDIODIRS) -regex '.*\.\(it\|mod\|s3m\|wav\|xm\)')
    ifneq ($(SOURCES_AUDIO),)
        INCLUDEDIRS	+= $(SOUNDBANKINFODIR)
    endif
endif

SOURCES_S	+= $(shell find -L $(SOURCEDIRS) -name "*.s")
SOURCES_C	+= $(shell find -L $(SOURCEDIRS) -name "*.c")
SOURCES_CPP	+= $(shell find -L $(SOURCEDIRS) -name "*.cpp")

# Compiler and linker flags
# -------------------------

DEFINES		+= -D__NDS__ -D__BLOCKSDS__ -DARM9

# Appended last: the self-driving development ROM targets at the bottom of this
# file set this on the command line to compile the scripted-input harness in.
# It stays empty for the release build.
DEFINES		+= $(DEVROM_DEFINE)

ARCH		:= -mthumb -mcpu=arm946e-s+nofp

SPECS		:= $(BLOCKSDS)/sys/crts/ds_arm9.specs

WARNFLAGS	:= -Wall

ifeq ($(SOURCES_CPP),)
	LIBS	+= -lc
else
	LIBS	+= -lstdc++ -lc
endif

INCLUDEFLAGS	:= $(foreach path,$(INCLUDEDIRS),-I$(path)) \
		   $(foreach path,$(LIBDIRS),-I$(path)/include)

LIBDIRSFLAGS	:= $(foreach path,$(LIBDIRS),-L$(path)/lib)

ASFLAGS		+= -x assembler-with-cpp $(INCLUDEFLAGS) $(DEFINES) \
		   $(ARCH) -ffunction-sections -fdata-sections \
		   -specs=$(SPECS)

CFLAGS		+= $(WARNFLAGS) $(INCLUDEFLAGS) $(DEFINES) \
		   $(ARCH) -O2 -ffunction-sections -fdata-sections \
		   -specs=$(SPECS)

CXXFLAGS	+= $(WARNFLAGS) $(INCLUDEFLAGS) $(DEFINES) \
		   $(ARCH) -O2 -ffunction-sections -fdata-sections \
		   -fno-exceptions -fno-rtti \
		   -specs=$(SPECS)

LDFLAGS		+= $(ARCH) $(LIBDIRSFLAGS) -Wl,-Map,$(MAP) $(DEFINES) \
		   -Wl,--start-group $(LIBS) -Wl,--end-group -specs=$(SPECS)

# Intermediate build files
# ------------------------

OBJS_ASSETS	:= $(addsuffix .o,$(addprefix $(BUILDDIR)/,$(SOURCES_BIN))) \
		   $(addsuffix .o,$(addprefix $(BUILDDIR)/,$(SOURCES_PNG)))

HEADERS_ASSETS	:= $(patsubst %.bin,%_bin.h,$(addprefix $(BUILDDIR)/,$(SOURCES_BIN))) \
		   $(patsubst %.png,%.h,$(addprefix $(BUILDDIR)/,$(SOURCES_PNG)))

ifneq ($(SOURCES_AUDIO),)
    ifeq ($(strip $(NITROFSDIR)),)
        OBJS_ASSETS		+= $(SOUNDBANKDIR)/soundbank.c.o
    endif
    HEADERS_ASSETS	+= $(SOUNDBANKINFODIR)/soundbank.h
endif

OBJS_SOURCES	:= $(addsuffix .o,$(addprefix $(BUILDDIR)/,$(SOURCES_S))) \
		   $(addsuffix .o,$(addprefix $(BUILDDIR)/,$(SOURCES_C))) \
		   $(addsuffix .o,$(addprefix $(BUILDDIR)/,$(SOURCES_CPP)))

OBJS		:= $(OBJS_ASSETS) $(OBJS_SOURCES)

DEPS		:= $(OBJS:.o=.d)

# Targets
# -------

.PHONY: all clean dump dldipatch sdimage

all: $(ROM)

ifneq ($(strip $(NITROFSDIR)),)
# Additional arguments for ndstool
NDSTOOL_ARGS	:= -d $(NITROFSDIR)

ifneq ($(SOURCES_AUDIO),)
    NDSTOOL_ARGS	+= -d $(SOUNDBANKDIR)
endif

# Make the NDS ROM depend on the filesystem only if it is needed
$(ROM): $(NITROFSDIR)
endif

# Combine the title strings
ifeq ($(strip $(GAME_SUBTITLE)),)
    GAME_FULL_TITLE := $(GAME_TITLE);$(GAME_AUTHOR)
else
    GAME_FULL_TITLE := $(GAME_TITLE);$(GAME_SUBTITLE);$(GAME_AUTHOR)
endif

$(ROM): $(ELF)
	@echo "  NDSTOOL $@"
	$(V)$(BLOCKSDS)/tools/ndstool/ndstool -c $@ \
		-7 $(ARM7ELF) -9 $(ELF) \
		-b $(GAME_ICON) "$(GAME_FULL_TITLE)" \
		$(NDSTOOL_ARGS)

$(ELF): $(OBJS)
	@echo "  LD      $@"
	$(V)$(LD) -o $@ $(OBJS) $(LDFLAGS)

$(DUMP): $(ELF)
	@echo "  OBJDUMP   $@"
	$(V)$(OBJDUMP) -h -C -S $< > $@

dump: $(DUMP)

clean:
	@echo "  CLEAN"
	$(V)$(RM) $(ROM) $(DUMP) build $(SDIMAGE) compile_commands.json
	$(V)$(RM) $(addsuffix .nds,$(DEVROMS))

sdimage:
	@echo "  MKFATIMG $(SDIMAGE) $(SDROOT)"
	$(V)$(BLOCKSDS)/tools/mkfatimg/mkfatimg -t $(SDROOT) $(SDIMAGE)

dldipatch: $(ROM)
	@echo "  DLDIPATCH $(ROM)"
	$(V)$(BLOCKSDS)/tools/dldipatch/dldipatch patch \
		$(BLOCKSDS)/sys/dldi_r4/r4tf.dldi $(ROM)

# Self-driving development ROMs
# -----------------------------
# Each of these builds the whole game again with the scripted-input harness
# compiled in (the harness lives in source/input.c, behind the AUTOPLAY
# defines) so the emulator can play unattended and screenshot screens a human
# would otherwise have to reach by hand:
#
#     ./build.sh --autoplay   auto-play a battle
#     ./build.sh --menu       auto level-up, exercise the card picker
#     ./build.sh --tap        same, but picks the card by touch
#     ./build.sh --boss       auto-play until the boss appears
#     ./build.sh --ending     jump straight to the ending
#
# They re-enter make rather than sharing the rules above because NAME drives
# BUILDDIR, ELF, DUMP and ROM, and all of those are resolved while this file is
# being parsed. Giving each variant its own NAME keeps its objects and ROM
# completely separate from the release build.

DEVROMS			:= autoplay menu tap boss ending

DEVROM_DEFINE_autoplay	:= -DAUTOPLAY
DEVROM_DEFINE_menu	:= -DAUTOPLAY -DAUTOPLAY_MENU
DEVROM_DEFINE_tap	:= -DAUTOPLAY -DAUTOPLAY_TAP_CARD
DEVROM_DEFINE_boss	:= -DAUTOPLAY -DAUTOPLAY_BOSS
DEVROM_DEFINE_ending	:= -DAUTOPLAY -DAUTOPLAY_ENDING

.PHONY: $(DEVROMS)
$(DEVROMS):
	@$(MAKE) --no-print-directory \
		NAME=$@ \
		DEVROM_DEFINE='$(DEVROM_DEFINE_$@)' \
		all

ifeq ($(COMPDB),1)
# Add an additional dependency to the "all" rule
all: compile_commands.json

compile_commands.json: $(OBJS)
	@echo "  MERGE   compile_commands.json"
	$(V)$(WONDERFUL_TOOLCHAIN)/bin/wf-compile-commands-merge $@ $(patsubst %.o,%.cc.json,$^)
endif

# Rules
# -----

ifeq ($(COMPDB),1)

$(BUILDDIR)/%.s.o : %.s
	@echo "  AS      $<"
	@$(MKDIR) -p $(@D)
	$(V)$(CC) $(ASFLAGS) -MMD -MP -c -MJ $(patsubst %.o,%.cc.json,$@) -o $@ $<

$(BUILDDIR)/%.c.o : %.c
	@echo "  CC      $<"
	@$(MKDIR) -p $(@D)
	$(V)$(CC) $(CFLAGS) -MMD -MP -c -MJ $(patsubst %.o,%.cc.json,$@) -o $@ $<

$(BUILDDIR)/%.arm.c.o : %.arm.c
	@echo "  CC      $<"
	@$(MKDIR) -p $(@D)
	$(V)$(CC) $(CFLAGS) -MMD -MP -marm -mlong-calls -c -MJ $(patsubst %.o,%.cc.json,$@) -o $@ $<

$(BUILDDIR)/%.cpp.o : %.cpp
	@echo "  CXX     $<"
	@$(MKDIR) -p $(@D)
	$(V)$(CXX) $(CXXFLAGS) -MMD -MP -c -MJ $(patsubst %.o,%.cc.json,$@) -o $@ $<

$(BUILDDIR)/%.arm.cpp.o : %.arm.cpp
	@echo "  CXX     $<"
	@$(MKDIR) -p $(@D)
	$(V)$(CXX) $(CXXFLAGS) -MMD -MP -marm -mlong-calls -c -MJ $(patsubst %.o,%.cc.json,$@) -o $@ $<

else

$(BUILDDIR)/%.s.o : %.s
	@echo "  AS      $<"
	@$(MKDIR) -p $(@D)
	$(V)$(CC) $(ASFLAGS) -MMD -MP -c -o $@ $<

$(BUILDDIR)/%.c.o : %.c
	@echo "  CC      $<"
	@$(MKDIR) -p $(@D)
	$(V)$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILDDIR)/%.arm.c.o : %.arm.c
	@echo "  CC      $<"
	@$(MKDIR) -p $(@D)
	$(V)$(CC) $(CFLAGS) -MMD -MP -marm -mlong-calls -c -o $@ $<

$(BUILDDIR)/%.cpp.o : %.cpp
	@echo "  CXX     $<"
	@$(MKDIR) -p $(@D)
	$(V)$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

$(BUILDDIR)/%.arm.cpp.o : %.arm.cpp
	@echo "  CXX     $<"
	@$(MKDIR) -p $(@D)
	$(V)$(CXX) $(CXXFLAGS) -MMD -MP -marm -mlong-calls -c -o $@ $<

endif

$(BUILDDIR)/%.bin.o $(BUILDDIR)/%_bin.h : %.bin $(BUILDDIR)/.assets-verified
	@echo "  BIN2C   $<"
	@$(MKDIR) -p $(@D)
	$(V)$(BLOCKSDS)/tools/bin2c/bin2c $< $(@D)
	$(V)$(CC) $(CFLAGS) -MMD -MP -c -o $(BUILDDIR)/$*.bin.o $(BUILDDIR)/$*_bin.c

$(BUILDDIR)/%.png.o $(BUILDDIR)/%.h : %.png %.grit
	@echo "  GRIT    $<"
	@$(MKDIR) -p $(@D)
	$(V)$(BLOCKSDS)/tools/grit/grit $< -ftc -W1 -o$(BUILDDIR)/$*
	$(V)$(CC) $(CFLAGS) -MMD -MP -c -o $(BUILDDIR)/$*.png.o $(BUILDDIR)/$*.c
	$(V)touch $(BUILDDIR)/$*.png.o $(BUILDDIR)/$*.h

ifneq ($(SOURCES_AUDIO),)

$(SOUNDBANKINFODIR)/soundbank.h: $(SOURCES_AUDIO)
	@echo "  MMUTIL  $^"
	@$(MKDIR) -p $(SOUNDBANKDIR)
	@$(MKDIR) -p $(SOUNDBANKINFODIR)
	$(V)$(BLOCKSDS)/tools/mmutil/mmutil $^ -d \
		-o$(SOUNDBANKDIR)/soundbank.bin -h$(SOUNDBANKINFODIR)/soundbank.h

ifeq ($(strip $(NITROFSDIR)),)
$(SOUNDBANKDIR)/soundbank.c.o: $(SOUNDBANKINFODIR)/soundbank.h
	@echo "  BIN2C   soundbank.bin"
	$(V)$(BLOCKSDS)/tools/bin2c/bin2c $(SOUNDBANKDIR)/soundbank.bin \
		$(SOUNDBANKDIR)
	@echo "  CC.9    soundbank_bin.c"
	$(V)$(CC) $(CFLAGS) -MMD -MP -c -o $(SOUNDBANKDIR)/soundbank.c.o \
		$(SOUNDBANKDIR)/soundbank_bin.c
endif

endif

# All assets must be built before the source code
# -----------------------------------------------

$(SOURCES_S) $(SOURCES_C) $(SOURCES_CPP): $(HEADERS_ASSETS)

# Include dependency files if they exist
# --------------------------------------

-include $(DEPS)
