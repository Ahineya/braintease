# C Doom TITLEPIC demo
#
# wad.c / mem.c / picture.c are included from doom-fixed.c and compiled as
# one translation unit. rcc assigns each separately compiled file's globals
# from address 0, so linking multiple C objects would overlap in GP.
#
# Usage:
#   make
#   make run WAD=/path/to/DOOM1.WAD
#   make run                    # uses ./doom1.wad if present

ROOT      ?= $(abspath ../../..)
BUILD     ?= $(ROOT)/c-test/build
SRC       := $(abspath .)
RUNTIME   := $(ROOT)/runtime

RCC       := $(ROOT)/target/release/rcc
RCPP      := $(ROOT)/target/release/rcpp
RASM      := $(ROOT)/src/ripple-asm/target/release/rasm
RLINK     := $(ROOT)/src/ripple-asm/target/release/rlink
RVM       := $(ROOT)/target/release/rvm

BANK_SIZE ?= 64000
MAX_IMM   := 65535
INCLUDES  := -I $(RUNTIME)/include -I $(SRC)

C_SOURCES := doom-fixed.c wad.c mem.c picture.c
PP        := $(BUILD)/doom-c.pp.c
ASM       := $(BUILD)/doom-c.asm
POBJ      := $(BUILD)/doom-c.pobj
BIN       := $(BUILD)/doom-c.bin

WAD       ?= $(SRC)/doom1.wad
FREQUENCY ?=

.PHONY: all runtime clean run
.PRECIOUS: $(PP) $(ASM) $(POBJ)

all: $(BIN)

runtime:
	$(MAKE) -C $(RUNTIME) all crt0.pobj BANK_SIZE=$(BANK_SIZE)

$(BUILD):
	mkdir -p $(BUILD)

$(PP): $(C_SOURCES) wad.h mem.h picture.h | $(BUILD)
	@echo "Preprocessing doom-fixed.c"
	$(RCPP) $(SRC)/doom-fixed.c -o $@ $(INCLUDES)

$(ASM): $(PP)
	@echo "Compiling $<"
	$(RCC) compile $< -o $@ --no-preprocess --bank-size $(BANK_SIZE)

$(POBJ): $(ASM)
	@echo "Assembling $<"
	$(RASM) assemble $< -o $@ --bank-size $(BANK_SIZE) --max-immediate $(MAX_IMM)

$(BIN): runtime $(POBJ)
	@echo "Linking $@"
	$(RLINK) $(RUNTIME)/crt0.pobj $(RUNTIME)/libruntime.par $(POBJ) \
		-f binary --bank-size $(BANK_SIZE) -o $@
	@echo "Built $@"

run: $(BIN)
	@if [ ! -f "$(WAD)" ]; then \
		echo "error: IWAD not found: $(WAD)"; \
		echo "Set WAD=/path/to/DOOM1.WAD"; \
		exit 1; \
	fi
	@echo "Running rvm --visual --disk $(WAD)"
	$(RVM) $(BIN) --visual --disk "$(WAD)" $(if $(FREQUENCY),--frequency $(FREQUENCY),)

clean:
	rm -f $(PP) $(ASM) $(POBJ) $(BIN)
	rm -f $(BUILD)/doom-c-*.pp.c $(BUILD)/doom-c-*.asm $(BUILD)/doom-c-*.pobj
