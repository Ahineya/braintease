# rOS — a tiny FAT16 explorer for RippleVM
#
# Usage:
#   make
#   make run
#   make run INPUT='dir\ncd docs\ntype spec.txt\nexit\n'

ROOT      ?= $(abspath ../../..)
BUILD     ?= $(ROOT)/c-test/build
SRC       := $(abspath .)
RUNTIME   := $(ROOT)/runtime

RCC       := $(ROOT)/target/release/rcc
RASM      := $(ROOT)/src/ripple-asm/target/release/rasm
RLINK     := $(ROOT)/src/ripple-asm/target/release/rlink
RVM       := $(ROOT)/target/release/rvm

BANK_SIZE ?= 64000
MAX_IMM   := 65535
INCLUDES  := -I $(RUNTIME)/include -I $(SRC)

C_SOURCES := disk.c fat16.c util.c shell.c
ASMS      := $(addprefix $(BUILD)/,$(C_SOURCES:.c=.asm))
OBJS      := $(addprefix $(BUILD)/,$(C_SOURCES:.c=.pobj))
BIN       := $(BUILD)/rOS.bin
DISK      ?= $(ROOT)/disks/ripple_fat16.img
INPUT     ?=

.PHONY: all runtime clean run
.PRECIOUS: $(ASMS) $(OBJS)

all: $(BIN)

runtime:
	$(MAKE) -C $(RUNTIME) all crt0.pobj BANK_SIZE=$(BANK_SIZE)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.asm: $(SRC)/%.c disk.h fat16.h util.h | $(BUILD)
	@echo "Compiling $<"
	$(RCC) compile $< -o $@ --bank-size $(BANK_SIZE) $(INCLUDES)

$(BUILD)/%.pobj: $(BUILD)/%.asm
	@echo "Assembling $<"
	$(RASM) assemble $< -o $@ --bank-size $(BANK_SIZE) --max-immediate $(MAX_IMM)

$(BIN): runtime $(OBJS)
	@echo "Linking $@"
	$(RLINK) $(RUNTIME)/crt0.pobj $(RUNTIME)/libruntime.par $(OBJS) \
		-f binary --bank-size $(BANK_SIZE) -o $@
	@echo "Built $@"

run: $(BIN)
	@if [ ! -f "$(DISK)" ]; then \
		echo "error: disk image not found: $(DISK)"; \
		exit 1; \
	fi
	@echo "Running rvm --disk $(DISK)"
	$(RVM) $(BIN) --disk "$(DISK)" $(if $(INPUT),--input "$(INPUT)",)

clean:
	rm -f $(ASMS) $(OBJS) $(BIN)
	rm -f $(BUILD)/disk.pp.c $(BUILD)/fat16.pp.c $(BUILD)/util.pp.c $(BUILD)/shell.pp.c
