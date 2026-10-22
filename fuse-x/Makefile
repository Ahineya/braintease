.DEFAULT_GOAL := all
.DELETE_ON_ERROR:
.SECONDARY:
ROOT := $(abspath ..)
BUILD ?= $(CURDIR)/build
RCC ?= $(ROOT)/target/release/rcc
NATIVE_RUNTIME ?= $(if $(RCC_NATIVE_RUNTIME),$(RCC_NATIVE_RUNTIME),$(ROOT)/target/release/librcc_native_runtime.a)
RFL ?= $(ROOT)/target/release/rfl
K := $(ROOT)/ports/fuzix/upstream/Kernel
# Select the shared wide userspace ABI. Address spaces are implemented by the
# native hosted platform, independent of Ripple's hardware MMU implementation.
export FUSE_X_HEAP_PROFILE ?= hosted
ifeq ($(FUSE_X_HEAP_PROFILE),bounded)
HEAP_FLAGS := -DFUSE_X_BOUNDED_HEAP=1
else ifneq ($(FUSE_X_HEAP_PROFILE),hosted)
$(error FUSE_X_HEAP_PROFILE must be hosted or bounded)
endif
FLAGS := $(HEAP_FLAGS) -DRIPPLE_FUZIX_NETWORK -DRIPPLE_MMU=1 --target native --bank-size 64000 -DRIPPLE_BULK_IO=0 -DRIPPLE_MMIO_NO_INLINE=1 -I$(CURDIR)/include -I$(ROOT)/ports/fuzix/ripple -I$(K)/include -I$(K)/dev -I$(ROOT)/runtime/include
CORE := start devio filesys inode kdata process mm syscall_exec syscall_fs syscall_fs2 syscall_fs3 syscall_other syscall_proc timer tty blk512 usermem select
NET_SRC := core config wire ipv4 link arp history icmp icmp_error udp tcp tcp_wire tcp_ack tcp_payload tcp_stream tcp_poll tcp_receive tcp_decode tcp_error tcp_listen tcp_passive tcp_wait
UNITS := $(addprefix $(BUILD)/kernel/,$(addsuffix .ir,$(CORE))) $(BUILD)/kernel/memalloc_none.ir $(BUILD)/port/platform.ir $(BUILD)/port/devices.ir $(BUILD)/port/sysdev.ir $(BUILD)/port/check.ir $(BUILD)/port/memory.ir $(BUILD)/port/exec.ir $(BUILD)/port/jobcontrol.ir $(BUILD)/port/pty.ir $(BUILD)/port/version.ir $(BUILD)/runtime/vm_devices.ir $(BUILD)/runtime/vm_network.ir $(BUILD)/port/sockets.ir $(addprefix $(BUILD)/runtime/net/,$(addsuffix .ir,$(NET_SRC)))
UNITS += $(BUILD)/port/hostfs.ir $(BUILD)/port/remotefs.ir $(BUILD)/port/remotefs-node.ir $(BUILD)/port/selectwait.ir $(BUILD)/port/deadline.ir $(BUILD)/port/clock.ir $(BUILD)/port/localsockets.ir
UNITS += $(BUILD)/port/scheduler.ir $(BUILD)/port/pipes.ir $(BUILD)/port/kernel-io.ir
UNITS += $(BUILD)/port/console.ir
UNITS += $(BUILD)/port/graphics.ir
$(BUILD)/port/graphics.ir: $(ROOT)/ports/fuzix/ripple/raster-abi.h render-abi.h session-abi.h panel-abi.h $(ROOT)/ports/graphics/include/gx-session.h $(ROOT)/ports/graphics/include/gx-panel.h
$(BUILD)/port/init.ir: $(ROOT)/ports/graphics/include/gx-panel.h
UNITS += $(BUILD)/port/pcm.ir $(BUILD)/runtime/vm_audio.ir $(BUILD)/runtime/vm_music.ir
HEADERS_PCM := $(ROOT)/ports/fuzix/ripple/include/sys/pcm.h
$(BUILD)/port/pcm.ir: $(HEADERS_PCM)
HOSTFS_UNITS := filesys inode devio syscall_fs syscall_fs3 syscall_other syscall_fs2
APPS := $(addprefix $(BUILD)/port/,$(addsuffix .ir,userlib init shell process-check))
HEADERS := $(wildcard include/*.h $(K)/include/*.h $(ROOT)/ports/fuzix/ripple/*.h $(ROOT)/runtime/include/*.h)
HEADERS += $(wildcard $(ROOT)/ports/fuzix/ripple/*.inc)
FLAGS += -I$(ROOT)/ports/graphics/include
HEADERS += $(ROOT)/ports/graphics/include/gx.h
HEAP_STAMP := $(shell python3 heap_profile.py --build '$(BUILD)' --profile '$(FUSE_X_HEAP_PROFILE)')
ifeq ($(strip $(HEAP_STAMP)),)
$(error Failed to record the heap profile)
endif
HEADERS += $(HEAP_STAMP)
.PHONY: all tools test test-rfl
.PHONY: freestanding-ir
FREESTANDING_PLATFORM = $(BUILD)/port/freestanding$(if $(filter 1,$(FREESTANDING_BOOTSTRAP)),-bootstrap)-platform.ir
freestanding-ir: $(filter-out $(BUILD)/port/platform.ir $(BUILD)/port/exec.ir,$(UNITS)) $(APPS) $(FREESTANDING_PLATFORM) $(BUILD)/port/freestanding-exec.ir $(BUILD)/port/freestanding-commands.ir
	@printf '%s\n' $(filter-out $(BUILD)/port/platform.ir $(BUILD)/port/exec.ir,$(UNITS)) $(FREESTANDING_PLATFORM) $(BUILD)/port/freestanding-exec.ir > $(BUILD)/freestanding-kernel-inputs.txt
	@printf '%s\n' $(APPS) $(BUILD)/port/freestanding-commands.ir > $(BUILD)/freestanding-app-inputs.txt
$(BUILD)/port/freestanding-platform.ir: $(ROOT)/tools/x86_64/riplix/platform.c platform.c $(HEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@$(RCC) compile $< $(FLAGS) --emit-ir > $@.tmp 2>$@.log || { cat $@.log >&2; exit 1; }
	@mv $@.tmp $@
$(BUILD)/port/freestanding-bootstrap-platform.ir: $(ROOT)/tools/x86_64/riplix/bootstrap.c $(ROOT)/tools/x86_64/riplix/platform.c platform.c $(HEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@$(RCC) compile $< $(FLAGS) --emit-ir > $@.tmp 2>$@.log || { cat $@.log >&2; exit 1; }
	@mv $@.tmp $@
$(BUILD)/port/freestanding-exec.ir: $(ROOT)/tools/x86_64/riplix/exec.c exec.c $(HEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@$(RCC) compile $< $(FLAGS) --emit-ir > $@.tmp 2>$@.log || { cat $@.log >&2; exit 1; }
	@mv $@.tmp $@
$(BUILD)/port/freestanding-commands.ir: $(ROOT)/tools/x86_64/riplix/commands.c $(HEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@$(RCC) compile $< $(FLAGS) --emit-ir > $@.tmp 2>$@.log || { cat $@.log >&2; exit 1; }
	@mv $@.tmp $@
# The application inventory builds Lynx once, after the prerequisite programs.
# Keeping it here too ran a second fingerprint/build pass in the same launch.
APP_NAMES := native-hello cat ls cp sh rfl-demo rfl-fm rift qr-demo telegram music wavplay chat
.PHONY: telegram
telegram: $(BUILD)/apps/telegram.fxe
.PHONY: music
music: $(BUILD)/apps/music.fxe $(BUILD)/apps/wavplay.fxe
EXECUTABLES := $(addprefix $(BUILD)/apps/,$(addsuffix .fxe,$(APP_NAMES)))
# Track mode changes without touching the stamp on unchanged builds.
export FUSE_X_SHARED_LIBRARIES ?= 1
LINK_MODE := $(shell python3 link_mode.py --build '$(BUILD)')
export RCC_NATIVE_MEMORY ?= checked
ifneq ($(RCC_NATIVE_MEMORY),checked)
ifneq ($(RCC_NATIVE_MEMORY),read-v1)
$(error RCC_NATIVE_MEMORY must be checked or read-v1)
endif
endif
MEMORY_MODE := $(shell RCC_NATIVE_MEMORY='$(RCC_NATIVE_MEMORY)' python3 memory_mode.py --build '$(BUILD)')
ifeq ($(strip $(MEMORY_MODE)),)
$(error Failed to record RCC_NATIVE_MEMORY)
endif
SDK_SOURCES := $(wildcard $(ROOT)/libraries/unicode/*.[ch] $(ROOT)/libraries/unicode/*.inc) $(wildcard $(ROOT)/ports/fuzix/ripple/libc/*.c $(ROOT)/ports/fuzix/upstream/Library/libs/*.c) $(ROOT)/ports/fuzix/ripple/userlib.c libc/bridge.c cc.py
SDK_SOURCES += $(LINK_MODE) $(HEAP_STAMP) $(MEMORY_MODE) memory_mode.py
SDK_HEADERS := $(K)/include/userstructs.h $(shell find $(ROOT)/ports/fuzix/ripple/include $(ROOT)/ports/fuzix/upstream/Library/include $(ROOT)/runtime/include -name '*.h')
SDK_HEADERS += $(ROOT)/ports/fuzix/ripple/exec-args.h
SDK_HEADERS += include/memory-layout.h
SDK_HEADERS += $(ROOT)/ports/fuzix/ripple/identity.h
SDK_HEADERS += $(ROOT)/ports/fuzix/ripple/select-abi.h $(ROOT)/ports/fuzix/ripple/file-limits.h
all: $(BUILD)/fuse-x $(EXECUTABLES) applications
all: graphics
# A desktop launch opts into the Graphite shell and catalog applications. This
# target is hosted-only; freestanding/x64 preparation does not start or build it.
ifeq ($(FUSE_X_GRAPHITE),1)
.PHONY: graphite
all: graphite
graphite:
	python3 graphite.py --build $(BUILD) --compiler $(RCC)
endif
.PHONY: bundle
all: bundle
bundle: $(BUILD)/fuse-x
	python3 bundle.py --build $(BUILD)
.PHONY: graphics test-graphics
graphics:
	python3 graphics.py --build $(BUILD) --compiler $(RCC)
test-graphics: $(BUILD)/fuse-x graphics $(addprefix $(BUILD)/apps/,$(addsuffix .fxe,sh native-hello cat ls cp))
	python3 apps.py echo kill ps --compiler $(RCC) --build $(BUILD)
	python3 test_graphics.py --executable $(BUILD)/fuse-x
.PHONY: test-process-capacity
test-process-capacity: $(BUILD)/fuse-x $(BUILD)/apps/native-hello.fxe $(BUILD)/apps/sh.fxe
	python3 apps.py ps top free uptime echo sleep --compiler $(RCC) --build $(BUILD)
	python3 $(ROOT)/ports/fuzix/tools/test_process_capacity.py --native --native-build $(BUILD)
tools:
	cd $(ROOT) && cargo build --release -p rcc-driver -p rcc-native-runtime -p rfl
$(BUILD)/kernel/%.ir: $(K)/%.c $(HEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@printf '  RCC %s\n' '$<'
	@$(RCC) compile $< $(FLAGS) $(if $(filter syscall_proc,$*),-D_brk=fuse_legacy_brk -D_sbrk=fuse_legacy_sbrk -D_pause=fuse_legacy_pause -D_time=ripple_legacy_time) --emit-ir > $@.tmp 2>$@.log || { cat $@.log >&2; exit 1; }
	@mv $@.tmp $@
$(addprefix $(BUILD)/generated/,$(addsuffix .c,$(HOSTFS_UNITS))): $(BUILD)/generated/%.c: $(K)/%.c hostfs.py
	python3 hostfs.py $< $@
$(addprefix $(BUILD)/kernel/,$(addsuffix .ir,$(HOSTFS_UNITS))): $(BUILD)/kernel/%.ir: $(BUILD)/generated/%.c $(HEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@printf '  RCC %s\n' '$<'
	@$(RCC) compile $< $(FLAGS) --emit-ir > $@.tmp 2>$@.log || { cat $@.log >&2; exit 1; }
	@mv $@.tmp $@
$(BUILD)/kernel/memalloc_none.ir: $(K)/mm/memalloc_none.c $(HEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@printf '  RCC %s\n' '$<'
	@$(RCC) compile $< $(FLAGS) --emit-ir > $@.tmp 2>$@.log || { cat $@.log >&2; exit 1; }
	@mv $@.tmp $@
$(BUILD)/port/%.ir: %.c $(HEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@printf '  RCC %s\n' '$<'
	@$(RCC) compile $< $(FLAGS) --emit-ir > $@.tmp 2>$@.log || { cat $@.log >&2; exit 1; }
	@mv $@.tmp $@
$(BUILD)/generated/sockets.c: $(ROOT)/ports/fuzix/ripple/sockets.c network.py
	python3 network.py $< $@
$(BUILD)/generated/selectwait.c: $(ROOT)/ports/fuzix/ripple/selectwait.c graphics_adapt.py
	@mkdir -p $(@D)
	python3 graphics_adapt.py $< $@
$(BUILD)/port/selectwait.ir: $(BUILD)/generated/selectwait.c $(HEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@$(RCC) compile $< $(FLAGS) --emit-ir > $@.tmp 2>$@.log || { cat $@.log >&2; exit 1; }
	@mv $@.tmp $@
$(BUILD)/port/sockets.ir: $(BUILD)/generated/sockets.c $(HEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@printf '  RCC %s\n' '$<'
	@$(RCC) compile $< $(FLAGS) --emit-ir > $@.tmp 2>$@.log || { cat $@.log >&2; exit 1; }
	@mv $@.tmp $@
$(BUILD)/port/%.ir: $(ROOT)/ports/fuzix/ripple/%.c $(HEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@printf '  RCC %s\n' '$<'
	@$(RCC) compile $< $(FLAGS) --emit-ir > $@.tmp 2>$@.log || { cat $@.log >&2; exit 1; }
	@mv $@.tmp $@
$(BUILD)/runtime/%.ir: $(ROOT)/runtime/%.c $(HEADERS) $(wildcard $(ROOT)/runtime/net/*.h) $(RCC) Makefile
	@mkdir -p $(@D)
	@printf '  RCC %s\n' '$<'
	@$(RCC) compile $< $(FLAGS) --emit-ir > $@.tmp 2>$@.log || { cat $@.log >&2; exit 1; }
	@mv $@.tmp $@
$(BUILD)/fuse-x: $(UNITS) $(APPS) $(NATIVE_RUNTIME) $(MEMORY_MODE)
	@printf '  LINK %s\n' '$@'
	@RCC_NATIVE_RUNTIME='$(NATIVE_RUNTIME)' $(RCC) link-native $(addprefix --kernel ,$(UNITS)) $(APPS) -o $@ >$(BUILD)/link.log 2>&1 || { cat $(BUILD)/link.log >&2; exit 1; }
# Boot tools are ordinary native userland; no x64/Freestanding activation.
.PHONY: services
services: $(addprefix $(BUILD)/apps/,$(addsuffix .fxe,svcd svc console-session))
$(addprefix $(BUILD)/apps/,$(addsuffix .fxe,svcd svc console-session)): $(wildcard $(ROOT)/ports/services/*.[ch] $(ROOT)/libraries/ini/*.[ch]) apps.py $(SDK_SOURCES) $(SDK_HEADERS) $(RCC)
	python3 apps.py $(@F:.fxe=) --compiler $(RCC) --build $(BUILD)

$(BUILD)/apps/native-hello.fxe: examples/hello.c $(SDK_SOURCES) $(SDK_HEADERS) $(RCC)
	@mkdir -p $(@D)
	python3 cc.py $< --compiler $(RCC) --build $(BUILD) -o $@
$(BUILD)/apps/rfl-demo.fxe: $(ROOT)/rfl/examples/posix.rfl $(shell find $(ROOT)/rfl/std $(ROOT)/rfl/lib $(ROOT)/libraries/ini $(ROOT)/libraries/tui-theme -type f) $(ROOT)/rfl/os/posix.c $(ROOT)/rfl/tools/build_os.py $(SDK_SOURCES) $(SDK_HEADERS) $(RFL)
	@mkdir -p $(@D)
	$(RFL) build $< --os fuse-x --build-dir $(BUILD) --compiler $(RCC) -o $@
$(BUILD)/apps/qr-demo.fxe: $(ROOT)/rfl/examples/qr-demo.rfl $(ROOT)/rfl/lib/qr.rfl $(ROOT)/rfl/std/curses.rfl $(shell find $(ROOT)/libraries/qr -type f) $(wildcard $(ROOT)/rfl/os/*.c) $(wildcard $(ROOT)/ports/fuzix/ripple/curses/*.c) $(ROOT)/rfl/tools/build_os.py $(SDK_SOURCES) $(SDK_HEADERS) $(RFL)
	@mkdir -p $(@D)
	$(RFL) build $< --os fuse-x --build-dir $(BUILD) --compiler $(RCC) -o $@
$(BUILD)/apps/rfl-fm.fxe: $(wildcard $(ROOT)/rfl/apps/commander/*.rfl) $(shell find $(ROOT)/rfl/std $(ROOT)/rfl/lib $(ROOT)/libraries/text $(ROOT)/libraries/ini $(ROOT)/libraries/tui-theme -type f) $(wildcard $(ROOT)/rfl/os/*.c) $(ROOT)/rfl/tools/build_os.py $(SDK_SOURCES) $(SDK_HEADERS) $(RFL) $(wildcard $(ROOT)/ports/fuzix/ripple/curses/*.c)
	@mkdir -p $(@D)
	$(RFL) build $(ROOT)/rfl/apps/commander/main.rfl --os fuse-x --build-dir $(BUILD) --compiler $(RCC) -o $@
$(BUILD)/apps/telegram.fxe: $(wildcard $(ROOT)/rfl/apps/telegram/*.rfl) $(shell find $(ROOT)/ports/telegram -type f -name '*.[ch]' ! -path '*/tests/*') $(shell find $(ROOT)/rfl/std $(ROOT)/rfl/lib $(ROOT)/libraries/qr $(ROOT)/libraries/ini $(ROOT)/libraries/tui-theme -type f) $(wildcard $(ROOT)/rfl/os/*.c) $(ROOT)/rfl/tools/build_os.py $(SDK_SOURCES) $(SDK_HEADERS) $(RFL) $(wildcard $(ROOT)/ports/fuzix/ripple/curses/*.c)
	@mkdir -p $(@D)
	$(RFL) build $(ROOT)/rfl/apps/telegram/main.rfl --os fuse-x --build-dir $(BUILD) --compiler $(RCC) -o $@
$(BUILD)/apps/music.fxe: $(wildcard $(ROOT)/rfl/apps/music/*.rfl $(ROOT)/ports/music/*.[ch]) $(shell find $(ROOT)/rfl/std $(ROOT)/rfl/lib $(ROOT)/libraries/ini $(ROOT)/libraries/tui-theme -type f) $(wildcard $(ROOT)/rfl/os/*.c $(ROOT)/ports/fuzix/ripple/curses/*.c) $(SDK_SOURCES) $(SDK_HEADERS) $(RFL)
	@mkdir -p $(@D)
	$(RFL) build $(ROOT)/rfl/apps/music/main.rfl --os fuse-x --build-dir $(BUILD) --compiler $(RCC) -o $@
$(BUILD)/apps/wavplay.fxe: $(wildcard $(ROOT)/ports/music/*.[ch]) $(SDK_SOURCES) $(SDK_HEADERS) $(RCC)
	python3 cc.py $(ROOT)/ports/music/wavplay.c $(ROOT)/ports/music/music.c $(ROOT)/ports/music/wav.c --compiler $(RCC) --build $(BUILD) -o $@
$(BUILD)/apps/pcmcheck.fxe: $(ROOT)/ports/music/tests/pcmcheck.c $(SDK_SOURCES) $(SDK_HEADERS) $(RCC)
	python3 cc.py $< --compiler $(RCC) --build $(BUILD) -o $@
$(BUILD)/apps/rift.fxe: $(wildcard $(ROOT)/rfl/apps/rift/*.rfl) $(shell find $(ROOT)/rfl/std $(ROOT)/rfl/lib $(ROOT)/libraries/text $(ROOT)/libraries/ini $(ROOT)/libraries/tui-theme -type f) $(wildcard $(ROOT)/rfl/os/*.c) $(ROOT)/rfl/tools/build_os.py $(SDK_SOURCES) $(SDK_HEADERS) $(RFL) $(wildcard $(ROOT)/ports/fuzix/ripple/curses/*.c)
	@mkdir -p $(@D)
	$(RFL) build $(ROOT)/rfl/apps/rift/main.rfl --os fuse-x --build-dir $(BUILD) --compiler $(RCC) -o $@
.PHONY: test-rift
test-rift: $(BUILD)/fuse-x $(BUILD)/apps/rift.fxe
	python3 $(ROOT)/rfl/tools/test_rift.py --os fuse-x --native-build $(BUILD) --rfl $(RFL)
$(BUILD)/apps/sh.fxe: $(wildcard $(ROOT)/ports/fuzix/upstream/Applications/V7/cmd/sh/*.[ch]) $(ROOT)/ports/fuzix/ripple/shell-input.c $(ROOT)/ports/fuzix/ripple/shell-jobs.c $(ROOT)/ports/fuzix/ripple/shell-prompt.c $(ROOT)/ports/fuzix/ripple/shell-prompt.h apps.py $(SDK_SOURCES) $(SDK_HEADERS) $(RCC)
	python3 apps.py sh --compiler $(RCC) --build $(BUILD)
# apps.py fingerprints the browser, its configuration and the shared SDK.
.PHONY: lynx-check
lynx-check:
$(BUILD)/apps/lynx.fxe: lynx-check
	python3 apps.py lynx --compiler $(RCC) --build $(BUILD)
.PHONY: applications
applications: $(EXECUTABLES)
	python3 apps.py --all --require lynx --require chat --require svcd --require svc --require console-session --require mux --compiler $(RCC) --build $(BUILD)
$(BUILD)/apps/%.fxe: $(ROOT)/ports/fuzix/upstream/Applications/util/%.c $(SDK_SOURCES) $(SDK_HEADERS) $(RCC)
	@mkdir -p $(@D)
	python3 cc.py $< $(if $(filter ls,$*),-DUIDGID=0) --compiler $(RCC) --build $(BUILD) -o $@
test-rfl: all
	python3 $(ROOT)/rfl/tools/test_os.py --os fuse-x --native-build $(BUILD) --rfl $(RFL)
test: all
	python3 test_boot.py --executable $(BUILD)/fuse-x
	python3 test_session.py --executable $(BUILD)/fuse-x
	python3 test_executables.py --executable $(BUILD)/fuse-x --compiler $(RCC)
	python3 test_shell.py --executable $(BUILD)/fuse-x --compiler $(RCC)
	python3 $(ROOT)/ports/fuzix/tools/test_exec_permissions.py --os fuse-x --native-build $(BUILD)
	python3 test_prompt.py --executable $(BUILD)/fuse-x
	python3 $(ROOT)/ports/fuzix/tools/test_prompt_format.py
	python3 test_applications.py --executable $(BUILD)/fuse-x
	python3 $(ROOT)/ports/fuzix/tools/test_diff.py --os fuse-x --native-build $(BUILD)
	python3 $(ROOT)/ports/fuzix/tools/test_select.py --native --native-build $(BUILD)
	python3 $(ROOT)/ports/fuzix/tools/test_select.py --native --native-build $(BUILD) --local
	python3 $(ROOT)/ports/fuzix/tools/test_curses.py --native --native-build $(BUILD)
	python3 $(ROOT)/rfl/tools/test_os.py --os fuse-x --native-build $(BUILD) --rfl $(RFL)
	python3 $(ROOT)/ports/fuzix/tools/test_deadlines.py --native --native-build $(BUILD)
	python3 test_network.py --executable $(BUILD)/fuse-x --compiler $(RCC)
	python3 test_hostfs.py --executable $(BUILD)/fuse-x --compiler $(RCC)
	python3 test_console.py
	python3 test_console_paste.py --executable $(BUILD)/fuse-x
	python3 test_broker.py
	python3 test_console_broker.py
	python3 test_lifecycle.py --executable $(BUILD)/fuse-x --compiler $(RCC)
	python3 test_hosted_console.py --executable $(BUILD)/fuse-x

.PHONY: test-entropy
test-entropy: $(BUILD)/fuse-x $(BUILD)/apps/sh.fxe
	python3 cc.py $(ROOT)/ports/fuzix/ripple/entropycheck.c --compiler $(RCC) --build $(BUILD) -o $(BUILD)/apps/entropycheck.fxe
	python3 apps.py echo --compiler $(RCC) --build $(BUILD)
	python3 $(ROOT)/ports/fuzix/tools/test_entropy.py --native --native-build $(BUILD)

.PHONY: test-heap
test-heap: $(BUILD)/fuse-x $(BUILD)/apps/sh.fxe
	python3 apps.py echo --compiler $(RCC) --build $(BUILD)
	python3 $(ROOT)/ports/fuzix/tools/bench_foundations.py --os fuse-x --native-build $(BUILD) --source $(ROOT)/ports/fuzix/tools/heapedgecheck.c --repetitions 1 --output $(BUILD)/heap-check

# package.py caches each portable compiler stage independently.
.PHONY: compiler
all: compiler
compiler:
	python3 $(ROOT)/rcc-portable/package.py --os fuse-x --build $(BUILD)

$(BUILD)/apps/mux.fxe: $(ROOT)/ports/fuzix/ripple/mux.c $(wildcard $(ROOT)/ports/fuzix/ripple/mux/*) $(ROOT)/ports/graphics/terminal.c $(ROOT)/ports/graphics/terminal.h apps.py $(SDK_SOURCES) $(SDK_HEADERS) $(RCC)
	python3 apps.py mux --compiler $(RCC) --build $(BUILD)
.PHONY: test-mux
test-mux: $(BUILD)/fuse-x $(BUILD)/apps/mux.fxe $(BUILD)/apps/rift.fxe $(BUILD)/apps/rfl-fm.fxe $(BUILD)/apps/lynx.fxe $(BUILD)/apps/native-hello.fxe
	python3 apps.py ptysh sh cat echo sleep stty ps seq levee --compiler $(RCC) --build $(BUILD)
	python3 cc.py $(ROOT)/ports/fuzix/tools/muxcheck.c --compiler $(RCC) --build $(BUILD) -o $(BUILD)/apps/muxcheck.fxe
	python3 $(ROOT)/ports/fuzix/tools/test_mux.py --system fuse-x --native-build $(BUILD)

.PHONY: chat
chat: $(BUILD)/apps/chat.fxe
CHAT_SOURCES := $(wildcard $(ROOT)/ports/chat/*.[ch] $(ROOT)/ports/chat/build.py) $(wildcard $(ROOT)/ports/fuzix/ripple/curses/*.[ch] $(ROOT)/ports/fuzix/upstream/Library/libs/curses/*.[ch]) $(shell find $(ROOT)/ports/tls -type f \( -name '*.c' -o -name '*.h' -o -name '*.py' -o -name '*.json' \) ! -path '*/tests/*')
$(BUILD)/apps/chat.fxe: $(CHAT_SOURCES) $(SDK_SOURCES) $(SDK_HEADERS) $(RCC)
	@mkdir -p $(@D)
	python3 $(ROOT)/ports/chat/build.py --target native --compiler $(RCC) --build-dir $(BUILD) -o $@

.PHONY: test-kernel-io
test-kernel-io: $(BUILD)/fuse-x $(BUILD)/apps/sh.fxe
	python3 apps.py echo --compiler $(RCC) --build $(BUILD)
	python3 $(ROOT)/ports/fuzix/tools/test_kernel_io_unit.py
	python3 $(ROOT)/ports/fuzix/tools/test_kernel_io.py --native-build $(BUILD) --output $(BUILD)/kernel-io-check

.PHONY: test-pkill
test-pkill: $(BUILD)/fuse-x $(BUILD)/apps/sh.fxe
	python3 apps.py pkill echo --compiler $(RCC) --build $(BUILD)
	python3 $(ROOT)/ports/fuzix/tools/test_pkill.py --native --native-build $(BUILD)

$(BUILD)/generated/remotefs-node.c: hostfs.c remotefs.py
	@mkdir -p $(@D)
	python3 remotefs.py $< $@
$(BUILD)/port/remotefs-node.ir: $(BUILD)/generated/remotefs-node.c $(HEADERS) $(RCC)
	@$(RCC) compile $< $(FLAGS) --emit-ir > $@.tmp 2>$@.log || { cat $@.log >&2; exit 1; }
	@mv $@.tmp $@

$(BUILD)/port/remotefs.ir: $(ROOT)/ports/ssh/remotefs.h
$(BUILD)/apps/rfl-fm.fxe: $(ROOT)/ports/ssh/commander.c $(ROOT)/ports/ssh/remotefs.h
