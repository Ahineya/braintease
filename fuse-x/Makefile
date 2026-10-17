.DEFAULT_GOAL := all
.DELETE_ON_ERROR:
ROOT := $(abspath ..)
BUILD ?= $(CURDIR)/build
RCC ?= $(ROOT)/target/release/rcc
K := $(ROOT)/ports/fuzix/upstream/Kernel
# Select the shared wide userspace ABI. Address spaces are implemented by the
# native hosted platform, independent of Ripple's hardware MMU implementation.
FLAGS := -DRIPPLE_MMU=1 --target native --bank-size 64000 -DRIPPLE_BULK_IO=0 -DRIPPLE_MMIO_NO_INLINE=1 -I$(CURDIR)/include -I$(ROOT)/ports/fuzix/ripple -I$(K)/include -I$(K)/dev -I$(ROOT)/runtime/include
CORE := start devio filesys inode kdata process mm syscall_exec syscall_fs syscall_fs2 syscall_fs3 syscall_other syscall_proc timer tty blk512 usermem
NET_SRC := core config wire ipv4 link arp history icmp icmp_error udp tcp tcp_wire tcp_ack tcp_payload tcp_stream tcp_poll tcp_receive tcp_decode tcp_error tcp_listen tcp_passive tcp_wait
UNITS := $(addprefix $(BUILD)/kernel/,$(addsuffix .ir,$(CORE))) $(BUILD)/kernel/memalloc_none.ir $(BUILD)/port/platform.ir $(BUILD)/port/devices.ir $(BUILD)/port/sysdev.ir $(BUILD)/port/check.ir $(BUILD)/port/memory.ir $(BUILD)/port/exec.ir $(BUILD)/port/jobcontrol.ir $(BUILD)/port/pty.ir $(BUILD)/port/version.ir $(BUILD)/runtime/vm_devices.ir $(BUILD)/runtime/vm_network.ir $(BUILD)/port/sockets.ir $(addprefix $(BUILD)/runtime/net/,$(addsuffix .ir,$(NET_SRC)))
UNITS += $(BUILD)/port/hostfs.ir
HOSTFS_UNITS := filesys inode devio syscall_fs3 syscall_other syscall_fs2
APPS := $(addprefix $(BUILD)/port/,$(addsuffix .ir,userlib init shell process-check))
HEADERS := $(wildcard include/*.h $(K)/include/*.h $(ROOT)/ports/fuzix/ripple/*.h $(ROOT)/runtime/include/*.h)
FLAGS += -I$(ROOT)/ports/graphics/include
HEADERS += $(ROOT)/ports/graphics/include/gx.h
.PHONY: all tools test
APP_NAMES := native-hello cat ls cp sh
EXECUTABLES := $(addprefix $(BUILD)/apps/,$(addsuffix .fxe,$(APP_NAMES)))
SDK_SOURCES := $(wildcard $(ROOT)/ports/fuzix/ripple/libc/*.c $(ROOT)/ports/fuzix/upstream/Library/libs/*.c) $(ROOT)/ports/fuzix/ripple/userlib.c libc/bridge.c cc.py
SDK_HEADERS := $(K)/include/userstructs.h $(shell find $(ROOT)/ports/fuzix/ripple/include $(ROOT)/ports/fuzix/upstream/Library/include $(ROOT)/runtime/include -name '*.h')
all: $(BUILD)/fuse-x $(EXECUTABLES) applications
tools:
	cd $(ROOT) && cargo build --release -p rcc-driver -p rcc-native-runtime
$(BUILD)/kernel/%.ir: $(K)/%.c $(HEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@printf '  RCC %s\n' '$<'
	@$(RCC) compile $< $(FLAGS) $(if $(filter syscall_proc,$*),-D_brk=fuse_legacy_brk -D_sbrk=fuse_legacy_sbrk -D_pause=fuse_legacy_pause) --emit-ir > $@.tmp 2>$@.log || { cat $@.log >&2; exit 1; }
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
$(BUILD)/fuse-x: $(UNITS) $(APPS) $(ROOT)/target/release/librcc_native_runtime.a
	@printf '  LINK %s\n' '$@'
	@$(RCC) link-native $(addprefix --kernel ,$(UNITS)) $(APPS) -o $@ >$(BUILD)/link.log 2>&1 || { cat $(BUILD)/link.log >&2; exit 1; }
$(BUILD)/apps/native-hello.fxe: examples/hello.c $(SDK_SOURCES) $(SDK_HEADERS) $(RCC)
	@mkdir -p $(@D)
	python3 cc.py $< --compiler $(RCC) --build $(BUILD) -o $@
$(BUILD)/apps/sh.fxe: $(wildcard $(ROOT)/ports/fuzix/upstream/Applications/V7/cmd/sh/*.[ch]) $(ROOT)/ports/fuzix/ripple/shell-input.c $(ROOT)/ports/fuzix/ripple/shell-jobs.c apps.py $(SDK_SOURCES) $(SDK_HEADERS) $(RCC)
	python3 apps.py sh --compiler $(RCC) --build $(BUILD)
.PHONY: applications
applications: $(EXECUTABLES)
	python3 apps.py --all --compiler $(RCC) --build $(BUILD)
$(BUILD)/apps/%.fxe: $(ROOT)/ports/fuzix/upstream/Applications/util/%.c $(SDK_SOURCES) $(SDK_HEADERS) $(RCC)
	@mkdir -p $(@D)
	python3 cc.py $< $(if $(filter ls,$*),-DUIDGID=0) --compiler $(RCC) --build $(BUILD) -o $@
test: all
	python3 test_boot.py --executable $(BUILD)/fuse-x
	python3 test_session.py --executable $(BUILD)/fuse-x
	python3 test_executables.py --executable $(BUILD)/fuse-x --compiler $(RCC)
	python3 test_shell.py --executable $(BUILD)/fuse-x --compiler $(RCC)
	python3 test_applications.py --executable $(BUILD)/fuse-x
	python3 test_network.py --executable $(BUILD)/fuse-x --compiler $(RCC)
	python3 test_hostfs.py --executable $(BUILD)/fuse-x --compiler $(RCC)
	python3 test_console.py
	python3 test_broker.py
	python3 test_lifecycle.py --executable $(BUILD)/fuse-x --compiler $(RCC)
	python3 test_hosted_console.py --executable $(BUILD)/fuse-x
