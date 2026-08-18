# C Doom TITLEPIC demo
#
# wad.c, mem.c, picture.c, and doom-fixed.c are compiled as separate
# translation units. The linker concatenates each object's GP range.
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
RASM      := $(ROOT)/src/ripple-asm/target/release/rasm
RLINK     := $(ROOT)/src/ripple-asm/target/release/rlink
RVM       := $(ROOT)/target/release/rvm

BANK_SIZE ?= 64000
MAX_IMM   := 65535
INCLUDES  := -I $(RUNTIME)/include -I $(SRC)

C_SOURCES := wad.c mem.c picture.c doom-fixed.c
ASMS      := $(addprefix $(BUILD)/,$(C_SOURCES:.c=.asm))
OBJS      := $(addprefix $(BUILD)/,$(C_SOURCES:.c=.pobj))
BIN       := $(BUILD)/doom-c.bin

WAD       ?= $(SRC)/doom1.wad
FREQUENCY ?=

.PHONY: all runtime clean run
.PRECIOUS: $(ASMS) $(OBJS)

all: $(BIN)

runtime:
	$(MAKE) -C $(RUNTIME) all crt0.pobj BANK_SIZE=$(BANK_SIZE)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.asm: $(SRC)/%.c wad.h mem.h picture.h | $(BUILD)
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
	@if [ ! -f "$(WAD)" ]; then \
		echo "error: IWAD not found: $(WAD)"; \
		echo "Set WAD=/path/to/DOOM1.WAD"; \
		exit 1; \
	fi
	@echo "Running rvm --visual --disk $(WAD)"
	$(RVM) $(BIN) --visual --disk "$(WAD)" $(if $(FREQUENCY),--frequency $(FREQUENCY),)

clean:
	rm -f $(ASMS) $(OBJS) $(BIN)
	rm -f $(BUILD)/doom-c.pp.c $(BUILD)/doom-c.asm $(BUILD)/doom-c.pobj
