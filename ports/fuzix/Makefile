.DEFAULT_GOAL := images
.DELETE_ON_ERROR:
.SECONDARY:

ROOT := $(abspath ../..)
PORT := $(abspath .)
BUILD ?= $(PORT)/build
K := $(PORT)/upstream/Kernel
R := $(ROOT)/runtime
ROS := $(ROOT)/c-test/examples/rOS
RCC := $(ROOT)/target/release/rcc
RFL ?= $(ROOT)/target/release/rfl
RFL_SOURCES := $(shell find $(ROOT)/rfl/std $(ROOT)/rfl/lib $(ROOT)/libraries/ini $(ROOT)/libraries/tui-theme -type f) $(ROOT)/rfl/os/posix.c $(ROOT)/rfl/tools/build_os.py
RASM := $(ROOT)/src/ripple-asm/target/release/rasm
RLINK := $(ROOT)/src/ripple-asm/target/release/rlink
# Retained only to diagnose older build commands; there is one supported profile.
MMU ?= 1
ifneq ($(MMU),1)
$(error Non-MMU FUZIX/Ripple is retired; remove MMU=$(MMU) and rebuild for RVM)
endif
BULK_IO ?= 1
CODE_BANKS ?= 1024
TEST_ENGINE ?= jit
FLAGS := -DRIPPLE_MMU=1 -DRIPPLE_BULK_IO=$(BULK_IO) -DRIPPLE_CODE_BANKS=$(CODE_BANKS) --bank-size 64000 -I$(PORT)/ripple -I$(K)/include -I$(K)/dev -I$(R)/include -I$(ROOT)/ports/graphics/include -DBANK_SIZE=64000
RUNTIME_HEADERS := $(addprefix $(R)/include/,$(addsuffix .h,stdint stddef stdarg string ctype stdlib mmio mmio_constants vm_devices vm_bulk irq stdio limits stdbool))
RUNTIME_HEADERS += $(addprefix $(R)/include/,$(addsuffix .h,net net_tcp vm_network))
U := $(PORT)/upstream/Library
USERFLAGS := -DRIPPLE_MMU=1 --bank-size 64000 -I$(PORT)/ripple/include -I$(U)/include -I$(R)/include -I$(PORT)/ripple -I$(ROOT)/ports/graphics/include -DNO_64BIT
USERHEADERS := $(BUILD)/.mmu $(shell find ripple/include $(U)/include -name '*.h') $(RUNTIME_HEADERS) $(K)/include/userstructs.h
LIBC_LOCAL := syscalls stdio format scan wait clock dirent misc heap pipe sockets resolver search identity
AWK_MATH_SRC := __sin __cos __rem_pio2 __rem_pio2_large scalbn fabs sqrt floor log exp pow sin cos atan atan2 strtod ecvt fmod frexp ldexp modf
LIBC_UPSTREAM := $(filter-out regerror malloc calloc realloc readline readline_fn clock_gettime clock_getres $(AWK_MATH_SRC),$(basename $(notdir $(wildcard upstream/Library/libs/*.c))))
LIBC_OBJS := $(addprefix $(BUILD)/libc/,$(addsuffix .pobj,$(LIBC_LOCAL))) $(addprefix $(BUILD)/ulib/,$(addsuffix .pobj,$(LIBC_UPSTREAM)))
.PHONY: rfl-sdk rfl-sdk-info
rfl-sdk: $(BUILD)/user/entry.pobj $(BUILD)/user/userlib.pobj $(LIBC_OBJS)
rfl-sdk-info:
	@python3 -c 'import json; print(json.dumps({"entry":"$(BUILD)/user/entry.pobj","libraries":"$(BUILD)/user/userlib.pobj $(if $(filter 1,$(RFL_CURSES)),$(CURSES_OBJS)) $(LIBC_OBJS)".split()}))'
APPS := basename dirname cat cp ls wc head tail grep fgrep sort uniq echo mkdir rmdir touch tee cmp chmod cut colrm expand pwd rm sync true false seq sleep kill tr od sum yes which env printenv chown chgrp du df sed ed xargs id whoami uname date mkfifo mknod comm rev test ps vile levee find dd diff diffh tar cowsay picol more less mktemp groups logname nohup fmt column cksum stty join split tsort tty nice time clear pr col man wump hamurabi expr awk make m4 dc
APPS += free uptime sok top ptysh
APPS += ifconfig ping telnet telnetd
APPS += htget dig echoping ntpdate host
APPS += bf
APPS += gterm
APPS += nxdraw
APPS += nano-X nanowm nxlaunch nxterm startx
APPS += nxeyes nxclock
APPS += lynx

CORE := start devio filesys inode kdata process mm syscall_exec syscall_fs syscall_fs2 syscall_fs3 syscall_other syscall_proc timer tty blk512 select
BULK_CONFIG := $(BUILD)/.bulk-io
MMU_CONFIG := $(BUILD)/.mmu
CODE_CONFIG := $(BUILD)/.code-banks
$(CODE_CONFIG): FORCE
	@mkdir -p $(@D)
	@if test "$$(cat $@ 2>/dev/null)" != "$(CODE_BANKS)"; then printf '%s\n' '$(CODE_BANKS)' > $@; fi
$(MMU_CONFIG): FORCE
	@mkdir -p $(@D)
	@if test "$$(cat $@ 2>/dev/null)" != "$(MMU)"; then printf '%s\n' '$(MMU)' > $@; fi
.PHONY: FORCE
FORCE:
$(BULK_CONFIG): FORCE
	@mkdir -p $(@D)
	@if test "$$(cat $@ 2>/dev/null)" != "$(BULK_IO)"; then printf '%s\n' '$(BULK_IO)' > $@; fi
HEADERS := $(ROOT)/ports/graphics/include/gx.h $(MMU_CONFIG) $(BULK_CONFIG) $(CODE_CONFIG) $(filter-out ripple/user.h,$(wildcard ripple/*.h)) $(wildcard $(K)/include/*.h) $(RUNTIME_HEADERS) $(ROS)/disk.h $(ROS)/fat16.h
USERHEADERS += ripple/select-abi.h ripple/clock-abi.h ripple/socket-abi.h ripple/mmu-profile.h ripple/resources.h ripple/pty-limits.h $(ROOT)/ports/graphics/include/gx.h
OBJS := $(addprefix $(BUILD)/kernel/,$(addsuffix .pobj,$(CORE)))
.PHONY: all kernel-objects clean
.PRECIOUS: $(BUILD)/kernel/%.asm
all: images
kernel-objects: $(OBJS)
# The MMU supplies bank-aware brk/sbrk in ripple/mmu.c. Keep the upstream
# definitions under unused names instead of changing the vendored syscall file.
$(BUILD)/kernel/%.asm: $(K)/%.c $(HEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@printf "  RCC %s\n" "$<"
	@$(RCC) compile $< -o $@ $(FLAGS) $(if $(filter syscall_proc,$*),-D_brk=ripple_legacy_brk -D_sbrk=ripple_legacy_sbrk -D_time=ripple_legacy_time) >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/kernel/%.pobj: $(BUILD)/kernel/%.asm $(RASM)
	@$(RASM) assemble $< -o $@ --bank-size 64000 --max-immediate 65535 >$@.log 2>&1 || { cat $@.log; exit 1; }
# Persistent disks and backups deliberately survive clean.
clean:
	rm -rf "$(BUILD)/kernel" "$(BUILD)/port" "$(BUILD)/rt" "$(BUILD)/ros" "$(BUILD)/user" "$(BUILD)/apps" "$(BUILD)/libc" "$(BUILD)/ulib" "$(BUILD)/sh" "$(BUILD)/levee" "$(BUILD)/awk" "$(BUILD)/math" "$(BUILD)/net" "$(BUILD)/curses" "$(BUILD)/nanox"
	rm -f "$(BUILD)/FUZIX.SYS" "$(BUILD)/bios.bin" "$(BUILD)/bios.bf" "$(BUILD)"/*.rxe "$(BUILD)/apps.json"

PORT_CORE := platform devices console jobcontrol pty usermem exec version sockets localsockets graphics mmu selectwait deadline clock
PORT_OBJS := $(addprefix $(BUILD)/port/,$(addsuffix .pobj,$(PORT_CORE)))
SHARED := string ctype stdlib mmio irq putchar puts vm_devices vm_bulk vm_network
RT_OBJS := $(addprefix $(BUILD)/rt/,$(addsuffix .pobj,$(SHARED)))
NET_SRC := core config wire ipv4 link arp history icmp icmp_error udp tcp tcp_wire tcp_ack tcp_payload tcp_stream tcp_poll tcp_receive tcp_decode tcp_error tcp_listen tcp_passive tcp_wait
NET_OBJS := $(addprefix $(BUILD)/net/,$(addsuffix .pobj,$(NET_SRC)))
$(BUILD)/net/%.asm: $(R)/net/%.c $(wildcard $(R)/net/*.h) $(RUNTIME_HEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@$(RCC) compile $< -o $@ --bank-size 64000 -I$(R)/include -DBANK_SIZE=64000 -DRIPPLE_FUZIX_NETWORK >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/net/%.pobj: $(BUILD)/net/%.asm $(RASM)
	@$(RASM) assemble $< -o $@ --bank-size 64000 --max-immediate 65535 >$@.log 2>&1 || { cat $@.log; exit 1; }
.PRECIOUS: $(BUILD)/net/%.asm
$(BUILD)/port/%.asm: ripple/%.c $(HEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@printf "  RCC %s\n" "$<"
	@$(RCC) compile $< -o $@ $(FLAGS) -I$(ROS) >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/rt/%.asm: $(R)/%.c $(RCC) Makefile $(HEADERS)
	@mkdir -p $(@D)
	@printf "  RCC %s\n" "$<"
	@$(RCC) compile $< -o $@ --bank-size 64000 -I$(R)/include -DBANK_SIZE=64000 -DRIPPLE_FUZIX_NETWORK >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/ros/%.asm: $(ROS)/%.c $(RCC) Makefile $(HEADERS)
	@mkdir -p $(@D)
	@printf "  RCC %s\n" "$<"
	@$(RCC) compile $< -o $@ --bank-size 64000 -I$(R)/include -I$(ROS) >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/%.pobj: $(BUILD)/%.asm $(RASM)
	@$(RASM) assemble $< -o $@ --bank-size 64000 --max-immediate 65535 >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/port/lowlevel.pobj: ripple/lowlevel.asm $(RASM) $(MMU_CONFIG) Makefile
	@mkdir -p $(@D)
	@$(RASM) assemble $< -o $@ --bank-size 64000 --max-immediate 65535 >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/kernel/memalloc_none.asm: $(K)/mm/memalloc_none.c $(HEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@printf "  RCC %s\n" "$<"
	@$(RCC) compile $< -o $@ $(FLAGS) >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/FUZIX.SYS: $(BUILD)/port/lowlevel.pobj $(OBJS) $(PORT_OBJS) $(RT_OBJS) $(NET_OBJS) $(BUILD)/kernel/usermem.pobj $(BUILD)/kernel/memalloc_none.pobj $(BUILD)/ros/disk.pobj $(BUILD)/ros/fat16.pobj $(RLINK)
	@$(RLINK) $(filter %.pobj,$^) -f sys --code-bank-base 1 --gp-bank 3 --sb-bank 32 --bank-size 64000 -o $@ >$(BUILD)/kernel-link.log 2>&1 || { cat $(BUILD)/kernel-link.log; exit 1; }
.PHONY: kernel
kernel: $(BUILD)/FUZIX.SYS
$(BUILD)/port/boot-entry.pobj: ripple/boot-entry.asm $(RASM)
	@mkdir -p $(@D)
	@$(RASM) assemble $< -o $@ --bank-size 64000 --max-immediate 65535 >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/bios.bin: $(BUILD)/port/boot-entry.pobj $(BUILD)/port/boot.pobj $(RT_OBJS) $(BUILD)/ros/disk.pobj $(BUILD)/ros/fat16.pobj $(RLINK)
	@$(RLINK) $(filter %.pobj,$^) -f binary --bank-size 64000 -o $@ >$(BUILD)/bios-link.log 2>&1 || { cat $(BUILD)/bios-link.log; exit 1; }

# Preserve an explicit failure for old commands, even if a stale export exists.
.PHONY: bf $(BUILD)/bios.bf
bf $(BUILD)/bios.bf:
	@echo "error: non-MMU FUZIX/RBF is retired; use images and run-fuzix.sh on RVM" >&2
	@exit 1

.PHONY: bios
bios: $(BUILD)/bios.bin
.PRECIOUS: $(BUILD)/port/%.asm $(BUILD)/rt/%.asm $(BUILD)/ros/%.asm
$(BUILD)/user/%.asm: ripple/%.c ripple/user.h $(USERHEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@printf "  RCC %s\n" "$<"
	@$(RCC) compile $< -o $@ $(USERFLAGS) >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/user/entry.pobj: ripple/user-entry.asm $(RASM) $(MMU_CONFIG) Makefile
	@mkdir -p $(@D)
	@$(RASM) assemble $< -o $@ --bank-size 64000 --max-immediate 65535 >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/%.rxe: $(BUILD)/user/entry.pobj $(BUILD)/user/%.pobj $(BUILD)/user/userlib.pobj $(LIBC_OBJS) $(RLINK)
	@$(RLINK) $(filter %.pobj,$^) -f rxe --bank-size 64000 -o $@ >$@.log 2>&1 || { cat $@.log; exit 1; }
.PHONY: userspace images
userspace: $(BUILD)/init.rxe $(BUILD)/hello.rxe $(BUILD)/forkcheck.rxe $(BUILD)/preemptcheck.rxe $(BUILD)/iotest.rxe $(BUILD)/libccheck.rxe $(BUILD)/mmucheck.rxe $(BUILD)/ttycheck.rxe
$(BUILD)/rfl-demo.rxe: $(ROOT)/rfl/examples/posix.rfl $(RFL_SOURCES) $(RFL) $(BUILD)/user/entry.pobj $(BUILD)/user/userlib.pobj $(LIBC_OBJS)
	$(RFL) build $< --os fuzix --build-dir $(BUILD) --compiler $(RCC) -o $@
userspace: $(BUILD)/rfl-demo.rxe
$(BUILD)/rfl-fm.rxe: $(wildcard $(ROOT)/rfl/apps/commander/*.rfl) $(RFL_SOURCES) $(wildcard $(ROOT)/rfl/os/*.c) $(RFL) $(BUILD)/user/entry.pobj $(BUILD)/user/userlib.pobj $(LIBC_OBJS) $(CURSES_OBJS)
	$(RFL) build $(ROOT)/rfl/apps/commander/main.rfl --os fuzix --build-dir $(BUILD) --compiler $(RCC) -o $@
userspace: $(BUILD)/rfl-fm.rxe
$(BUILD)/rift.rxe: $(wildcard $(ROOT)/rfl/apps/rift/*.rfl) $(RFL_SOURCES) $(wildcard $(ROOT)/libraries/text/* $(ROOT)/rfl/os/*.c) $(RFL) $(BUILD)/user/entry.pobj $(BUILD)/user/userlib.pobj $(LIBC_OBJS)
	$(RFL) build $(ROOT)/rfl/apps/rift/main.rfl --os fuzix --build-dir $(BUILD) --compiler $(RCC) -o $@
userspace: $(BUILD)/rift.rxe
.PHONY: test-rift
test-rift: kernel bios $(BUILD)/rift.rxe
	python3 $(ROOT)/rfl/tools/test_rift.py --os fuzix --fuzix-build $(BUILD) --rfl $(RFL)
$(BUILD)/mc.rxe: $(BUILD)/rfl-fm.rxe
	@cp $< $@
userspace: $(BUILD)/mc.rxe

.PHONY: test-rfl
test-rfl: images
	python3 $(ROOT)/rfl/tools/test_os.py --os fuzix --fuzix-build $(BUILD) --rfl $(RFL)
images: kernel bios userspace
.PRECIOUS: $(BUILD)/user/%.asm $(BUILD)/user/%.pobj

.PHONY: test-graphics
test-graphics: images $(BUILD)/gfxcheck.rxe
	python3 $(ROOT)/ports/graphics/tests/run.py
	python3 tools/test_graphics.py --build $(BUILD)

.PHONY: test-select
test-select: images
	python3 tools/test_select.py --build $(BUILD)
	python3 tools/test_select.py --build $(BUILD) --local

.PHONY: test-largefs
test-largefs: images
	python3 tools/test_largefs.py --build $(BUILD)

.PHONY: test
test: images
	python3 tools/test_launch.py
	python3 tools/test.py --build $(BUILD) --engine $(TEST_ENGINE)
	python3 tools/test_largefs.py --build $(BUILD)
	python3 tools/test_select.py --build $(BUILD)
	python3 tools/test_select.py --build $(BUILD) --local
	python3 tools/test_curses.py --build $(BUILD)
	python3 tools/test_deadlines.py --build $(BUILD)
	python3 tools/test_timers.py --build $(BUILD) --engine $(TEST_ENGINE)
	python3 tools/test_network.py --build $(BUILD) --engine $(TEST_ENGINE)
	python3 tools/test_nettools.py --build $(BUILD)
	node --test tools/disk.test.mjs tools/sokoban.test.mjs
	python3 tools/test_terminal.py --build $(BUILD) --engine $(TEST_ENGINE)
	python3 tools/test_unix.py --build $(BUILD) --engine $(TEST_ENGINE)
	python3 tools/test_editor.py --build $(BUILD) --engine $(TEST_ENGINE)
	python3 tools/test_interactive.py --build $(BUILD) --engine $(TEST_ENGINE)
	python3 tools/test_shell.py --build $(BUILD) --engine $(TEST_ENGINE)
	python3 tools/test_filetools.py --build $(BUILD) --engine $(TEST_ENGINE)
	python3 tools/test_texttools.py --build $(BUILD) --engine $(TEST_ENGINE)
	python3 tools/test_pagers.py --build $(BUILD) --engine $(TEST_ENGINE)
	python3 tools/test_fun.py --build $(BUILD) --engine $(TEST_ENGINE)
	python3 tools/test_manual_games.py --build $(BUILD) --engine $(TEST_ENGINE)
	python3 tools/test_devtools.py --build $(BUILD) --engine $(TEST_ENGINE)
	python3 tools/test_resources_games.py --build $(BUILD) --engine $(TEST_ENGINE)
	python3 tools/test_jobs.py --build $(BUILD) --engine $(TEST_ENGINE)
	python3 tools/test_code_banks.py --build $(BUILD) --engine $(TEST_ENGINE)
	python3 $(ROOT)/ports/nanox/tools/test_engine.py --build $(BUILD)

$(BUILD)/libc/%.asm: ripple/libc/%.c $(USERHEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@printf "  RCC %s\n" "$<"
	@$(RCC) compile $< -o $@ $(USERFLAGS) >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/ulib/%.asm: $(U)/libs/%.c $(USERHEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@printf "  RCC %s\n" "$<"
	@$(RCC) compile $< -o $@ $(USERFLAGS) >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/apps/%.asm: upstream/Applications/util/%.c $(USERHEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@printf "  RCC %s\n" "$<"
	@$(RCC) compile $< -o $@ $(USERFLAGS) -DUIDGID=0 $(if $(filter cut,$*),-Dwarn=cut_warn) $(if $(filter grep,$*),-Dregerror=grep_regerror) $(if $(filter vile,$*),-DRIPPLE) >$@.log 2>&1 || { cat $@.log; exit 1; }
$(addprefix $(BUILD)/,$(addsuffix .rxe,$(filter-out levee awk lynx nxdraw nano-X nanowm nxlaunch nxterm startx nxeyes nxclock,$(APPS)))): $(BUILD)/%.rxe: $(BUILD)/user/entry.pobj $(BUILD)/apps/%.pobj $(BUILD)/user/userlib.pobj $(LIBC_OBJS) $(RLINK)
	@$(RLINK) $(filter %.pobj,$^) -f rxe --bank-size 64000 -o $@ >$@.log 2>&1 || { cat $@.log; exit 1; }
userspace: $(addprefix $(BUILD)/,$(addsuffix .rxe,$(APPS)))

$(BUILD)/apps/test.asm: upstream/Applications/MWC/cmd/test.c $(USERHEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@$(RCC) compile $< -o $@ $(USERFLAGS) >$@.log 2>&1 || { cat $@.log; exit 1; }
$(addprefix $(BUILD)/apps/,$(addsuffix .asm,comm rev join split tsort tty nice time clear pr col)): $(BUILD)/apps/%.asm: upstream/Applications/V7/cmd/%.c $(USERHEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@$(RCC) compile $< -o $@ $(USERFLAGS) >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/mv.rxe $(BUILD)/ln.rxe: $(BUILD)/cp.rxe
	@cp $< $@
$(BUILD)/apps.json: Makefile
	@mkdir -p $(@D)
	@python3 -c 'import json; print(json.dumps("init hello forkcheck preemptcheck iotest libccheck mmucheck ttycheck rfl-demo rfl-fm mc rift $(APPS) mv ln sh vi tcl tclsh sokoban nslookup".split()))' >$@
userspace: $(BUILD)/mv.rxe $(BUILD)/ln.rxe $(BUILD)/sh.rxe $(BUILD)/vi.rxe $(BUILD)/tcl.rxe $(BUILD)/tclsh.rxe $(BUILD)/apps.json
userspace: $(BUILD)/sokoban.rxe
userspace: $(BUILD)/nslookup.rxe
$(BUILD)/nslookup.rxe: $(BUILD)/host.rxe
	@cp $< $@
$(BUILD)/sokoban.rxe: $(BUILD)/sok.rxe
	@cp $< $@

SHELL_SRC := args blok builtin cmd ctype error expand fault glob io macro main msg name print service setbrk stak string word xec
.PHONY: test-prompt-format
test-prompt-format:
	python3 tools/test_prompt_format.py

SHELL_DIR := upstream/Applications/V7/cmd/sh
SHELL_OBJS := $(addprefix $(BUILD)/sh/,$(addsuffix .pobj,$(SHELL_SRC)))
$(BUILD)/sh/main.asm $(BUILD)/sh/input.asm: ripple/shell-prompt.h
$(BUILD)/sh/%.asm: $(SHELL_DIR)/%.c $(wildcard $(SHELL_DIR)/*.h) $(USERHEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@printf "  RCC %s\n" "$<"
	@$(RCC) compile $< -o $@ $(USERFLAGS) -DBUILD_FSH >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/sh.rxe: $(BUILD)/user/entry.pobj $(SHELL_OBJS) $(BUILD)/sh/input.pobj $(BUILD)/sh/jobs.pobj $(BUILD)/sh/prompt.pobj $(BUILD)/user/userlib.pobj $(LIBC_OBJS) $(RLINK)
	@$(RLINK) $(filter %.pobj,$^) -f rxe --bank-size 64000 -o $@ >$@.log 2>&1 || { cat $@.log; exit 1; }
.PRECIOUS: $(BUILD)/sh/%.asm

$(BUILD)/vi.rxe: $(BUILD)/levee.rxe
	@cp $< $@

$(BUILD)/apps/find.asm: upstream/Applications/MWC/cmd/find.c upstream/Applications/MWC/cmd/findnode.h $(USERHEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@$(RCC) compile $< -o $@ $(USERFLAGS) >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/apps/diff.asm $(BUILD)/apps/diffh.asm: $(BUILD)/apps/%.asm: upstream/Applications/V7/cmd/%.c $(USERHEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@$(RCC) compile $< -o $@ $(USERFLAGS) >$@.log 2>&1 || { cat $@.log; exit 1; }

upstream/Applications/MWC/cmd/find.c: upstream/Applications/MWC/cmd/find.y ripple/find-parser.inc tools/generate_find.py
	python3 tools/generate_find.py

LEVEE_DIR := upstream/Applications/levee
LEVEE_SRC := blockio display editcor exec find globals insert main misc modify move ucsd undo unixcall wildargs
LEVEE_OBJS := $(addprefix $(BUILD)/levee/,$(addsuffix .pobj,$(LEVEE_SRC)))
$(BUILD)/levee/%.asm: $(LEVEE_DIR)/%.c $(wildcard $(LEVEE_DIR)/*.h) $(USERHEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@printf "  RCC %s\n" "$<"
	@$(RCC) compile $< -o $@ $(USERFLAGS) -DANSI=1 -DVT52=0 -DSIZE=30000 -DRIPPLE >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/levee.rxe: $(BUILD)/user/entry.pobj $(LEVEE_OBJS) $(BUILD)/user/userlib.pobj $(LIBC_OBJS) $(RLINK)
	@$(RLINK) $(filter %.pobj,$^) -f rxe --bank-size 64000 -o $@ >$@.log 2>&1 || { cat $@.log; exit 1; }
.PRECIOUS: $(BUILD)/levee/%.asm

$(BUILD)/sh/input.asm: ripple/shell-input.c $(wildcard $(SHELL_DIR)/*.h) $(USERHEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@$(RCC) compile $< -o $@ $(USERFLAGS) -I$(SHELL_DIR) -DBUILD_FSH >$@.log 2>&1 || { cat $@.log; exit 1; }

$(BUILD)/sh/prompt.asm: ripple/shell-prompt.c ripple/shell-prompt.h $(USERHEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@$(RCC) compile $< -o $@ $(USERFLAGS) >$@.log 2>&1 || { cat $@.log; exit 1; }

$(addprefix $(BUILD)/apps/,$(addsuffix .asm,cowsay hamurabi sok)): $(BUILD)/apps/%.asm: upstream/Applications/games/%.c $(USERHEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@$(RCC) compile $< -o $@ $(USERFLAGS) >$@.log 2>&1 || { cat $@.log; exit 1; }

$(BUILD)/apps/sok.asm: upstream/Applications/games/sok.h
$(BUILD)/apps/wump.asm: upstream/Applications/V7/games/wump.c $(USERHEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@printf "  RCC %s\n" "$<"
	@$(RCC) compile $< -o $@ $(USERFLAGS) >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/apps/picol.asm: upstream/Applications/TCL/picol.c ripple/picol-extra.inc $(USERHEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@$(RCC) compile $< -o $@ $(USERFLAGS) >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/tcl.rxe $(BUILD)/tclsh.rxe: $(BUILD)/picol.rxe
	@cp $< $@

$(addprefix $(BUILD)/apps/,$(addsuffix .asm,expr make m4)): $(BUILD)/apps/%.asm: upstream/Applications/MWC/cmd/%.c upstream/Applications/MWC/cmd/make.h $(USERHEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@printf "  RCC %s\n" "$<"
	@$(RCC) compile $< -o $@ $(USERFLAGS) >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/apps/dc.asm: upstream/Applications/V7/cmd/dc.c $(USERHEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@$(RCC) compile $< -o $@ $(USERFLAGS) >$@.log 2>&1 || { cat $@.log; exit 1; }

AWK_SRC := awkgram.tab b main parse proctab tran lib run lex
AWKFLAGS := $(filter-out -DNO_64BIT,$(USERFLAGS)) -I$(PORT)/awk
FLOAT_LIB_OBJS := $(addprefix $(BUILD)/awk/,$(addsuffix .pobj,format softfloat math-bits)) $(addprefix $(BUILD)/math/,$(addsuffix .pobj,$(AWK_MATH_SRC)))
AWK_OBJS := $(addprefix $(BUILD)/awk/,$(addsuffix .pobj,$(AWK_SRC) support)) $(FLOAT_LIB_OBJS)
.PHONY: float-sdk float-sdk-info
float-sdk: $(FLOAT_LIB_OBJS)
float-sdk-info:
	@python3 -c 'import json; print(json.dumps({"libraries":"$(FLOAT_LIB_OBJS)".split()}))'
$(BUILD)/awk/%.asm: awk/%.c $(wildcard awk/*.h) $(USERHEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@printf "  RCC %s\n" "$<"
	@$(RCC) compile $< -o $@ $(AWKFLAGS) >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/math/%.asm: upstream/Library/libs/%.c upstream/Library/libs/libm.h $(USERHEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@printf "  RCC %s\n" "$<"
	@$(RCC) compile $< -o $@ $(AWKFLAGS) >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/awk/support.asm: ripple/awk-support.c $(USERHEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@$(RCC) compile $< -o $@ $(AWKFLAGS) >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/awk/math-bits.asm: ripple/libc/math-bits.c $(USERHEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@$(RCC) compile $< -o $@ $(AWKFLAGS) >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/awk/format.asm: ripple/libc/format.c ripple/float-format.inc $(USERHEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@$(RCC) compile $< -o $@ $(AWKFLAGS) -DRIPPLE_FLOAT_FORMAT >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/awk/softfloat.asm: $(R)/softfloat.c $(USERHEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@$(RCC) compile $< -o $@ $(AWKFLAGS) >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/awk.rxe: $(BUILD)/user/entry.pobj $(AWK_OBJS) $(BUILD)/user/userlib.pobj $(filter-out $(BUILD)/libc/format.pobj,$(LIBC_OBJS)) $(RLINK)
	@$(RLINK) $(filter %.pobj,$^) -f rxe --bank-size 64000 -o $@ >$@.log 2>&1 || { cat $@.log; exit 1; }
.PRECIOUS: $(BUILD)/awk/%.asm $(BUILD)/math/%.asm

$(BUILD)/sh/jobs.asm: ripple/shell-jobs.c $(wildcard $(SHELL_DIR)/*.h) $(USERHEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@$(RCC) compile $< -o $@ $(USERFLAGS) -I$(SHELL_DIR) -DBUILD_FSH >$@.log 2>&1 || { cat $@.log; exit 1; }

$(addprefix $(BUILD)/apps/,$(addsuffix .asm,ps top ptysh)): $(BUILD)/apps/%.asm: ripple/%.c ripple/process-report.h $(USERHEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@$(RCC) compile $< -o $@ $(USERFLAGS) >$@.log 2>&1 || { cat $@.log; exit 1; }

$(addprefix $(BUILD)/apps/,$(addsuffix .asm,ifconfig ping telnet telnetd)): $(BUILD)/apps/%.asm: ripple/%.c ripple/telnet-protocol.h $(USERHEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@$(RCC) compile $< -o $@ $(USERFLAGS) >$@.log 2>&1 || { cat $@.log; exit 1; }

$(addprefix $(BUILD)/apps/,$(addsuffix .asm,htget dig echoping ntpdate)): $(BUILD)/apps/%.asm: upstream/Applications/netd/%.c $(USERHEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@$(RCC) compile $< -o $@ $(USERFLAGS) >$@.log 2>&1 || { cat $@.log; exit 1; }
$(addprefix $(BUILD)/apps/,$(addsuffix .asm,host bf gterm)): $(BUILD)/apps/%.asm: ripple/%.c $(USERHEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@$(RCC) compile $< -o $@ $(USERFLAGS) >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/apps/gterm.asm: $(ROOT)/ports/graphics/terminal.c $(ROOT)/ports/graphics/terminal.h $(ROOT)/ports/graphics/fonts/rom8x16.inc

# Opt-in integration probe; not installed on normal disks.
$(BUILD)/gfxcheck.rxe: $(BUILD)/user/entry.pobj $(BUILD)/user/gfxcheck.pobj $(BUILD)/user/userlib.pobj $(LIBC_OBJS) $(RLINK)

# Shared C16 curses; only consumers link it. Keep terminal input and allocation
# adaptations separate from the imported screen-update algorithms.
CURSES_DIR := $(U)/libs/curses
CURSES_UPSTREAM := $(filter-out initscr endwin newwin windel wgetch wgetstr doupdt refresh winscrol,$(basename $(notdir $(wildcard $(CURSES_DIR)/*.c))))
CURSES_OBJS := $(addprefix $(BUILD)/curses/,$(addsuffix .pobj,$(CURSES_UPSTREAM))) $(addprefix $(BUILD)/curses/local/,$(addsuffix .pobj,input window screen color render cells))
$(BUILD)/rfl-fm.rxe $(BUILD)/rift.rxe: $(CURSES_OBJS)
ifeq ($(RFL_CURSES),1)
rfl-sdk: $(CURSES_OBJS)
endif
$(BUILD)/curses/%.asm: $(CURSES_DIR)/%.c $(CURSES_DIR)/curspriv.h $(USERHEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@$(RCC) compile $< -o $@ $(USERFLAGS) -I$(CURSES_DIR) >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/curses/local/%.asm: ripple/curses/%.c $(CURSES_DIR)/curspriv.h $(USERHEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@$(RCC) compile $< -o $@ $(USERFLAGS) -I$(CURSES_DIR) >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/user/cursescheck.asm: tools/cursescheck.c $(USERHEADERS) $(RCC)
	@mkdir -p $(@D)
	@$(RCC) compile $< -o $@ $(USERFLAGS) >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/cursescheck.rxe: $(BUILD)/user/entry.pobj $(BUILD)/user/cursescheck.pobj $(BUILD)/user/userlib.pobj $(CURSES_OBJS) $(LIBC_OBJS) $(RLINK)
.PRECIOUS: $(BUILD)/curses/%.asm $(BUILD)/curses/local/%.asm

include $(ROOT)/ports/nanox/engine.mk

.PHONY: test-curses
test-curses: images $(BUILD)/cursescheck.rxe
	python3 tools/test_curses.py --build $(BUILD)

# Fine deadline integration probe; standalone queue tests are part of the app.
$(BUILD)/user/finecheck.asm: tools/finecheck.c ripple/deadline.c ripple/deadline.h $(USERHEADERS) $(RCC)
	@mkdir -p $(@D)
	@$(RCC) compile $< -o $@ $(USERFLAGS) >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/finecheck.rxe: $(BUILD)/user/entry.pobj $(BUILD)/user/finecheck.pobj $(BUILD)/user/userlib.pobj $(LIBC_OBJS) $(RLINK)

.PHONY: test-deadlines
test-deadlines: images $(BUILD)/finecheck.rxe
	python3 tools/test_deadlines.py --build $(BUILD)

# Opt-in large-static-data probe, shared with the native executable tests.
$(BUILD)/user/globalcheck.asm: $(ROOT)/fuse-x/examples/global-banks.c $(USERHEADERS) $(RCC)
	@mkdir -p $(@D)
	@$(RCC) compile $< -o $@ $(USERFLAGS) >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/globalcheck.rxe: $(BUILD)/user/entry.pobj $(BUILD)/user/globalcheck.pobj $(BUILD)/user/userlib.pobj $(LIBC_OBJS) $(RLINK)

.PHONY: test-global-banks
test-global-banks: kernel bios $(BUILD)/init.rxe $(BUILD)/sh.rxe $(BUILD)/echo.rxe $(BUILD)/globalcheck.rxe
	python3 tools/test_global_banks.py --build $(BUILD)

# Optional configuration companions. Kept out of libc and core curses.
CONFIG_OBJS := $(BUILD)/config-ini.pobj $(BUILD)/config-theme.pobj $(BUILD)/config-curses.pobj
CONFIG_HEADERS := $(ROOT)/libraries/ini/ini.h $(ROOT)/libraries/tui-theme/tui_theme.h $(ROOT)/libraries/tui-theme/tui_curses.h
.PHONY: config-sdk
config-sdk: $(CONFIG_OBJS)
$(BUILD)/config-ini.pobj: CONFIG_SOURCE := $(ROOT)/libraries/ini/ini.c
$(BUILD)/config-ini.pobj: $(ROOT)/libraries/ini/ini.c
$(BUILD)/config-theme.pobj: CONFIG_SOURCE := $(ROOT)/libraries/tui-theme/tui_theme.c
$(BUILD)/config-theme.pobj: $(ROOT)/libraries/tui-theme/tui_theme.c
$(BUILD)/config-curses.pobj: CONFIG_SOURCE := $(ROOT)/libraries/tui-theme/tui_curses.c
$(BUILD)/config-curses.pobj: $(ROOT)/libraries/tui-theme/tui_curses.c
$(CONFIG_OBJS): Makefile $(CONFIG_HEADERS) $(USERHEADERS) $(RCC) $(RASM)
	@mkdir -p $(@D)
	$(RCC) compile $(CONFIG_SOURCE) $(USERFLAGS) -o $@.asm
	$(RASM) assemble $@.asm -o $@ --bank-size 64000 --max-immediate 65535
$(BUILD)/cursescheck.rxe: $(CONFIG_OBJS)

# Full browser port; normal userspace and apps.json include it on both hosts.
LYNX := $(ROOT)/ports/lynx
LYNX_SOURCES := $(wildcard $(LYNX)/*.py $(LYNX)/*.c $(LYNX)/config/* $(LYNX)/vendor/*)
$(BUILD)/lynx.rxe: $(LYNX_SOURCES) $(USERHEADERS) $(RCC) $(ROOT)/target/release/rcpp $(RASM) $(RLINK) $(BUILD)/user/entry.pobj $(BUILD)/user/userlib.pobj $(LIBC_OBJS) $(CURSES_OBJS) $(FLOAT_LIB_OBJS)
	python3 $(LYNX)/build.py --target ripple --compiler $(RCC) --build-dir $(BUILD) -o $@
