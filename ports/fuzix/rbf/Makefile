.DEFAULT_GOAL := images
.DELETE_ON_ERROR:
.SECONDARY:
ROOT := $(abspath ../../..)
PORT := $(ROOT)/ports/fuzix
HERE := $(PORT)/rbf
BUILD ?= $(ROOT)/build/fuzix-rbf
K := $(PORT)/upstream/Kernel
R := $(ROOT)/runtime
RCC := $(ROOT)/target/release/rcc
RASM := $(ROOT)/src/ripple-asm/target/release/rasm
RLINK := $(ROOT)/src/ripple-asm/target/release/rlink
BANK_SIZE := 16384
FLAGS := --bank-size $(BANK_SIZE) -DBANK_SIZE=$(BANK_SIZE) -DRIPPLE_BULK_IO=1 -I$(HERE) -I$(PORT)/ripple -I$(K)/include -I$(K)/dev -I$(R)/include
HEADERS := $(wildcard $(HERE)/*.h $(PORT)/ripple/*.h $(K)/include/*.h $(R)/include/*.h)
CORE := start devio filesys inode process mm blk512 tty timer
RAM_BANKS := 4
ENTRY := entry
ifeq ($(SHELL_PROFILE),1)
FLAGS += -DRBF_SHELL=1
CORE += syscall_fs syscall_fs2 syscall_fs3 syscall_proc usermem
RAM_BANKS := 8
ENTRY := shell-entry
SHELL_OBJS := $(addprefix $(BUILD)/port/,$(addsuffix .pobj,exec syscall usermem process)) $(BUILD)/rt/vm_devices.pobj
endif
OBJS := $(BUILD)/entry.pobj $(SHELL_OBJS) $(addprefix $(BUILD)/kernel/,$(addsuffix .pobj,$(CORE))) $(addprefix $(BUILD)/port/,$(addsuffix .pobj,data platform devices version)) $(BUILD)/rt/string.pobj $(BUILD)/rt/vm_bulk.pobj
$(BUILD)/kernel/start.asm: EXTRA := -Dexec_or_die=rbf_boot_complete
$(BUILD)/kernel/process.asm: EXTRA := -Dexec_or_die=rbf_unused_exec_or_die $(if $(filter 1,$(SHELL_PROFILE)),-Dsync=rbf_no_exit_sync,)
$(BUILD)/kernel/%.asm: $(K)/%.c $(HEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@echo "  RCC $<"
	@$(RCC) compile $< -o $@ $(FLAGS) $(EXTRA) >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/port/version.asm: $(PORT)/ripple/version.c $(HEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@$(RCC) compile $< -o $@ $(FLAGS) >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/port/%.asm: $(HERE)/%.c $(HEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@echo "  RCC $<"
	@$(RCC) compile $< -o $@ $(FLAGS) >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/rt/%.asm: $(R)/%.c $(HEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@echo "  RCC $<"
	@$(RCC) compile $< -o $@ $(FLAGS) -I$(R) >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/%.pobj: $(BUILD)/%.asm $(RASM)
	@$(RASM) assemble $< -o $@ --bank-size $(BANK_SIZE) --max-immediate 65535 >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/entry.pobj: $(HERE)/$(ENTRY).asm $(RASM) Makefile
	@mkdir -p $(@D)
	@$(RASM) assemble $< -o $@ --bank-size $(BANK_SIZE) --max-immediate 65535
$(BUILD)/kernel.bin: $(OBJS) $(RLINK) $(HERE)/check_image.py
	@echo "Build: Linking reference kernel"
	@$(RLINK) $(OBJS) -f binary --bank-size $(BANK_SIZE) -o $@ >$@.log 2>&1 || { cat $@.log; exit 1; }
ifeq ($(SHELL_PROFILE),1)
	python3 $(HERE)/check_image.py kernel $@
endif
$(BUILD)/kernel.bf: $(OBJS) $(RLINK) $(if $(filter 1,$(SHELL_PROFILE)),$(BUILD)/kernel.bin,)
	@echo "Build: Exporting Brainfuck boot kernel"
	@$(RLINK) $(OBJS) -f bf --standalone --bf-host --memory-banks $(RAM_BANKS) $(if $(filter 1,$(SHELL_PROFILE)),--code-banks 14,) --bank-size $(BANK_SIZE) -o $@ >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/root.img: $(HERE)/disk.mjs $(wildcard $(PORT)/tools/*.mjs)
ifneq ($(SHELL_PROFILE),1)
	@echo "Build: Creating read-only root disk"
	node $(HERE)/disk.mjs $@
endif
.PHONY: images
images: $(BUILD)/kernel.bin $(BUILD)/kernel.bf $(BUILD)/root.img

ifeq ($(SHELL_PROFILE),1)
USER_FLAGS := --bank-size $(BANK_SIZE) -DBANK_SIZE=$(BANK_SIZE) -I$(PORT)/ripple -I$(PORT)/ripple/include -I$(PORT)/upstream/Library/include -I$(R)/include
$(BUILD)/user/%.asm: $(HERE)/%.c $(HEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@echo "  RCC $<"
	@$(RCC) compile $< -o $@ $(USER_FLAGS) >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/user/cat.asm: $(PORT)/upstream/Applications/util/cat.c $(HEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@echo "  RCC $<"
	@$(RCC) compile $< -o $@ $(USER_FLAGS) >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/user/uname-command.asm: $(PORT)/upstream/Applications/util/uname.c $(HEADERS) $(RCC) Makefile
	@mkdir -p $(@D)
	@echo "  RCC $<"
	@$(RCC) compile $< -o $@ $(USER_FLAGS) >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/bin/%.sys: $(BUILD)/user/entry.pobj $(BUILD)/user/%.pobj $(BUILD)/user/userlib.pobj $(RLINK) $(HERE)/check_image.py
	@mkdir -p $(@D)
	@echo "Build: Linking userspace $*"
	@$(RLINK) $(filter %.pobj,$^) -f sys --code-bank-base 12 --gp-bank 3 --sb-bank 4 --bank-size $(BANK_SIZE) -o $@ >$@.log 2>&1 || { cat $@.log; exit 1; }
	python3 $(HERE)/check_image.py program $@
$(BUILD)/bin/uname.sys: $(BUILD)/user/entry.pobj $(BUILD)/user/uname-command.pobj $(BUILD)/user/uname.pobj $(BUILD)/user/userlib.pobj $(RLINK) $(HERE)/check_image.py
	@mkdir -p $(@D)
	@echo "Build: Linking userspace uname"
	@$(RLINK) $(filter %.pobj,$^) -f sys --code-bank-base 12 --gp-bank 3 --sb-bank 4 --bank-size $(BANK_SIZE) -o $@ >$@.log 2>&1 || { cat $@.log; exit 1; }
	python3 $(HERE)/check_image.py program $@
$(BUILD)/user/entry.pobj: $(HERE)/user-entry.asm $(RASM)
	@mkdir -p $(@D)
	@$(RASM) assemble $< -o $@ --bank-size $(BANK_SIZE) --max-immediate 65535
$(BUILD)/init.sys: $(BUILD)/user/entry.pobj $(BUILD)/user/init.pobj $(BUILD)/user/userlib.pobj $(RLINK) $(HERE)/check_image.py
	@echo "Build: Linking tiny init shell"
	@$(RLINK) $(filter %.pobj,$^) -f sys --code-bank-base 10 --gp-bank 3 --sb-bank 4 --bank-size $(BANK_SIZE) -o $@ >$@.log 2>&1 || { cat $@.log; exit 1; }
	python3 $(HERE)/check_image.py init $@
$(BUILD)/root.img: $(BUILD)/init.sys $(addprefix $(BUILD)/bin/,$(addsuffix .sys,cat ls uname))
	@echo "Build: Creating disposable writable shell disk"
	node $(HERE)/disk.mjs $@ $(BUILD)/init.sys
else
.PHONY: shell
shell:
	@$(MAKE) --no-print-directory SHELL_PROFILE=1 BUILD=$(BUILD)/shell images
images: shell
endif
