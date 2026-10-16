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
CORE := start devio filesys inode kdata process mm syscall_exec syscall_fs syscall_fs2 syscall_fs3 syscall_other syscall_proc timer tty blk512
HEADERS := $(wildcard ripple/*.h) $(wildcard $(K)/include/*.h) $(wildcard $(R)/include/*.h)
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
	rm -rf "$(BUILD)/kernel" "$(BUILD)/port" "$(BUILD)/rt" "$(BUILD)/ros" "$(BUILD)/user"
	rm -f "$(BUILD)/FUZIX.SYS" "$(BUILD)/bios.bin" "$(BUILD)"/*.rxe

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

.PHONY: bios
bios: $(BUILD)/bios.bin
.PRECIOUS: $(BUILD)/port/%.asm $(BUILD)/rt/%.asm $(BUILD)/ros/%.asm
$(BUILD)/user/%.asm: ripple/%.c ripple/user.h $(RCC) Makefile
	@mkdir -p $(@D)
	@printf "  RCC %s\n" "$<"
	@$(RCC) compile $< -o $@ --bank-size 64000 -I$(R)/include -I$(PORT)/ripple >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/user/entry.pobj: ripple/user-entry.asm $(RASM)
	@mkdir -p $(@D)
	@$(RASM) assemble $< -o $@ --bank-size 64000 --max-immediate 65535 >$@.log 2>&1 || { cat $@.log; exit 1; }
$(BUILD)/%.rxe: $(BUILD)/user/entry.pobj $(BUILD)/user/%.pobj $(BUILD)/user/userlib.pobj
	@$(RLINK) $^ -f rxe --bank-size 64000 -o $@ >$@.log 2>&1 || { cat $@.log; exit 1; }
.PHONY: userspace images
userspace: $(BUILD)/init.rxe $(BUILD)/hello.rxe $(BUILD)/forkcheck.rxe $(BUILD)/preemptcheck.rxe $(BUILD)/iotest.rxe
images: kernel bios userspace
.PRECIOUS: $(BUILD)/user/%.asm $(BUILD)/user/%.pobj

.PHONY: test
test: images
	python3 tools/test.py --build $(BUILD)
	node --test tools/disk.test.mjs
	python3 tools/test_terminal.py --build $(BUILD)
