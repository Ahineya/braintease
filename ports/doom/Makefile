# Doom uses the repository's existing compiler, assembler and VM hosts.
# Adapted engine sources are maintained in engine/; no fetch or patch step.
.DEFAULT_GOAL := all
.DELETE_ON_ERROR:
SHELL := /bin/bash

ROOT := $(abspath ../..)
BUILD ?= build
override BUILD := $(abspath $(BUILD))
GENERATED := $(BUILD)/generated
PROGRAM ?= engine
TARGET ?= ripple
START_MAP ?= 0
LOW_DETAIL ?= 0
WAD ?= $(CURDIR)/assets/doom1.wad
WAD_MODE ?= cooked
ASSET_VALIDATION ?= preflight
ifeq ($(filter $(ASSET_VALIDATION),preflight guest),)
$(error ASSET_VALIDATION must be preflight or guest)
endif
BANK_SIZE := 64000
MEMORY_BANKS := 256
RCC := $(ROOT)/target/release/rcc
RASM := $(ROOT)/src/ripple-asm/target/release/rasm
RLINK := $(ROOT)/src/ripple-asm/target/release/rlink
ifeq ($(origin CC),default)
CC := clang
endif

# The generator writes these lists alongside bounded C initialization functions.
# Remaking an included makefile makes a clean checkout work on the first build.
ifeq ($(filter clean help,$(MAKECMDGOALS)),)
include $(GENERATED)/sources.mk
endif
$(GENERATED)/sources.mk: engine/tables.c engine/info.c tools/generate-data.mjs
	@node tools/generate-data.mjs engine "$(GENERATED)"

ENGINE_NAMES := dummy am_map doomdef doomstat dstrings d_event d_items d_iwad d_loop d_main d_mode d_net \
 f_finale g_game hu_lib hu_stuff info i_cdmus i_endoom i_joystick i_sound memio \
 m_argv m_bbox m_cheat m_controls m_fixed m_menu m_misc m_random \
 p_ceilng p_doors p_enemy p_floor p_inter p_lights p_map p_maputl p_mobj p_plats p_pspr p_saveg \
 p_setup p_sight p_spec p_switch p_telept p_tick p_user \
 r_bsp r_data r_draw r_main r_plane r_segs r_sky r_things \
 sounds statdump st_lib st_stuff s_sound tables v_video wi_stuff w_file w_main w_wad z_zone
SOURCES_engine = $(addprefix engine/,$(addsuffix .c,$(ENGINE_NAMES))) $(TABLE_SOURCES) $(INFO_SOURCES) \
 src/main.c src/platform.c src/config_port.c src/wad_io.c src/w_file_port.c src/patch_cache.c src/alloc_fast.c src/asset_metadata.c \
 src/render_fast.asm src/raster_fast.asm src/memory_fast.asm
include tests/programs.mk

SOURCES := $(SOURCES_$(PROGRAM))
RIPPLE_SOURCES := $(SOURCES) $(if $(filter engine/m_fixed.c,$(SOURCES)),src/fixed_fast.asm) \
 $(if $(filter src/wad_io.c,$(SOURCES)),src/word_read_fast.asm) \
 $(if $(filter src/asset_metadata.c,$(SOURCES)),src/metadata_validate_fast.asm) \
 $(if $(filter engine libc wad_lookup wad_directory texture_metadata,$(PROGRAM)),src/compat_libc.c)
NATIVE_SOURCES := $(filter %.c,$(SOURCES))
ROUT := $(BUILD)/ripple/$(PROGRAM)
NOUT := $(BUILD)/native/$(PROGRAM)
RIPPLE_BINARY := $(ROUT)/$(PROGRAM).bin
BF_IMAGE := $(ROUT)/$(PROGRAM).bf
NATIVE_BINARY := $(NOUT)/$(PROGRAM)
# The generated header is shared by programs. Its mtime cannot prove that an
# individual program was compiled for the current WAD/mode, especially after
# switching another program back to a previously used configuration. Compare
# content per program and record it only after a successful link. Each export
# needs its own stamp: building BF must not bless an older binary, or vice versa.
ifeq ($(filter clean help,$(MAKECMDGOALS)),)
ASSET_CONFIG_KEY := $(shell node tools/build-support.mjs config-key "$(WAD)" "$(WAD_MODE)" "$(BANK_SIZE)" "$(GENERATED)" "$(ASSET_VALIDATION)")
ifeq ($(ASSET_CONFIG_KEY),)
$(error Unable to determine WAD build configuration)
endif
RIPPLE_CONFIG_FORCE := $(if $(filter $(ASSET_CONFIG_KEY),$(shell cat "$(ROUT)/.asset-config" 2>/dev/null)),,FORCE)
RIPPLE_CONFIG_GATE := $(if $(RIPPLE_CONFIG_FORCE),invalidate-ripple-config)
BINARY_CONFIG_FORCE := $(if $(filter $(ASSET_CONFIG_KEY),$(shell cat "$(RIPPLE_BINARY).asset-config" 2>/dev/null)),,FORCE)
BF_CONFIG_FORCE := $(if $(filter $(ASSET_CONFIG_KEY),$(shell cat "$(BF_IMAGE).asset-config" 2>/dev/null)),,FORCE)
NATIVE_CONFIG_FORCE := $(if $(filter $(ASSET_CONFIG_KEY),$(shell cat "$(NOUT)/.asset-config" 2>/dev/null)),,FORCE)
endif
source_stem = $(if $(filter tests/%,$(1)),test_)$(basename $(notdir $(1)))
OBJECTS := $(foreach source,$(RIPPLE_SOURCES),$(ROUT)/$(call source_stem,$(source)).pobj)
INCLUDES := -Isrc -I$(GENERATED) -Iengine
DEFINES := -DDOOM_GENERATED_TABLES=1 -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 -DDOOM_PORT=1 \
 $(if $(filter engine info_storage,$(PROGRAM)),-DDOOM_GENERATED_INFO=1)
SCENE_DEFINES := $(if $(filter 1,$(START_MAP)),-DDOOM_START_MAP=1) $(if $(filter 1,$(LOW_DETAIL)),-DDOOM_LOW_DETAIL=1)
RIPPLE_FLAGS := -Iinclude $(INCLUDES) -I$(ROOT)/runtime/include $(DEFINES) -DRIPPLE=1 -DBANK_SIZE=$(BANK_SIZE) \
 -DDOOM_ASSET_STRICT=$(if $(filter guest,$(ASSET_VALIDATION)),1,0) \
 $(if $(filter engine memory_kernels alloc_fast texture_metadata,$(PROGRAM)),-DDOOM_FAST_MEMORY=1)
NATIVE_SDK_FLAGS = $(if $(findstring sysroot,$(CFLAGS)),,$(shell node tools/build-support.mjs native-flags "$(CC)"))
comma := ,
# Evaluate after comma is defined, including on older GNU make shipped by macOS.
NATIVE_GC_FLAGS = $(if $(filter $(PROGRAM),$(NATIVE_FIXTURES)),-ffunction-sections -fdata-sections $(if $(filter Darwin,$(shell uname -s)),-Wl$(comma)-dead_strip,-Wl$(comma)--gc-sections))
NATIVE_FLAGS = -std=c99 -O2 -g -Wall -Wextra -Wno-unused-parameter -fwrapv $(NATIVE_SDK_FLAGS) $(CFLAGS) $(CPPFLAGS) $(INCLUDES) $(DEFINES) $(SCENE_DEFINES) $(NATIVE_GC_FLAGS)
NATIVE_COMPILER := $(shell command -v "$(CC)")
HEADERS := $(wildcard include/*.h include/*/*.h src/*.h engine/*.h $(ROOT)/runtime/include/*.h $(ROOT)/runtime/include/*/*.h) \
 $(GENERATED)/doom_build_config.h $(GENERATED)/info_actions.h $(GENERATED)/info_test_actions.h
FIXTURE_STAMP := $(if $(GENERATE_$(PROGRAM)),$(GENERATED)/$(PROGRAM).fixture-stamp)
FIXTURE_FILES_projection_widths := projection_loop.inc
FIXTURE_FILES_texture_columns := texture_cache.inc
FIXTURE_FILES_texture_metadata := texture_metadata.inc
FIXTURE_FILES_keyboard_controls := input_commands.inc controls_defaults.inc platform_keyboard.inc
FIXTURE_FILES_config_bindings := config_bindings.inc
FIXTURE_OUTPUTS := $(addprefix $(GENERATED)/,$(FIXTURE_FILES_$(PROGRAM)))

.PHONY: all ripple bf native both prepare runtime run clean help test test-fixtures test-kernels test-gameplay test-assets FORCE _ripple _bf _native invalidate-ripple-config
all: $(TARGET)
both: native ripple
prepare: $(GENERATED)/sources.mk $(GENERATED)/doom_build_config.h $(FIXTURE_STAMP)
FORCE:

# A failed switch must not leave an old signature blessing partly overwritten
# objects. This shared gate runs before any forced compilation, once per make.
invalidate-ripple-config:
	@node tools/build-support.mjs config-invalidate "$(ROUT)/.asset-config"

$(GENERATED)/doom_build_config.h: FORCE tools/build-support.mjs tools/assets-pack.mjs tools/assets-validate.mjs
	@node tools/build-support.mjs config "$(WAD)" "$(GENERATED)" "$(BUILD)/manifest.json" "$(BANK_SIZE)" "$(PROGRAM)" "$(START_MAP)" "$(LOW_DETAIL)" "$(DEFINES)" "$(RIPPLE_FLAGS)" "$(CC)" "$(CFLAGS)" "$(CPPFLAGS)" "$(LDFLAGS)" "$(WAD_MODE)" "$(ASSET_VALIDATION)"

# Recover a removed generated file without requiring deletion of all build data.
$(TABLE_SOURCES) $(INFO_SOURCES) $(GENERATED)/info_actions.h $(GENERATED)/info_test_actions.h: $(GENERATED)/sources.mk
	@test -f "$@" || node tools/generate-data.mjs engine "$(GENERATED)"

ifneq ($(FIXTURE_STAMP),)
$(FIXTURE_STAMP): tools/fixtures.mjs $(wildcard engine/*.c src/*.c tests/*.c)
	@$(GENERATE_$(PROGRAM))
	@touch "$@"
$(FIXTURE_OUTPUTS): $(FIXTURE_STAMP)
	@test -f "$@" || $(GENERATE_$(PROGRAM))
endif

$(ROUT)/.flags: FORCE tools/build-support.mjs Makefile
	@node tools/build-support.mjs stamp "$@" "$(RIPPLE_FLAGS)" "$(RCC)" "$(RASM)" "$(BANK_SIZE)"
$(ROUT)/.main-flags: FORCE tools/build-support.mjs
	@node tools/build-support.mjs stamp "$@" "$(SCENE_DEFINES)"
$(NOUT)/.flags: FORCE tools/build-support.mjs Makefile
	@node tools/build-support.mjs stamp "$@" "$(CC)" "$(NATIVE_FLAGS)" "$(LDFLAGS)"

$(RCC) $(RASM) $(RLINK):
	@printf 'Missing tool: %s\nBuild the release toolchain from the repository root first.\n' "$@" >&2
	@exit 1

# Complete the runtime's dependency check before the recursive link dependency
# check. This avoids relinking on every run and catches a rebuilt runtime archive.
runtime: $(RCC) $(RASM) $(RLINK)
	+@bash tools/log-command.sh "$(BUILD)/logs/runtime.log" $(MAKE) -C "$(ROOT)/runtime" all BANK_SIZE=$(BANK_SIZE)
ripple: runtime
	@test -n "$(SOURCES)" || { printf 'Unknown PROGRAM: %s\n' "$(PROGRAM)" >&2; exit 1; }
	@case ' $(NATIVE_FIXTURES) ' in *' $(PROGRAM) '*) printf '%s is native-only; use TARGET=native\n' '$(PROGRAM)' >&2; exit 1;; esac
	+@$(MAKE) --no-print-directory _ripple
bf: runtime
	@test -n "$(SOURCES)" || { printf 'Unknown PROGRAM: %s\n' "$(PROGRAM)" >&2; exit 1; }
	@case ' $(NATIVE_FIXTURES) ' in *' $(PROGRAM) '*) printf '%s is native-only; use TARGET=native\n' '$(PROGRAM)' >&2; exit 1;; esac
	+@$(MAKE) --no-print-directory _bf
native: _native
_ripple: $(RIPPLE_BINARY)
_bf: $(BF_IMAGE)
_native: $(NATIVE_BINARY)

define compile_c
$(ROUT)/$(call source_stem,$(1)).asm: $(1) $(HEADERS) $(RCC) $(ROUT)/.flags $(FIXTURE_STAMP) $(FIXTURE_OUTPUTS) $(RIPPLE_CONFIG_FORCE) $(if $(filter src/main.c,$(1)),$(ROUT)/.main-flags) | $(RIPPLE_CONFIG_GATE)
	@printf '  Ripple: %s\n' '$(1)'
	@bash tools/log-command.sh "$(ROUT)/$(call source_stem,$(1)).compile.log" "$(RCC)" compile "$(1)" -o "$$@" --bank-size $(BANK_SIZE) $(RIPPLE_FLAGS) $(if $(filter src/main.c,$(1)),$(SCENE_DEFINES))
endef
define copy_asm
$(ROUT)/$(call source_stem,$(1)).asm: $(1) $(ROUT)/.flags $(RIPPLE_CONFIG_FORCE) | $(RIPPLE_CONFIG_GATE)
	@cp "$$<" "$$@"
endef
define assemble
$(ROUT)/$(call source_stem,$(1)).pobj: $(ROUT)/$(call source_stem,$(1)).asm $(RASM) $(ROUT)/.flags $(RIPPLE_CONFIG_FORCE)
	@bash tools/log-command.sh "$(ROUT)/$(call source_stem,$(1)).assemble.log" "$(RASM)" assemble "$$<" -o "$$@" --bank-size $(BANK_SIZE) --max-immediate 65535
endef
$(foreach source,$(filter %.c,$(RIPPLE_SOURCES)),$(eval $(call compile_c,$(source))))
$(foreach source,$(filter %.asm,$(RIPPLE_SOURCES)),$(eval $(call copy_asm,$(source))))
$(foreach source,$(RIPPLE_SOURCES),$(eval $(call assemble,$(source))))

$(RIPPLE_BINARY): $(OBJECTS) $(ROOT)/runtime/crt0.pobj $(ROOT)/runtime/libruntime.par $(RLINK) Makefile tests/programs.mk $(GENERATED)/sources.mk $(RIPPLE_CONFIG_FORCE) $(BINARY_CONFIG_FORCE)
	@node tools/build-support.mjs config-invalidate "$@.asset-config"
	@bash tools/log-command.sh "$(ROUT)/link.log" "$(RLINK)" "$(ROOT)/runtime/crt0.pobj" "$(ROOT)/runtime/libruntime.par" $(OBJECTS) -f binary --bank-size $(BANK_SIZE) -o "$@"
	@node tools/build-support.mjs config-stamp "$(ROUT)/.asset-config" "$(ASSET_CONFIG_KEY)"
	@node tools/build-support.mjs config-stamp "$@.asset-config" "$(ASSET_CONFIG_KEY)"
	@printf '%s\n' "$@"
$(BF_IMAGE): $(OBJECTS) $(ROOT)/runtime/crt0.pobj $(ROOT)/runtime/libruntime.par $(RLINK) Makefile tests/programs.mk $(GENERATED)/sources.mk $(RIPPLE_CONFIG_FORCE) $(BF_CONFIG_FORCE)
	@node tools/build-support.mjs config-invalidate "$@.asset-config"
	@printf '  Linking Brainfuck CPU image: %s\n' '$(PROGRAM)'
	@bash tools/log-command.sh "$(ROUT)/bf-link.log" "$(RLINK)" "$(ROOT)/runtime/crt0.pobj" "$(ROOT)/runtime/libruntime.par" $(OBJECTS) -f bf --standalone --bf-host --bank-size $(BANK_SIZE) --memory-banks $(MEMORY_BANKS) -o "$@"
	@node tools/build-support.mjs config-stamp "$(ROUT)/.asset-config" "$(ASSET_CONFIG_KEY)"
	@node tools/build-support.mjs config-stamp "$@.asset-config" "$(ASSET_CONFIG_KEY)"
	@printf '%s\n' "$@"
$(NATIVE_BINARY): $(NATIVE_SOURCES) $(HEADERS) $(NOUT)/.flags $(FIXTURE_STAMP) $(FIXTURE_OUTPUTS) $(NATIVE_COMPILER) Makefile tests/programs.mk $(GENERATED)/sources.mk $(NATIVE_CONFIG_FORCE)
	@test -n "$(SOURCES)" || { printf 'Unknown PROGRAM: %s\n' "$(PROGRAM)" >&2; exit 1; }
	@printf '  Native: %s\n' '$(PROGRAM)'
	@node tools/build-support.mjs config-invalidate "$(NOUT)/.asset-config"
	@bash tools/log-command.sh "$(NOUT)/build.log" $(CC) $(NATIVE_FLAGS) $(NATIVE_SOURCES) $(LDFLAGS) -o "$@"
	@node tools/build-support.mjs config-stamp "$(NOUT)/.asset-config" "$(ASSET_CONFIG_KEY)"
	@printf '%s\n' "$@"

run:
	+@DOOM_BUILD="$(BUILD)" DOOM_WAD="$(WAD)" DOOM_WAD_MODE="$(WAD_MODE)" DOOM_ASSET_VALIDATION="$(ASSET_VALIDATION)" ./run.sh $(if $(filter 1,$(START_MAP)),--scene e1m1) $(if $(filter 1,$(LOW_DETAIL)),--low-detail) $(RUN_ARGS)
test:
	+@DOOM_BUILD="$(abspath $(BUILD))" DOOM_WAD_MODE="$(WAD_MODE)" node tools/test.mjs smoke --wad "$(WAD)"
test-fixtures:
	+@DOOM_BUILD="$(abspath $(BUILD))" DOOM_WAD_MODE="$(WAD_MODE)" node tools/test.mjs fixtures --wad "$(WAD)"
test-kernels:
	+@DOOM_BUILD="$(abspath $(BUILD))" DOOM_WAD_MODE="$(WAD_MODE)" node tools/test.mjs kernels --wad "$(WAD)"
test-gameplay:
	+@DOOM_BUILD="$(abspath $(BUILD))" DOOM_WAD_MODE="$(WAD_MODE)" node tools/test.mjs gameplay --wad "$(WAD)"
test-assets:
	+@DOOM_BUILD="$(abspath $(BUILD))" node --test tools/assets-pack.test.mjs tools/assets-validate.test.mjs tools/asset-preflight.test.mjs tools/asset-policy.test.mjs
	+@DOOM_BUILD="$(abspath $(BUILD))" node tools/metadata-validate.mjs --wad "$(WAD)"
	+@DOOM_BUILD="$(abspath $(BUILD))" node tools/texture-metadata-validate.mjs --wad "$(WAD)"
	+@DOOM_BUILD="$(abspath $(BUILD))" DOOM_WAD_MODE="$(WAD_MODE)" node tools/test.mjs assets --wad "$(WAD)"
clean:
	rm -rf "$(BUILD)/ripple" "$(BUILD)/native" "$(BUILD)/assets" "$(GENERATED)"
help:
	@printf '%s\n' 'make -j4 [TARGET=ripple|bf|native|both] [WAD_MODE=packed|expanded|cooked] [START_MAP=1] [LOW_DETAIL=1]' \
	 'ASSET_VALIDATION=preflight (default) uses assets checked when packed; guest also enables detailed guest checks.' \
	 'make PROGRAM=numeric TARGET=both' 'make test | test-fixtures | test-kernels | test-gameplay | test-assets' \
	 './run.sh --scene e1m1 --low-detail' './run-bf.sh --telemetry' 'WAD=/path/to/doom1.wad and BUILD=build may be overridden.'
