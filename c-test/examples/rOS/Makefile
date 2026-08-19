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

BANK_SIZE ?= 64000
MAX_IMM   := 65535
INCLUDES  := -I $(RUNTIME)/include -I $(SRC)

BIOS_BIN  := $(BUILD)/bios.bin
IO_SYS    := $(BUILD)/IO.SYS
KERNEL_SYS := $(BUILD)/KERNEL.SYS
COMMAND_COM := $(BUILD)/COMMAND.COM
STUB_IO   := $(BUILD)/stub_IO.SYS
STUB_KERNEL := $(BUILD)/stub_KERNEL.SYS
STUB_COMMAND := $(BUILD)/stub_COMMAND.COM
HELLO_RXE := $(BUILD)/HELLO.RXE
DISK      := $(DISKS)/ros.img
INPUT     ?=

CRT0      := $(RUNTIME)/crt0.pobj
CRT0_SYS  := $(BUILD)/crt0_sys.pobj
LIBRT     := $(RUNTIME)/libruntime.par

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

.PHONY: all runtime bios io kernel command disk run \
	test-bios test-io test-kernel hello clean
.PRECIOUS: $(BUILD)/%.asm $(BUILD)/%.pobj

all: disk

$(BUILD):
	mkdir -p $(BUILD)

runtime:
	$(MAKE) -C $(RUNTIME) all crt0.pobj BANK_SIZE=$(BANK_SIZE)

$(CRT0_SYS): $(RUNTIME)/ros/crt0_sys.asm | $(BUILD)
	$(RASM) assemble $< -o $@ --bank-size $(BANK_SIZE) --max-immediate $(MAX_IMM)

$(BUILD)/%.pobj: $(SRC)/%.asm | $(BUILD)
	$(RASM) assemble $< -o $@ --bank-size $(BANK_SIZE) --max-immediate $(MAX_IMM)

$(BUILD)/%.asm: $(SRC)/%.c | $(BUILD)
	$(RCC) compile $< -o $@ --bank-size $(BANK_SIZE) $(INCLUDES)

$(BUILD)/%.pobj: $(BUILD)/%.asm
	$(RASM) assemble $< -o $@ --bank-size $(BANK_SIZE) --max-immediate $(MAX_IMM)

bios: runtime $(BIOS_BIN)
io: runtime $(IO_SYS)
kernel: runtime $(KERNEL_SYS)
command: runtime $(COMMAND_COM)

$(BIOS_BIN): runtime $(CRT0) $(BUILD)/bios.pobj $(BUILD)/disk.pobj $(BUILD)/fat16.pobj $(BUILD)/loader.pobj $(BUILD)/imem.pobj
	$(RLINK) $(CRT0) $(MIN_RT) \
		$(BUILD)/bios.pobj $(BUILD)/disk.pobj $(BUILD)/fat16.pobj \
		$(BUILD)/loader.pobj $(BUILD)/imem.pobj \
		-f binary --bank-size $(BANK_SIZE) -o $@

$(IO_SYS): runtime $(CRT0_SYS) $(BUILD)/io_vectors.pobj $(BUILD)/io.pobj $(BUILD)/disk.pobj $(BUILD)/fat16.pobj $(BUILD)/loader.pobj $(BUILD)/imem.pobj
	$(RLINK) $(BUILD)/io_vectors.pobj $(CRT0_SYS) $(MIN_RT) \
		$(BUILD)/io.pobj $(BUILD)/disk.pobj $(BUILD)/fat16.pobj \
		$(BUILD)/loader.pobj $(BUILD)/imem.pobj \
		-f sys --bank-size $(BANK_SIZE) \
		--code-bank-base 1 --gp-bank 3 --sb-bank 2 -o $@

$(KERNEL_SYS): runtime $(CRT0_SYS) $(BUILD)/kernel_vectors.pobj $(BUILD)/io_disk.pobj \
		$(BUILD)/kernel.pobj $(BUILD)/fat16.pobj $(BUILD)/loader.pobj $(BUILD)/imem.pobj
	$(RLINK) $(BUILD)/kernel_vectors.pobj $(CRT0_SYS) $(MIN_RT) $(BUILD)/io_disk.pobj \
		$(BUILD)/kernel.pobj $(BUILD)/fat16.pobj $(BUILD)/loader.pobj $(BUILD)/imem.pobj \
		-f sys --bank-size $(BANK_SIZE) \
		--code-bank-base 2 --gp-bank 4 --sb-bank 2 -o $@

$(COMMAND_COM): runtime $(CRT0_SYS) $(BUILD)/io_tty.pobj $(BUILD)/kernel_api.pobj \
		$(BUILD)/shell.pobj $(BUILD)/util.pobj $(CMD_RT_OBJS)
	$(RLINK) $(CRT0_SYS) $(BUILD)/io_tty.pobj $(BUILD)/kernel_api.pobj \
		$(CMD_RT_OBJS) $(BUILD)/util.pobj $(BUILD)/shell.pobj \
		-f sys --bank-size $(BANK_SIZE) \
		--code-bank-base 3 --gp-bank 6 --sb-bank 7 -o $@

$(STUB_IO): runtime $(CRT0_SYS) $(BUILD)/stub_io.pobj
	$(RLINK) $(CRT0_SYS) $(TINY_RT) $(BUILD)/stub_io.pobj \
		-f sys --bank-size $(BANK_SIZE) \
		--code-bank-base 1 --gp-bank 3 --sb-bank 2 -o $@

$(STUB_KERNEL): runtime $(CRT0_SYS) $(BUILD)/stub_kernel.pobj
	$(RLINK) $(CRT0_SYS) $(TINY_RT) $(BUILD)/stub_kernel.pobj \
		-f sys --bank-size $(BANK_SIZE) \
		--code-bank-base 2 --gp-bank 4 --sb-bank 2 -o $@

$(STUB_COMMAND): runtime $(CRT0_SYS) $(BUILD)/stub_command.pobj
	$(RLINK) $(CRT0_SYS) $(TINY_RT) $(BUILD)/stub_command.pobj \
		-f sys --bank-size $(BANK_SIZE) \
		--code-bank-base 3 --gp-bank 6 --sb-bank 7 -o $@

hello: runtime $(HELLO_RXE)
$(HELLO_RXE): runtime $(RUNTIME)/ros/crt0_ros.pobj $(BUILD)/hello.pobj $(BUILD)/io_tty.pobj $(CMD_RT_OBJS)
	$(MAKE) -C $(RUNTIME) ros/crt0_ros.pobj BANK_SIZE=$(BANK_SIZE) || \
		$(RASM) assemble $(RUNTIME)/ros/crt0_ros.asm -o $(RUNTIME)/ros/crt0_ros.pobj --bank-size $(BANK_SIZE) --max-immediate $(MAX_IMM)
	$(RLINK) $(RUNTIME)/ros/crt0_ros.pobj $(BUILD)/io_tty.pobj $(CMD_RT_OBJS) $(BUILD)/hello.pobj \
		-f rxe --bank-size $(BANK_SIZE) -o $@

disk: $(BIOS_BIN) $(IO_SYS) $(KERNEL_SYS) $(COMMAND_COM)
	python3 $(SRC)/mkfat16.py -o $(DISK) \
		$(IO_SYS):IO.SYS \
		$(KERNEL_SYS):KERNEL.SYS \
		$(COMMAND_COM):COMMAND.COM
	@echo "Built $(DISK)"

run: disk
	$(RVM) $(BIOS_BIN) --disk $(DISK) $(if $(INPUT),--input "$(INPUT)",)

test-bios: runtime $(BIOS_BIN) $(STUB_IO)
	python3 $(SRC)/mkfat16.py -o $(BUILD)/test_bios.img $(STUB_IO):IO.SYS
	$(RVM) $(BIOS_BIN) --disk $(BUILD)/test_bios.img

test-io: runtime $(BIOS_BIN) $(IO_SYS) $(STUB_KERNEL)
	python3 $(SRC)/mkfat16.py -o $(BUILD)/test_io.img $(IO_SYS):IO.SYS $(STUB_KERNEL):KERNEL.SYS
	$(RVM) $(BIOS_BIN) --disk $(BUILD)/test_io.img

test-kernel: runtime $(BIOS_BIN) $(IO_SYS) $(KERNEL_SYS) $(STUB_COMMAND)
	python3 $(SRC)/mkfat16.py -o $(BUILD)/test_kernel.img \
		$(IO_SYS):IO.SYS $(KERNEL_SYS):KERNEL.SYS $(STUB_COMMAND):COMMAND.COM
	$(RVM) $(BIOS_BIN) --disk $(BUILD)/test_kernel.img

clean:
	rm -rf $(BUILD)
	rm -f $(DISK)
