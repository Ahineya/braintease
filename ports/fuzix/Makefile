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
MMU ?= 1
BULK_IO ?= 1
TEST_ENGINE ?= jit
FLAGS := -DRIPPLE_MMU=$(MMU) -DRIPPLE_BULK_IO=$(BULK_IO) --bank-size 64000 -I$(PORT)/ripple -I$(K)/include -I$(K)/dev -I$(R)/include -DBANK_SIZE=64000
RUNTIME_HEADERS := $(addprefix $(R)/include/,$(addsuffix .h,stdint stddef stdarg string ctype stdlib mmio mmio_constants vm_devices vm_bulk irq stdio limits stdbool))
U := $(PORT)/upstream/Library
USERFLAGS := -DRIPPLE_MMU=$(MMU) --bank-size 64000 -I$(PORT)/ripple/include -I$(U)/include -I$(R)/include -I$(PORT)/ripple -DNO_64BIT
USERHEADERS := $(BUILD)/.mmu $(shell find ripple/include $(U)/include -name '*.h') $(RUNTIME_HEADERS) $(K)/include/userstructs.h
LIBC_LOCAL := syscalls stdio format dirent misc heap pipe
AWK_MATH_SRC := __sin __cos __rem_pio2 __rem_pio2_large scalbn fabs sqrt floor log exp pow sin cos atan atan2 strtod ecvt fmod frexp ldexp modf
LIBC_UPSTREAM := $(filter-out regerror malloc calloc realloc readline readline_fn $(AWK_MATH_SRC),$(basename $(notdir $(wildcard upstream/Library/libs/*.c))))
LIBC_OBJS := $(addprefix $(BUILD)/libc/,$(addsuffix .pobj,$(LIBC_LOCAL))) $(addprefix $(BUILD)/ulib/,$(addsuffix .pobj,$(LIBC_UPSTREAM)))
APPS := basename dirname cat cp ls wc head tail grep fgrep sort uniq echo mkdir rmdir touch tee cmp chmod cut colrm expand pwd rm sync true false seq sleep kill tr od sum yes which env printenv chown chgrp du df sed ed xargs id whoami uname date mkfifo mknod comm rev test ps vile levee find dd diff diffh tar cowsay picol more less mktemp groups logname nohup fmt column cksum stty join split tsort tty nice time clear pr col man wump hamurabi expr awk make m4 dc
APPS += free uptime sok top ptysh

CORE := start devio filesys inode kdata process mm syscall_exec syscall_fs syscall_fs2 syscall_fs3 syscall_other syscall_proc timer tty blk512
BULK_CONFIG := $(BUILD)/.bulk-io
MMU_CONFIG := $(BUILD)/.mmu
$(MMU_CONFIG): FORCE
	@mkdir -p $(@D)
	@if test "$$(cat $@ 2>/dev/null)" != "$(MMU)"; then printf '%s\n' '$(MMU)' > $@; fi
.PHONY: FORCE
FORCE:
$(BULK_CONFIG): FORCE
	@mkdir -p $(@D)
	@if test "$$(cat $@ 2>/dev/null)" != "$(BULK_IO)"; then printf '%s\n' '$(BULK_IO)' > $@; fi
HEADERS := $(MMU_CONFIG) $(BULK_CONFIG) $(filter-out ripple/user.h,$(wildcard ripple/*.h)) $(wildcard $(K)/include/*.h) $(RUNTIME_HEADERS) $(ROS)/disk.h $(ROS)/fat16.h
USERHEADERS += ripple/resources.h
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
	@$(RCC) compile $< -o $@ $(FLAGS) $(if $(and $(filter 1,$(MMU)),$(filter syscall_proc,$*)),-D_brk=ripple_legacy_brk -D_sbrk=ripple_legacy_sbrk) >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/kernel/%.pobj: $(BUILD)/kernel/%.asm $(RASM)
	@$(RASM) assemble $< -o $@ --bank-size 64000 --max-immediate 65535 >$@.log 2>&1 || { cat $@.log; exit 1; }
# Persistent disks and backups deliberately survive clean.
clean:
	rm -rf "$(BUILD)/kernel" "$(BUILD)/port" "$(BUILD)/rt" "$(BUILD)/ros" "$(BUILD)/user" "$(BUILD)/apps" "$(BUILD)/libc" "$(BUILD)/ulib" "$(BUILD)/sh" "$(BUILD)/levee" "$(BUILD)/awk" "$(BUILD)/math"
	rm -f "$(BUILD)/FUZIX.SYS" "$(BUILD)/bios.bin" "$(BUILD)/bios.bf" "$(BUILD)"/*.rxe "$(BUILD)/apps.json"

PORT_CORE := platform devices jobcontrol pty usermem exec version $(if $(filter 1,$(MMU)),mmu)
PORT_OBJS := $(addprefix $(BUILD)/port/,$(addsuffix .pobj,$(PORT_CORE)))
SHARED := string ctype stdlib mmio irq putchar puts vm_devices vm_bulk
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
$(BUILD)/port/lowlevel.pobj: $(BUILD)/port/lowlevel-profile.asm $(RASM)
	@mkdir -p $(@D)
	@$(RASM) assemble $< -o $@ --bank-size 64000 --max-immediate 65535 >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/kernel/memalloc_none.asm: $(K)/mm/memalloc_none.c $(HEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@printf "  RCC %s\n" "$<"
	@$(RCC) compile $< -o $@ $(FLAGS) >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/FUZIX.SYS: $(BUILD)/port/lowlevel.pobj $(OBJS) $(PORT_OBJS) $(RT_OBJS) $(BUILD)/kernel/usermem.pobj $(BUILD)/kernel/memalloc_none.pobj $(BUILD)/ros/disk.pobj $(BUILD)/ros/fat16.pobj $(RLINK)
	@$(RLINK) $(filter %.pobj,$^) -f sys --code-bank-base 1 --gp-bank 3 --sb-bank 32 --bank-size 64000 -o $@ >$(BUILD)/kernel-link.log 2>&1 || { cat $(BUILD)/kernel-link.log; exit 1; }
.PHONY: kernel
kernel: $(BUILD)/FUZIX.SYS
$(BUILD)/port/boot-entry.pobj: ripple/boot-entry.asm $(RASM)
	@mkdir -p $(@D)
	@$(RASM) assemble $< -o $@ --bank-size 64000 --max-immediate 65535 >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/bios.bin: $(BUILD)/port/boot-entry.pobj $(BUILD)/port/boot.pobj $(RT_OBJS) $(BUILD)/ros/disk.pobj $(BUILD)/ros/fat16.pobj $(RLINK)
	@$(RLINK) $(filter %.pobj,$^) -f binary --bank-size 64000 -o $@ >$(BUILD)/bios-link.log 2>&1 || { cat $(BUILD)/bios-link.log; exit 1; }

# Dynamic kernel/user code is loaded from the same disk as the native VM.
# Eight process slots plus one spare exec slot reserve twelve banks each, occupying code banks 32..139.
$(BUILD)/bios.bf: $(BUILD)/port/boot-entry.pobj $(BUILD)/port/boot.pobj $(RT_OBJS) $(BUILD)/ros/disk.pobj $(BUILD)/ros/fat16.pobj $(RLINK)
	@test "$(MMU)" = 0 || { echo "Brainfuck requires MMU=0; rebuild images and bf with that profile" >&2; exit 1; }
	@$(RLINK) $(filter %.pobj,$^) -f bf --standalone --bf-host --bf-irq --bank-size 64000 --memory-banks 80 --code-banks 140 -o $@ >$(BUILD)/bios-bf-link.log 2>&1 || { cat $(BUILD)/bios-bf-link.log; exit 1; }
.PHONY: bf
bf: $(BUILD)/bios.bf
	@test "$(MMU)" = 0 || { echo "Brainfuck requires MMU=0" >&2; exit 1; }

.PHONY: bios
bios: $(BUILD)/bios.bin
.PRECIOUS: $(BUILD)/port/%.asm $(BUILD)/rt/%.asm $(BUILD)/ros/%.asm
$(BUILD)/user/%.asm: ripple/%.c ripple/user.h $(USERHEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@printf "  RCC %s\n" "$<"
	@$(RCC) compile $< -o $@ $(USERFLAGS) >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/user/entry.pobj: $(BUILD)/user/entry-profile.asm $(RASM)
	@mkdir -p $(@D)
	@$(RASM) assemble $< -o $@ --bank-size 64000 --max-immediate 65535 >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/%.rxe: $(BUILD)/user/entry.pobj $(BUILD)/user/%.pobj $(BUILD)/user/userlib.pobj $(LIBC_OBJS) $(RLINK)
	@$(RLINK) $(filter %.pobj,$^) -f rxe --bank-size 64000 -o $@ >$@.log 2>&1 || { cat $@.log; exit 1; }
.PHONY: userspace images
userspace: $(BUILD)/init.rxe $(BUILD)/hello.rxe $(BUILD)/forkcheck.rxe $(BUILD)/preemptcheck.rxe $(BUILD)/iotest.rxe $(BUILD)/libccheck.rxe $(BUILD)/mmucheck.rxe $(BUILD)/ttycheck.rxe
images: kernel bios userspace
.PRECIOUS: $(BUILD)/user/%.asm $(BUILD)/user/%.pobj

.PHONY: test
test: images
	python3 tools/test_launch.py
	python3 tools/test.py --build $(BUILD) --engine $(TEST_ENGINE)
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
$(addprefix $(BUILD)/,$(addsuffix .rxe,$(filter-out levee awk,$(APPS)))): $(BUILD)/%.rxe: $(BUILD)/user/entry.pobj $(BUILD)/apps/%.pobj $(BUILD)/user/userlib.pobj $(LIBC_OBJS) $(RLINK)
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
	@python3 -c 'import json; print(json.dumps("init hello forkcheck preemptcheck iotest libccheck mmucheck ttycheck $(APPS) mv ln sh vi tcl tclsh sokoban".split()))' >$@
userspace: $(BUILD)/mv.rxe $(BUILD)/ln.rxe $(BUILD)/sh.rxe $(BUILD)/vi.rxe $(BUILD)/tcl.rxe $(BUILD)/tclsh.rxe $(BUILD)/apps.json
userspace: $(BUILD)/sokoban.rxe
$(BUILD)/sokoban.rxe: $(BUILD)/sok.rxe
	@cp $< $@

SHELL_SRC := args blok builtin cmd ctype error expand fault glob io macro main msg name print service setbrk stak string word xec
SHELL_DIR := upstream/Applications/V7/cmd/sh
SHELL_OBJS := $(addprefix $(BUILD)/sh/,$(addsuffix .pobj,$(SHELL_SRC)))
$(BUILD)/sh/%.asm: $(SHELL_DIR)/%.c $(wildcard $(SHELL_DIR)/*.h) $(USERHEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@printf "  RCC %s\n" "$<"
	@$(RCC) compile $< -o $@ $(USERFLAGS) -DBUILD_FSH >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/sh.rxe: $(BUILD)/user/entry.pobj $(SHELL_OBJS) $(BUILD)/sh/input.pobj $(BUILD)/sh/jobs.pobj $(BUILD)/user/userlib.pobj $(LIBC_OBJS) $(RLINK)
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
AWK_OBJS := $(addprefix $(BUILD)/awk/,$(addsuffix .pobj,$(AWK_SRC) support format softfloat)) $(addprefix $(BUILD)/math/,$(addsuffix .pobj,$(AWK_MATH_SRC)))
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
$(BUILD)/awk/format.asm: ripple/libc/format.c ripple/float-format.inc $(USERHEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@$(RCC) compile $< -o $@ $(AWKFLAGS) -DRIPPLE_FLOAT_FORMAT >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/awk/softfloat.asm: $(R)/softfloat.c $(USERHEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@$(RCC) compile $< -o $@ $(AWKFLAGS) >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/awk.rxe: $(BUILD)/user/entry.pobj $(AWK_OBJS) $(BUILD)/user/userlib.pobj $(filter-out $(BUILD)/libc/format.pobj,$(LIBC_OBJS)) $(RLINK)
	@$(RLINK) $(filter %.pobj,$^) -f rxe --bank-size 64000 -o $@ >$@.log 2>&1 || { cat $@.log; exit 1; }
.PRECIOUS: $(BUILD)/awk/%.asm $(BUILD)/math/%.asm

# Only these two hand-written files need profile selection; generated compiler
# assembly is already selected by the ordinary C preprocessor.
$(BUILD)/port/lowlevel-profile.asm: ripple/lowlevel.asm tools/asm_profile.py $(MMU_CONFIG)
	@mkdir -p $(@D)
	@python3 tools/asm_profile.py $< $@ $(MMU)
$(BUILD)/user/entry-profile.asm: ripple/user-entry.asm tools/asm_profile.py $(MMU_CONFIG)
	@mkdir -p $(@D)
	@python3 tools/asm_profile.py $< $@ $(MMU)

$(BUILD)/sh/jobs.asm: ripple/shell-jobs.c $(wildcard $(SHELL_DIR)/*.h) $(USERHEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@$(RCC) compile $< -o $@ $(USERFLAGS) -I$(SHELL_DIR) -DBUILD_FSH >$@.log 2>&1 || { cat $@.log; exit 1; }

$(addprefix $(BUILD)/apps/,$(addsuffix .asm,ps top ptysh)): $(BUILD)/apps/%.asm: ripple/%.c ripple/process-report.h $(USERHEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@$(RCC) compile $< -o $@ $(USERFLAGS) >$@.log 2>&1 || { cat $@.log; exit 1; }
