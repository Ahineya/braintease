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
RASM := $(ROOT)/src/ripple-asm/target/release/rasm
RLINK := $(ROOT)/src/ripple-asm/target/release/rlink
FLAGS := --bank-size 64000 -I$(PORT)/ripple -I$(K)/include -I$(K)/dev -I$(R)/include -DBANK_SIZE=64000
RUNTIME_HEADERS := $(addprefix $(R)/include/,$(addsuffix .h,stdint stddef stdarg string ctype stdlib mmio mmio_constants irq stdio limits stdbool))
U := $(PORT)/upstream/Library
USERFLAGS := --bank-size 64000 -I$(PORT)/ripple/include -I$(U)/include -I$(R)/include -I$(PORT)/ripple -DNO_64BIT
USERHEADERS := $(shell find ripple/include $(U)/include -name '*.h') $(RUNTIME_HEADERS) $(K)/include/userstructs.h
LIBC_LOCAL := syscalls stdio format dirent misc heap
LIBC_UPSTREAM := $(filter-out regerror malloc calloc realloc readline readline_fn,$(basename $(notdir $(wildcard upstream/Library/libs/*.c))))
LIBC_OBJS := $(addprefix $(BUILD)/libc/,$(addsuffix .pobj,$(LIBC_LOCAL))) $(addprefix $(BUILD)/ulib/,$(addsuffix .pobj,$(LIBC_UPSTREAM)))
APPS := basename dirname cat cp ls wc head tail grep fgrep sort uniq echo mkdir rmdir touch tee cmp chmod cut colrm expand pwd rm sync true false seq sleep kill tr od sum yes which env printenv chown chgrp du df sed ed xargs id whoami uname date mkfifo mknod comm rev test ps vile levee find dd diff diffh tar cowsay picol

CORE := start devio filesys inode kdata process mm syscall_exec syscall_fs syscall_fs2 syscall_fs3 syscall_other syscall_proc timer tty blk512
HEADERS := $(filter-out ripple/user.h,$(wildcard ripple/*.h)) $(wildcard $(K)/include/*.h) $(RUNTIME_HEADERS) $(ROS)/disk.h $(ROS)/fat16.h
OBJS := $(addprefix $(BUILD)/kernel/,$(addsuffix .pobj,$(CORE)))
.PHONY: all kernel-objects clean
.PRECIOUS: $(BUILD)/kernel/%.asm
all: images
kernel-objects: $(OBJS)
$(BUILD)/kernel/%.asm: $(K)/%.c $(HEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@printf "  RCC %s\n" "$<"
	@$(RCC) compile $< -o $@ $(FLAGS) >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/kernel/%.pobj: $(BUILD)/kernel/%.asm $(RASM)
	@$(RASM) assemble $< -o $@ --bank-size 64000 --max-immediate 65535 >$@.log 2>&1 || { cat $@.log; exit 1; }
# Persistent disks and backups deliberately survive clean.
clean:
	rm -rf "$(BUILD)/kernel" "$(BUILD)/port" "$(BUILD)/rt" "$(BUILD)/ros" "$(BUILD)/user" "$(BUILD)/apps" "$(BUILD)/libc" "$(BUILD)/ulib" "$(BUILD)/sh" "$(BUILD)/levee"
	rm -f "$(BUILD)/FUZIX.SYS" "$(BUILD)/bios.bin" "$(BUILD)/bios.bf" "$(BUILD)"/*.rxe "$(BUILD)/apps.json"

PORT_CORE := platform devices usermem exec version
PORT_OBJS := $(addprefix $(BUILD)/port/,$(addsuffix .pobj,$(PORT_CORE)))
SHARED := string ctype stdlib mmio irq putchar puts
RT_OBJS := $(addprefix $(BUILD)/rt/,$(addsuffix .pobj,$(SHARED)))
$(BUILD)/port/%.asm: ripple/%.c $(HEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@printf "  RCC %s\n" "$<"
	@$(RCC) compile $< -o $@ $(FLAGS) -I$(ROS) >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/rt/%.asm: $(R)/%.c $(RCC) Makefile $(HEADERS)
	@mkdir -p $(@D)
	@printf "  RCC %s\n" "$<"
	@$(RCC) compile $< -o $@ --bank-size 64000 -I$(R)/include -DBANK_SIZE=64000 >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/ros/%.asm: $(ROS)/%.c $(RCC) Makefile $(HEADERS)
	@mkdir -p $(@D)
	@printf "  RCC %s\n" "$<"
	@$(RCC) compile $< -o $@ --bank-size 64000 -I$(R)/include -I$(ROS) >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/%.pobj: $(BUILD)/%.asm $(RASM)
	@$(RASM) assemble $< -o $@ --bank-size 64000 --max-immediate 65535 >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/port/lowlevel.pobj: ripple/lowlevel.asm $(RASM)
	@mkdir -p $(@D)
	@$(RASM) assemble $< -o $@ --bank-size 64000 --max-immediate 65535 >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/kernel/memalloc_none.asm: $(K)/mm/memalloc_none.c $(HEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@printf "  RCC %s\n" "$<"
	@$(RCC) compile $< -o $@ $(FLAGS) >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/FUZIX.SYS: $(BUILD)/port/lowlevel.pobj $(OBJS) $(PORT_OBJS) $(RT_OBJS) $(BUILD)/kernel/usermem.pobj $(BUILD)/kernel/memalloc_none.pobj $(BUILD)/ros/disk.pobj $(BUILD)/ros/fat16.pobj
	@$(RLINK) $^ -f sys --code-bank-base 1 --gp-bank 3 --sb-bank 32 --bank-size 64000 -o $@ >$(BUILD)/kernel-link.log 2>&1 || { cat $(BUILD)/kernel-link.log; exit 1; }
.PHONY: kernel
kernel: $(BUILD)/FUZIX.SYS
$(BUILD)/port/boot-entry.pobj: ripple/boot-entry.asm $(RASM)
	@mkdir -p $(@D)
	@$(RASM) assemble $< -o $@ --bank-size 64000 --max-immediate 65535 >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/bios.bin: $(BUILD)/port/boot-entry.pobj $(BUILD)/port/boot.pobj $(RT_OBJS) $(BUILD)/ros/disk.pobj $(BUILD)/ros/fat16.pobj
	@$(RLINK) $^ -f binary --bank-size 64000 -o $@ >$(BUILD)/bios-link.log 2>&1 || { cat $(BUILD)/bios-link.log; exit 1; }

# Dynamic kernel/user code is loaded from the same disk as the native VM.
# Eight process slots plus one spare exec slot occupy code banks 32..67.
$(BUILD)/bios.bf: $(BUILD)/port/boot-entry.pobj $(BUILD)/port/boot.pobj $(RT_OBJS) $(BUILD)/ros/disk.pobj $(BUILD)/ros/fat16.pobj $(RLINK)
	@$(RLINK) $(filter %.pobj,$^) -f bf --standalone --bf-host --bf-irq --bank-size 64000 --memory-banks 80 --code-banks 68 -o $@ >$(BUILD)/bios-bf-link.log 2>&1 || { cat $(BUILD)/bios-bf-link.log; exit 1; }
.PHONY: bf
bf: $(BUILD)/bios.bf

.PHONY: bios
bios: $(BUILD)/bios.bin
.PRECIOUS: $(BUILD)/port/%.asm $(BUILD)/rt/%.asm $(BUILD)/ros/%.asm
$(BUILD)/user/%.asm: ripple/%.c ripple/user.h $(USERHEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@printf "  RCC %s\n" "$<"
	@$(RCC) compile $< -o $@ $(USERFLAGS) >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/user/entry.pobj: ripple/user-entry.asm $(RASM)
	@mkdir -p $(@D)
	@$(RASM) assemble $< -o $@ --bank-size 64000 --max-immediate 65535 >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/%.rxe: $(BUILD)/user/entry.pobj $(BUILD)/user/%.pobj $(BUILD)/user/userlib.pobj $(LIBC_OBJS)
	@$(RLINK) $^ -f rxe --bank-size 64000 -o $@ >$@.log 2>&1 || { cat $@.log; exit 1; }
.PHONY: userspace images
userspace: $(BUILD)/init.rxe $(BUILD)/hello.rxe $(BUILD)/forkcheck.rxe $(BUILD)/preemptcheck.rxe $(BUILD)/iotest.rxe $(BUILD)/libccheck.rxe
images: kernel bios userspace
.PRECIOUS: $(BUILD)/user/%.asm $(BUILD)/user/%.pobj

.PHONY: test
test: images
	python3 tools/test_launch.py
	python3 tools/test.py --build $(BUILD)
	node --test tools/disk.test.mjs
	python3 tools/test_terminal.py --build $(BUILD)
	python3 tools/test_unix.py --build $(BUILD)
	python3 tools/test_editor.py --build $(BUILD)
	python3 tools/test_interactive.py --build $(BUILD)
	python3 tools/test_shell.py --build $(BUILD)
	python3 tools/test_filetools.py --build $(BUILD)
	python3 tools/test_fun.py --build $(BUILD)

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
$(addprefix $(BUILD)/,$(addsuffix .rxe,$(filter-out levee,$(APPS)))): $(BUILD)/%.rxe: $(BUILD)/user/entry.pobj $(BUILD)/apps/%.pobj $(BUILD)/user/userlib.pobj $(LIBC_OBJS)
	@$(RLINK) $^ -f rxe --bank-size 64000 -o $@ >$@.log 2>&1 || { cat $@.log; exit 1; }
userspace: $(addprefix $(BUILD)/,$(addsuffix .rxe,$(APPS)))

$(BUILD)/apps/test.asm: upstream/Applications/MWC/cmd/test.c $(USERHEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@$(RCC) compile $< -o $@ $(USERFLAGS) >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/apps/comm.asm $(BUILD)/apps/rev.asm: $(BUILD)/apps/%.asm: upstream/Applications/V7/cmd/%.c $(USERHEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@$(RCC) compile $< -o $@ $(USERFLAGS) >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/mv.rxe $(BUILD)/ln.rxe: $(BUILD)/cp.rxe
	@cp $< $@
$(BUILD)/apps.json: Makefile
	@mkdir -p $(@D)
	@python3 -c 'import json; print(json.dumps("init hello forkcheck preemptcheck iotest libccheck $(APPS) mv ln sh vi tcl tclsh".split()))' >$@
userspace: $(BUILD)/mv.rxe $(BUILD)/ln.rxe $(BUILD)/sh.rxe $(BUILD)/vi.rxe $(BUILD)/tcl.rxe $(BUILD)/tclsh.rxe $(BUILD)/apps.json

SHELL_SRC := args blok builtin cmd ctype error expand fault glob io macro main msg name print service setbrk stak string word xec
SHELL_DIR := upstream/Applications/V7/cmd/sh
SHELL_OBJS := $(addprefix $(BUILD)/sh/,$(addsuffix .pobj,$(SHELL_SRC)))
$(BUILD)/sh/%.asm: $(SHELL_DIR)/%.c $(wildcard $(SHELL_DIR)/*.h) $(USERHEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@printf "  RCC %s\n" "$<"
	@$(RCC) compile $< -o $@ $(USERFLAGS) -DBUILD_FSH >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/sh.rxe: $(BUILD)/user/entry.pobj $(SHELL_OBJS) $(BUILD)/sh/input.pobj $(BUILD)/user/userlib.pobj $(LIBC_OBJS)
	@$(RLINK) $^ -f rxe --bank-size 64000 -o $@ >$@.log 2>&1 || { cat $@.log; exit 1; }
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
$(BUILD)/levee.rxe: $(BUILD)/user/entry.pobj $(LEVEE_OBJS) $(BUILD)/user/userlib.pobj $(LIBC_OBJS)
	@$(RLINK) $^ -f rxe --bank-size 64000 -o $@ >$@.log 2>&1 || { cat $@.log; exit 1; }
.PRECIOUS: $(BUILD)/levee/%.asm

$(BUILD)/sh/input.asm: ripple/shell-input.c $(wildcard $(SHELL_DIR)/*.h) $(USERHEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@$(RCC) compile $< -o $@ $(USERFLAGS) -I$(SHELL_DIR) -DBUILD_FSH >$@.log 2>&1 || { cat $@.log; exit 1; }

$(BUILD)/apps/cowsay.asm: upstream/Applications/games/cowsay.c $(USERHEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@$(RCC) compile $< -o $@ $(USERFLAGS) >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/apps/picol.asm: upstream/Applications/TCL/picol.c ripple/picol-extra.inc $(USERHEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@$(RCC) compile $< -o $@ $(USERFLAGS) >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/tcl.rxe $(BUILD)/tclsh.rxe: $(BUILD)/picol.rxe
	@cp $< $@
