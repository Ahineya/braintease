# rOS — DOS-like Ripple OS
#
# make bios / io / kernel / command / disk / run
# make test-bios / test-io / test-kernel

ROOT      ?= $(abspath ../../..)
BUILD     ?= $(ROOT)/c-test/build/ros
SRC       := $(abspath .)
RUNTIME   := $(ROOT)/runtime
DISKS     := $(ROOT)/disks

RCC       := $(ROOT)/target/release/rcc
RASM      := $(ROOT)/src/ripple-asm/target/release/rasm
RLINK     := $(ROOT)/src/ripple-asm/target/release/rlink
RVM       := $(ROOT)/target/release/rvm
NODE      ?= node

BANK_SIZE ?= 64000
MAX_IMM   := 65535
INCLUDES  := -I $(RUNTIME)/include -I $(SRC)
HEADERS   := $(wildcard $(SRC)/*.h) $(wildcard $(RUNTIME)/include/*.h)
BUILD_RULES := $(abspath $(lastword $(MAKEFILE_LIST)))
CONFIG    := $(BUILD)/.config

BIOS_BIN  := $(BUILD)/bios.bin
IO_SYS    := $(BUILD)/IO.SYS
KERNEL_SYS := $(BUILD)/KERNEL.SYS
COMMAND_COM := $(BUILD)/COMMAND.COM
STUB_IO   := $(BUILD)/stub_IO.SYS
STUB_KERNEL := $(BUILD)/stub_KERNEL.SYS
STUB_COMMAND := $(BUILD)/stub_COMMAND.COM
HELLO_RXE := $(BUILD)/HELLO.RXE
SIERP_RXE := $(BUILD)/SIERP.RXE
BF_RXE    := $(BUILD)/BF.RXE
README_TXT := $(SRC)/README.TXT
DISK      := $(DISKS)/ros.img
INPUT     ?=

CRT0      := $(RUNTIME)/crt0.pobj
CRT0_SYS  := $(RUNTIME)/ros/crt0_sys.pobj
CRT0_ROS  := $(RUNTIME)/ros/crt0_ros.pobj
LIBRT     := $(RUNTIME)/libruntime.par
LIBROS    := $(RUNTIME)/libros.par

# Slim runtime: full libruntime.par is huge and made SYS loads look hung.
MIN_RT := \
	$(RUNTIME)/putchar.pobj \
	$(RUNTIME)/getchar.pobj \
	$(RUNTIME)/puts.pobj \
	$(RUNTIME)/string.pobj \
	$(RUNTIME)/ctype.pobj \
	$(RUNTIME)/stdlib.pobj \
	$(RUNTIME)/mmio.pobj

TINY_RT := $(RUNTIME)/putchar.pobj $(RUNTIME)/puts.pobj $(RUNTIME)/stdlib.pobj

CMD_RT_OBJS := \
	$(RUNTIME)/__char_patch.pobj \
	$(RUNTIME)/puts.pobj \
	$(RUNTIME)/string.pobj \
	$(RUNTIME)/malloc.pobj \
	$(RUNTIME)/printf.pobj \
	$(RUNTIME)/ctype.pobj \
	$(RUNTIME)/stdlib.pobj \
	$(RUNTIME)/softfloat.pobj

.PHONY: all runtime bios io kernel command images disk run FORCE \
	test-bios test-io test-kernel test-disk-image hello clean
.PRECIOUS: $(BUILD)/%.asm $(BUILD)/%.pobj

all: disk

$(BUILD):
	mkdir -p $(BUILD)

# Build just the runtime objects used below. Finish that sub-make before
# evaluating image dependencies, so updated object timestamps are visible.
RUNTIME_FILES := $(sort $(CRT0) $(CRT0_SYS) $(CRT0_ROS) $(LIBROS) $(MIN_RT) \
	$(TINY_RT) $(CMD_RT_OBJS) $(RUNTIME)/ros/int21.pobj \
	$(RUNTIME)/ros/putchar.pobj $(RUNTIME)/ros/getchar.pobj)

runtime:
	$(MAKE) -C $(RUNTIME) $(patsubst $(RUNTIME)/%,%,$(RUNTIME_FILES)) \
		BANK_SIZE=$(BANK_SIZE) MAX_IMMEDIATE=$(MAX_IMM) SEGMENTED_STACK=0 \
		RCC=$(RCC) RASM=$(RASM) RLINK=$(RLINK)

$(CONFIG): FORCE | $(BUILD)
	@settings='$(BANK_SIZE):$(MAX_IMM):$(RCC):$(RASM):$(RLINK)'; \
	if test "$$(cat $@ 2>/dev/null)" != "$$settings"; then \
		printf '%s\n' "$$settings" > $@; \
	fi

FORCE:

$(BUILD)/%.pobj: $(SRC)/%.asm $(RASM) $(CONFIG) $(BUILD_RULES) | $(BUILD)
	$(RASM) assemble $< -o $@ --bank-size $(BANK_SIZE) --max-immediate $(MAX_IMM)

$(BUILD)/%.asm: $(SRC)/%.c $(RCC) $(HEADERS) $(CONFIG) $(BUILD_RULES) | $(BUILD)
	$(RCC) compile $< -o $@ --bank-size $(BANK_SIZE) $(INCLUDES)

$(BUILD)/%.pobj: $(BUILD)/%.asm $(RASM) $(CONFIG) $(BUILD_RULES)
	$(RASM) assemble $< -o $@ --bank-size $(BANK_SIZE) --max-immediate $(MAX_IMM)

bios: runtime
	$(MAKE) --no-print-directory $(BIOS_BIN)
io: runtime
	$(MAKE) --no-print-directory $(IO_SYS)
kernel: runtime
	$(MAKE) --no-print-directory $(KERNEL_SYS)
command: runtime
	$(MAKE) --no-print-directory $(COMMAND_COM)

$(BIOS_BIN): $(CRT0) $(MIN_RT) $(BUILD)/bios.pobj $(BUILD)/bootmsg.pobj \
		$(BUILD)/disk.pobj $(BUILD)/fat16.pobj $(BUILD)/loader.pobj $(BUILD)/imem.pobj
	$(RLINK) $(CRT0) $(MIN_RT) \
		$(BUILD)/bootmsg.pobj $(BUILD)/bios.pobj $(BUILD)/disk.pobj $(BUILD)/fat16.pobj \
		$(BUILD)/loader.pobj $(BUILD)/imem.pobj \
		-f binary --bank-size $(BANK_SIZE) -o $@

$(IO_SYS): $(CRT0_SYS) $(MIN_RT) $(BUILD)/io_vectors.pobj $(BUILD)/bootmsg.pobj \
		$(BUILD)/io.pobj $(BUILD)/disk.pobj $(BUILD)/fat16.pobj $(BUILD)/loader.pobj $(BUILD)/imem.pobj
	$(RLINK) $(BUILD)/io_vectors.pobj $(CRT0_SYS) $(MIN_RT) \
		$(BUILD)/bootmsg.pobj $(BUILD)/io.pobj $(BUILD)/disk.pobj $(BUILD)/fat16.pobj \
		$(BUILD)/loader.pobj $(BUILD)/imem.pobj \
		-f sys --bank-size $(BANK_SIZE) \
		--code-bank-base 1 --gp-bank 3 --sb-bank 2 -o $@

$(KERNEL_SYS): $(CRT0_SYS) $(MIN_RT) $(BUILD)/kernel_vectors.pobj $(BUILD)/io_disk.pobj \
		$(BUILD)/bootmsg.pobj $(BUILD)/kernel.pobj $(BUILD)/fat16.pobj \
		$(BUILD)/loader.pobj $(BUILD)/imem.pobj
	$(RLINK) $(BUILD)/kernel_vectors.pobj $(CRT0_SYS) $(MIN_RT) $(BUILD)/io_disk.pobj \
		$(BUILD)/bootmsg.pobj $(BUILD)/kernel.pobj $(BUILD)/fat16.pobj \
		$(BUILD)/loader.pobj $(BUILD)/imem.pobj \
		-f sys --bank-size $(BANK_SIZE) \
		--code-bank-base 2 --gp-bank 4 --sb-bank 2 -o $@

$(COMMAND_COM): $(CRT0_SYS) $(RUNTIME)/ros/int21.pobj \
		$(RUNTIME)/ros/putchar.pobj $(RUNTIME)/ros/getchar.pobj \
		$(BUILD)/bootmsg.pobj $(BUILD)/kernel_api.pobj \
		$(BUILD)/shell.pobj $(BUILD)/util.pobj $(CMD_RT_OBJS)
	$(RLINK) $(CRT0_SYS) $(RUNTIME)/ros/int21.pobj \
		$(RUNTIME)/ros/putchar.pobj $(RUNTIME)/ros/getchar.pobj \
		$(BUILD)/bootmsg.pobj $(BUILD)/kernel_api.pobj \
		$(CMD_RT_OBJS) $(BUILD)/util.pobj $(BUILD)/shell.pobj \
		-f sys --bank-size $(BANK_SIZE) \
		--code-bank-base 3 --gp-bank 6 --sb-bank 7 -o $@

$(STUB_IO): $(CRT0_SYS) $(TINY_RT) $(BUILD)/stub_io.pobj
	$(RLINK) $(CRT0_SYS) $(TINY_RT) $(BUILD)/stub_io.pobj \
		-f sys --bank-size $(BANK_SIZE) \
		--code-bank-base 1 --gp-bank 3 --sb-bank 2 -o $@

$(STUB_KERNEL): $(CRT0_SYS) $(TINY_RT) $(BUILD)/stub_kernel.pobj
	$(RLINK) $(CRT0_SYS) $(TINY_RT) $(BUILD)/stub_kernel.pobj \
		-f sys --bank-size $(BANK_SIZE) \
		--code-bank-base 2 --gp-bank 4 --sb-bank 2 -o $@

$(STUB_COMMAND): $(CRT0_SYS) $(TINY_RT) $(BUILD)/stub_command.pobj
	$(RLINK) $(CRT0_SYS) $(TINY_RT) $(BUILD)/stub_command.pobj \
		-f sys --bank-size $(BANK_SIZE) \
		--code-bank-base 3 --gp-bank 6 --sb-bank 7 -o $@

hello: runtime
	$(MAKE) --no-print-directory $(HELLO_RXE)

$(HELLO_RXE): $(CRT0_ROS) $(LIBROS) $(BUILD)/hello.pobj
	$(RLINK) $(CRT0_ROS) $(LIBROS) $(BUILD)/hello.pobj \
		-f rxe --bank-size $(BANK_SIZE) -o $@

$(BUILD)/sierpinski.asm: $(SRC)/../sierpinski.c $(RCC) $(HEADERS) $(CONFIG) $(BUILD_RULES) | $(BUILD)
	$(RCC) compile $< -o $@ --bank-size $(BANK_SIZE) $(INCLUDES)

$(BUILD)/brainfuck.asm: $(SRC)/../brainfuck.c $(RCC) $(HEADERS) $(CONFIG) $(BUILD_RULES) | $(BUILD)
	$(RCC) compile $< -o $@ --bank-size $(BANK_SIZE) $(INCLUDES)

$(SIERP_RXE): $(CRT0_ROS) $(LIBROS) $(BUILD)/sierpinski.pobj
	$(RLINK) $(CRT0_ROS) $(LIBROS) $(BUILD)/sierpinski.pobj \
		-f rxe --bank-size $(BANK_SIZE) -o $@

# Keep character I/O on INT21 while reusing the normal printf implementation.
$(BF_RXE): $(CRT0_ROS) $(RUNTIME)/ros/int21.pobj \
		$(RUNTIME)/ros/putchar.pobj $(RUNTIME)/ros/getchar.pobj \
		$(CMD_RT_OBJS) $(BUILD)/brainfuck.pobj
	$(RLINK) $(CRT0_ROS) $(RUNTIME)/ros/int21.pobj \
		$(RUNTIME)/ros/putchar.pobj $(RUNTIME)/ros/getchar.pobj \
		$(CMD_RT_OBJS) $(BUILD)/brainfuck.pobj \
		-f rxe --bank-size $(BANK_SIZE) -o $@

IMAGE_FILES := $(BIOS_BIN) $(IO_SYS) $(KERNEL_SYS) $(COMMAND_COM) $(HELLO_RXE) $(SIERP_RXE) $(BF_RXE)
$(IMAGE_FILES) $(STUB_IO) $(STUB_KERNEL) $(STUB_COMMAND): $(RLINK) $(CONFIG) $(BUILD_RULES)

images: runtime
	$(MAKE) --no-print-directory $(IMAGE_FILES)

disk: images
	$(NODE) $(SRC)/mkfat16.mjs -o $(DISK) \
		$(IO_SYS):IO.SYS \
		$(KERNEL_SYS):KERNEL.SYS \
		$(COMMAND_COM):COMMAND.COM \
		$(HELLO_RXE):HELLO.RXE \
		$(SIERP_RXE):SIERP.RXE \
		$(BF_RXE):BF.RXE \
		$(README_TXT):README.TXT
	@echo "Built $(DISK)"

run: disk
	$(RVM) $(BIOS_BIN) --disk $(DISK) $(if $(INPUT),--input "$(INPUT)",)

test-bios: runtime
	$(MAKE) --no-print-directory $(BIOS_BIN) $(STUB_IO)
	$(NODE) $(SRC)/mkfat16.mjs -o $(BUILD)/test_bios.img $(STUB_IO):IO.SYS
	$(RVM) $(BIOS_BIN) --disk $(BUILD)/test_bios.img

test-io: runtime
	$(MAKE) --no-print-directory $(BIOS_BIN) $(IO_SYS) $(STUB_KERNEL)
	$(NODE) $(SRC)/mkfat16.mjs -o $(BUILD)/test_io.img $(IO_SYS):IO.SYS $(STUB_KERNEL):KERNEL.SYS
	$(RVM) $(BIOS_BIN) --disk $(BUILD)/test_io.img

test-kernel: runtime
	$(MAKE) --no-print-directory $(BIOS_BIN) $(IO_SYS) $(KERNEL_SYS) $(STUB_COMMAND)
	$(NODE) $(SRC)/mkfat16.mjs -o $(BUILD)/test_kernel.img \
		$(IO_SYS):IO.SYS $(KERNEL_SYS):KERNEL.SYS $(STUB_COMMAND):COMMAND.COM
	$(RVM) $(BIOS_BIN) --disk $(BUILD)/test_kernel.img

test-disk-image:
	$(NODE) --test $(SRC)/mkfat16.test.mjs

clean:
	rm -rf $(BUILD)
	rm -f $(DISK)
