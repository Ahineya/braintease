# Doom uses the repository's existing compiler, assembler and VM hosts.
# Adapted engine sources are maintained in engine/; no fetch or patch step.
.DEFAULT_GOAL := all
.DELETE_ON_ERROR:
SHELL := /bin/bash

ROOT := $(abspath ../..)
BUILD ?= build
# Cleanup quotes these paths in the shell; keep spaces intact instead of
# splitting the directory into words through make's abspath function.
ifneq ($(MAKECMDGOALS),clean)
override BUILD := $(abspath $(BUILD))
endif
GENERATED := $(BUILD)/generated
PROGRAM ?= engine
TARGET ?= ripple
START_MAP ?= 0
START_DEMO ?= 0
LOW_DETAIL ?= 0
MOUSE_INPUT ?= 0
ifneq ($(words $(MOUSE_INPUT)),1)
$(error MOUSE_INPUT must be 0 or 1)
endif
ifeq ($(filter $(MOUSE_INPUT),0 1),)
$(error MOUSE_INPUT must be 0 or 1)
endif
RESOLUTION_SCALE ?= 1
ifneq ($(words $(RESOLUTION_SCALE)),1)
$(error RESOLUTION_SCALE must be 1, 2 or 4)
endif
ifeq ($(filter $(RESOLUTION_SCALE),1 2 4),)
$(error RESOLUTION_SCALE must be 1, 2 or 4)
endif
RENDERER_BF_SMOKE ?= 0
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
# It is expected to be absent after clean. The executable targets also require
# it explicitly, so generation failures still stop the build.
ifeq ($(filter clean help,$(MAKECMDGOALS)),)
-include $(GENERATED)/sources.mk
endif
$(GENERATED)/sources.mk: engine/tables.c engine/info.c tools/generate-data.mjs
	@node tools/generate-data.mjs engine "$(GENERATED)"

# BF guests retain the silent sound path: no mixer, sample reads, or PCM calls.
ifneq ($(filter bf _bf,$(TARGET) $(MAKECMDGOALS)),)
override PCM_AUDIO := 0
override MUSIC_AUDIO := 0
else
PCM_AUDIO ?= 1
MUSIC_AUDIO ?= 0
endif
ifneq ($(words $(PCM_AUDIO)),1)
$(error PCM_AUDIO must be 0 or 1)
endif
ifeq ($(filter $(PCM_AUDIO),0 1),)
$(error PCM_AUDIO must be 0 or 1)
endif

ifneq ($(words $(MUSIC_AUDIO)),1)
$(error MUSIC_AUDIO must be 0 or 1)
endif
ifeq ($(filter $(MUSIC_AUDIO),0 1),)
$(error MUSIC_AUDIO must be 0 or 1)
endif
ifeq ($(MUSIC_AUDIO):$(PCM_AUDIO),1:0)
$(error MUSIC_AUDIO requires PCM_AUDIO=1)
endif

ENGINE_NAMES := dummy am_map doomdef doomstat dstrings d_event d_items d_iwad d_loop d_main d_mode d_net \
 f_finale g_game hu_lib hu_stuff info i_cdmus i_endoom i_joystick i_sound memio \
 m_argv m_bbox m_cheat m_controls m_fixed m_menu m_misc m_random \
 p_ceilng p_doors p_enemy p_floor p_inter p_lights p_map p_maputl p_mobj p_plats p_pspr p_saveg \
 p_setup p_sight p_spec p_switch p_telept p_tick p_user \
 r_bsp r_data r_draw r_main r_plane r_segs r_sky r_things \
 sounds statdump st_lib st_stuff s_sound tables v_video wi_stuff w_file w_main w_wad z_zone
SOURCES_engine = $(addprefix engine/,$(addsuffix .c,$(ENGINE_NAMES))) $(TABLE_SOURCES) $(INFO_SOURCES) \
 src/main.c src/platform.c src/config_port.c src/wad_io.c src/w_file_port.c src/patch_cache.c src/alloc_fast.c src/asset_metadata.c \
 src/render_fast.asm src/raster_fast.asm src/memory_fast.asm $(if $(filter 1,$(PCM_AUDIO)),src/pcm_sound.c) $(if $(filter 1,$(MUSIC_AUDIO)),src/music_sound.c)
include tests/programs.mk

SOURCES := $(SOURCES_$(PROGRAM))
RIPPLE_SOURCES := $(SOURCES) $(if $(filter engine/m_fixed.c,$(SOURCES)),src/fixed_fast.asm) \
 $(if $(filter src/wad_io.c,$(SOURCES)),src/word_read_fast.asm) \
 $(if $(filter src/asset_metadata.c,$(SOURCES)),src/metadata_validate_fast.asm) \
 $(if $(filter engine libc wad_lookup wad_directory texture_metadata sprite_metadata sprite_metrics renderer_tables,$(PROGRAM)),src/compat_libc.c)
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
ASSET_CONFIG_KEY := $(shell node tools/build-support.mjs config-key "$(WAD)" "$(WAD_MODE)" "$(BANK_SIZE)" "$(GENERATED)" "$(ASSET_VALIDATION)" "$(RESOLUTION_SCALE)")
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
DEFINES := -DDOOM_MOUSE=$(MOUSE_INPUT) -DDOOM_PCM=$(PCM_AUDIO) -DDOOM_MUSIC=$(MUSIC_AUDIO) -DDOOM_GENERATED_TABLES=1 -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 -DDOOM_PORT=1 -DDOOM_RESOLUTION_SCALE=$(RESOLUTION_SCALE) \
 $(if $(filter engine info_storage,$(PROGRAM)),-DDOOM_GENERATED_INFO=1) \
 $(if $(filter renderer_tables,$(PROGRAM)),$(if $(filter 1,$(RENDERER_BF_SMOKE)),-DDOOM_RENDERER_BF_SMOKE=1))
SCENE_DEFINES := $(if $(filter 1,$(START_MAP)),-DDOOM_START_MAP=1) $(if $(filter 1,$(START_DEMO)),-DDOOM_START_DEMO=1) $(if $(filter 1,$(LOW_DETAIL)),-DDOOM_LOW_DETAIL=1)
RIPPLE_FLAGS := -Iinclude $(INCLUDES) -I$(ROOT)/runtime/include $(DEFINES) -DRIPPLE=1 -DBANK_SIZE=$(BANK_SIZE) \
 -DDOOM_ASSET_STRICT=$(if $(filter guest,$(ASSET_VALIDATION)),1,0) \
 $(if $(filter engine memory_kernels alloc_fast texture_metadata sprite_metadata sprite_metrics renderer_tables renderer_storage,$(PROGRAM)),-DDOOM_FAST_MEMORY=1)
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
FIXTURE_FILES_sprite_metadata := sprite_metadata.inc sprite_names.inc
FIXTURE_FILES_sprite_metrics := sprite_metrics.inc
FIXTURE_FILES_renderer_tables := renderer_tables.inc
FIXTURE_FILES_renderer_storage := renderer_storage.inc
FIXTURE_FILES_keyboard_controls := input_commands.inc controls_defaults.inc platform_keyboard.inc
FIXTURE_FILES_config_bindings := config_bindings.inc
FIXTURE_OUTPUTS := $(addprefix $(GENERATED)/,$(FIXTURE_FILES_$(PROGRAM)))

.PHONY: all ripple bf native both prepare runtime run clean help test test-fixtures test-kernels test-gameplay test-presentation test-resolution test-assets FORCE _ripple _bf _native invalidate-ripple-config
all: $(TARGET)
both: native ripple
prepare: $(GENERATED)/sources.mk $(GENERATED)/doom_build_config.h $(FIXTURE_STAMP)
FORCE:

# A failed switch must not leave an old signature blessing partly overwritten
# objects. This shared gate runs before any forced compilation, once per make.
invalidate-ripple-config:
	@node tools/build-support.mjs config-invalidate "$(ROUT)/.asset-config"

$(GENERATED)/doom_build_config.h: FORCE tools/build-support.mjs tools/assets-pack.mjs tools/assets-validate.mjs tools/assets-identity.mjs tools/sprite-metadata.mjs tools/sprite-metrics.mjs tools/renderer-tables.mjs tools/renderer-tables-validate.mjs
	@node tools/build-support.mjs config "$(WAD)" "$(GENERATED)" "$(BUILD)/manifest.json" "$(BANK_SIZE)" "$(PROGRAM)" "$(START_MAP)" "$(LOW_DETAIL)" "$(DEFINES)" "$(RIPPLE_FLAGS)" "$(CC)" "$(CFLAGS)" "$(CPPFLAGS)" "$(LDFLAGS)" "$(WAD_MODE)" "$(ASSET_VALIDATION)" "$(START_DEMO)" "$(RESOLUTION_SCALE)"

# Recover a removed generated file without requiring deletion of all build data.
$(TABLE_SOURCES) $(INFO_SOURCES) $(GENERATED)/info_actions.h $(GENERATED)/info_test_actions.h: $(GENERATED)/sources.mk
	@test -f "$@" || node tools/generate-data.mjs engine "$(GENERATED)"

ifneq ($(FIXTURE_STAMP),)
$(FIXTURE_STAMP): tools/fixtures.mjs tools/sprite-fixture.mjs tools/sprite-metrics-fixture.mjs tools/renderer-storage-fixture.mjs $(wildcard engine/*.c src/*.c tests/*.c tests/*.inc)
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
	+@bash tools/log-command.sh "$(BUILD)/logs/runtime.log" $(MAKE) -C "$(ROOT)/runtime" all BANK_SIZE=$(BANK_SIZE) AUDIO=$(PCM_AUDIO)
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
	+@DOOM_BUILD="$(BUILD)" DOOM_WAD="$(WAD)" DOOM_WAD_MODE="$(WAD_MODE)" DOOM_ASSET_VALIDATION="$(ASSET_VALIDATION)" ./run.sh $(if $(filter rcc-native,$(TARGET)),--target native) $(if $(filter 1,$(MOUSE_INPUT)),--mouse) $(if $(filter 1,$(START_MAP)),--scene e1m1) $(if $(filter 1,$(START_DEMO)),--scene demo1) $(if $(filter 1,$(LOW_DETAIL)),--low-detail) $(if $(filter 2,$(RESOLUTION_SCALE)),--half-resolution) $(if $(filter 4,$(RESOLUTION_SCALE)),--quarter-resolution) $(RUN_ARGS)
test:
	+@DOOM_BUILD="$(abspath $(BUILD))" DOOM_WAD_MODE="$(WAD_MODE)" node tools/test.mjs smoke --wad "$(WAD)"
test-fixtures:
	+@DOOM_BUILD="$(abspath $(BUILD))" DOOM_WAD_MODE="$(WAD_MODE)" node tools/test.mjs fixtures --wad "$(WAD)"
test-kernels:
	+@DOOM_BUILD="$(abspath $(BUILD))" DOOM_WAD_MODE="$(WAD_MODE)" node tools/test.mjs kernels --wad "$(WAD)"
test-gameplay:
	+@DOOM_BUILD="$(abspath $(BUILD))" DOOM_WAD_MODE="$(WAD_MODE)" node tools/test.mjs gameplay --wad "$(WAD)"
test-presentation:
	+@DOOM_BUILD="$(abspath $(BUILD))" DOOM_WAD_MODE="$(WAD_MODE)" node tools/test.mjs presentation --wad "$(WAD)"
test-resolution:
	+@node --test tools/resolution.test.mjs
	+@DOOM_BUILD="$(abspath $(BUILD))" DOOM_WAD_MODE="$(WAD_MODE)" node tools/test.mjs resolution --wad "$(WAD)"
test-assets:
	+@DOOM_BUILD="$(abspath $(BUILD))" node --test tools/assets-pack.test.mjs tools/assets-validate.test.mjs tools/asset-preflight.test.mjs tools/asset-policy.test.mjs tools/assets-identity.test.mjs tools/sprite-metadata.test.mjs tools/sprite-metrics.test.mjs tools/renderer-tables-validate.test.mjs
	+@DOOM_BUILD="$(abspath $(BUILD))" node tools/metadata-validate.mjs --wad "$(WAD)"
	+@DOOM_BUILD="$(abspath $(BUILD))" node tools/texture-metadata-validate.mjs --wad "$(WAD)"
	+@DOOM_BUILD="$(abspath $(BUILD))" node tools/sprite-metadata-validate.mjs --wad "$(WAD)"
	+@DOOM_BUILD="$(abspath $(BUILD))" node tools/sprite-metrics-validate.mjs --wad "$(WAD)"
	+@DOOM_BUILD="$(abspath $(BUILD))" node tools/renderer-metadata-validate.mjs --wad "$(WAD)"
	+@DOOM_BUILD="$(abspath $(BUILD))" DOOM_WAD_MODE="$(WAD_MODE)" node tools/test.mjs assets --wad "$(WAD)"
clean:
	rm -rf "$(BUILD)/ripple" "$(BUILD)/native" "$(BUILD)/rcc-native" "$(BUILD)/assets" "$(GENERATED)"
help:
	@printf '%s\n' 'make -j4 [TARGET=ripple|bf|native|rcc-native|both] [WAD_MODE=packed|expanded|cooked] [START_MAP=1|START_DEMO=1] [LOW_DETAIL=1] [RESOLUTION_SCALE=1|2|4] [MOUSE_INPUT=1]' \
	 'rcc-native compiles RCC IR through Cranelift; native is the Clang/GCC reference.' \
	 'ASSET_VALIDATION=preflight (default) uses assets checked when packed; guest also enables detailed guest checks.' \
	 'make PROGRAM=numeric TARGET=both' 'make test | test-fixtures | test-kernels | test-gameplay | test-presentation | test-resolution | test-assets' \
	 './run.sh --scene e1m1 --low-detail' './run-bf.sh --telemetry' 'WAD=/path/to/doom1.wad and BUILD=build may be overridden.'

# RCC host code generation keeps each C translation unit separate through IR.
# The clang "native" reference remains available for pixel comparisons.
RNOUT := $(BUILD)/rcc-native/$(PROGRAM)
RCC_NATIVE_SOURCES := $(sort $(NATIVE_SOURCES) src/compat_libc.c $(ROOT)/runtime/vm_devices.c $(if $(filter 1,$(PCM_AUDIO)),$(ROOT)/runtime/vm_audio.c) $(if $(filter 1,$(MUSIC_AUDIO)),$(ROOT)/runtime/vm_music.c) $(ROOT)/runtime/ctype.c $(ROOT)/runtime/scanf.c src/native_devices.c)
RCC_NATIVE_FLAGS := -Iinclude $(INCLUDES) -I$(ROOT)/runtime/include $(DEFINES) -DRCC_NATIVE=1 -DRIPPLE_MMIO_NO_INLINE=1 -DDOOM_ASSET_STRICT=$(if $(filter guest,$(ASSET_VALIDATION)),1,0)
RCC_NATIVE_IR := $(foreach source,$(RCC_NATIVE_SOURCES),$(RNOUT)/$(call source_stem,$(source)).ir)
.PHONY: rcc-native test-native
rcc-native: $(RNOUT)/$(PROGRAM)
test-native:
	+@DOOM_BUILD="$(BUILD)" DOOM_WAD="$(WAD)" DOOM_WAD_MODE="$(WAD_MODE)" node tools/test-native.mjs
$(RNOUT)/.flags: FORCE Makefile
	@node tools/build-support.mjs stamp "$@" "$(RCC_NATIVE_FLAGS)" "$(RCC)" "$(ASSET_CONFIG_KEY)"
$(RNOUT)/.main-flags: FORCE tools/build-support.mjs
	@node tools/build-support.mjs stamp "$@" "$(SCENE_DEFINES)"
define compile_native_ir
$(RNOUT)/$(call source_stem,$(1)).ir: $(1) $(HEADERS) $(RCC) $(RNOUT)/.flags $(FIXTURE_STAMP) $(FIXTURE_OUTPUTS) $(if $(filter src/main.c,$(1)),$(RNOUT)/.main-flags)
	@printf '  RCC native IR: %s\n' '$(1)'
	@"$(RCC)" compile "$(1)" --target native --emit-ir $(RCC_NATIVE_FLAGS) $(if $(filter src/main.c,$(1)),$(SCENE_DEFINES)) > "$$@.tmp" 2> "$$@.log" || { cat "$$@.log" >&2; rm -f "$$@.tmp"; exit 1; }
	@mv "$$@.tmp" "$$@"
endef
$(foreach source,$(RCC_NATIVE_SOURCES),$(eval $(call compile_native_ir,$(source))))
$(RNOUT)/$(PROGRAM): $(GENERATED)/sources.mk $(RCC_NATIVE_IR) $(ROOT)/target/release/librcc_native_runtime.a $(RCC)
	@printf '  RCC native link: %s\n' '$(PROGRAM)'
	@bash tools/log-command.sh "$(RNOUT)/link.log" "$(RCC)" link-native $(RCC_NATIVE_IR) -o "$@"
	@printf '%s\n' "$@"
