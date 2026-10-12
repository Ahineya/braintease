# Ripple C Runtime Library Makefile

# Tools
RCC = ../target/release/rcc
RASM = ../src/ripple-asm/target/release/rasm
RLINK = ../src/ripple-asm/target/release/rlink

# Assembler settings (can be overridden from command line)
BANK_SIZE ?= 64000  # Addressable 16-bit cells per bank
MAX_IMMEDIATE = 65535
RUNTIME_HEADERS = $(wildcard include/*.h)
BANK_SIZE_STAMP = .bank-size

# Source files
C_SOURCES = __char_patch.c putchar.c puts.c getchar.c mmio.c vm_devices.c rand.c graphics.c string.c malloc.c printf.c ctype.c stdlib.c softfloat.c scanf.c
ASM_SOURCES = crt0.asm

# qfixed lives in src/ and is compiled via the qfixed.asm rule below
ASM_FILES = $(C_SOURCES:.c=.asm) qfixed.asm
C_OBJ_FILES = $(C_SOURCES:.c=.pobj) qfixed.pobj

# Don't include crt0 in the runtime library - it should be linked separately
RUNTIME_OBJ_FILES = $(C_OBJ_FILES)
ALL_OBJ_FILES = $(C_OBJ_FILES) crt0.pobj
RUNTIME_LIB = libruntime.par

# rOS apps: INT21 putchar/getchar, no mmio. crt0_ros is linked separately.
ROS_C_SOURCES = ros/putchar.c ros/getchar.c
ROS_C_ASM = $(ROS_C_SOURCES:.c=.asm)
ROS_C_OBJ = $(ROS_C_SOURCES:.c=.pobj)
ROS_ASM_OBJ = ros/int21.pobj ros/crt0_ros.pobj ros/crt0_sys.pobj
LIBROS_OBJ = ros/int21.pobj ros/putchar.pobj ros/getchar.pobj puts.pobj
LIBROS = libros.par

# Default target
all: $(RUNTIME_LIB) $(LIBROS) crt0.pobj $(ROS_ASM_OBJ)

# Rewrite the stamp only when the configured bank size changes. Assembly and
# object files embed this setting and must not be reused for another size.
$(BANK_SIZE_STAMP): FORCE
	@if test "$$(cat $@ 2>/dev/null)" != "$(strip $(BANK_SIZE))"; then \
		printf '%s\n' "$(strip $(BANK_SIZE))" > $@; \
	fi

FORCE:

# Compile C files to assembly
%.asm: %.c $(RCC) $(RUNTIME_HEADERS) $(BANK_SIZE_STAMP)
	@echo "Compiling $< to assembly..."
	@$(RCC) compile $< -o $@ -I./include --bank-size $(BANK_SIZE) -D BANK_SIZE=$(BANK_SIZE)

# Compile qfixed source
qfixed.asm: src/qfixed.c $(RCC) $(RUNTIME_HEADERS) $(BANK_SIZE_STAMP)
	@echo "Compiling qfixed.c to assembly..."
	@$(RCC) compile $< -o $@ -I./include --bank-size $(BANK_SIZE) -D BANK_SIZE=$(BANK_SIZE)

# Assemble C-generated assembly to object files
$(C_OBJ_FILES): %.pobj: %.asm $(RASM) $(BANK_SIZE_STAMP)
	@echo "Assembling $< to object..."
	@$(RASM) assemble $< -o $@ --bank-size $(BANK_SIZE) --max-immediate $(MAX_IMMEDIATE)

# Special rule for crt0.pobj (already in assembly)
crt0.pobj: crt0.asm $(RASM) $(BANK_SIZE_STAMP)
	@echo "Assembling crt0.asm to object..."
	@$(RASM) assemble crt0.asm -o crt0.pobj --bank-size $(BANK_SIZE) --max-immediate $(MAX_IMMEDIATE)

$(ROS_C_OBJ): %.pobj: %.asm $(RASM) $(BANK_SIZE_STAMP)
	@echo "Assembling $< to object..."
	@$(RASM) assemble $< -o $@ --bank-size $(BANK_SIZE) --max-immediate $(MAX_IMMEDIATE)

ros/int21.pobj: ros/int21.asm $(RASM) $(BANK_SIZE_STAMP)
	@$(RASM) assemble $< -o $@ --bank-size $(BANK_SIZE) --max-immediate $(MAX_IMMEDIATE)

ros/crt0_ros.pobj: ros/crt0_ros.asm $(RASM) $(BANK_SIZE_STAMP)
	@$(RASM) assemble $< -o $@ --bank-size $(BANK_SIZE) --max-immediate $(MAX_IMMEDIATE)

ros/crt0_sys.pobj: ros/crt0_sys.asm $(RASM) $(BANK_SIZE_STAMP)
	@$(RASM) assemble $< -o $@ --bank-size $(BANK_SIZE) --max-immediate $(MAX_IMMEDIATE)

# Create runtime library archive (without crt0)
$(RUNTIME_LIB): $(RUNTIME_OBJ_FILES) $(RLINK)
	@echo "Creating runtime library archive..."
	@$(RLINK) $(RUNTIME_OBJ_FILES) -f archive -o $(RUNTIME_LIB)

$(LIBROS): $(LIBROS_OBJ) $(RLINK)
	@echo "Creating rOS library archive..."
	@$(RLINK) $(LIBROS_OBJ) -f archive -o $(LIBROS)

# Clean generated files (but not hand-written .asm)
clean:
	rm -f $(ASM_FILES) $(ROS_C_ASM) *.pobj *.par *.bf *.bfm ros/*.pobj $(BANK_SIZE_STAMP)
	@echo "Cleaned runtime directory"

# Test target - build a test program with runtime
test: $(RUNTIME_LIB) crt0.pobj
	@echo "Building test program with runtime..."
	@$(RCC) compile ../c-test/tests-runtime/test_runtime_simple.c -o test_main.asm --bank-size $(BANK_SIZE)
	@$(RASM) assemble test_main.asm -o test_main.pobj --bank-size $(BANK_SIZE) --max-immediate $(MAX_IMMEDIATE)
	@echo "Linking crt0 + runtime archive + test program..."
	@$(RLINK) crt0.pobj $(RUNTIME_LIB) test_main.pobj -f macro --standalone -o test_standalone.bf
	@echo "Test program built: test_standalone.bf"
	@echo "Run with: bfm expand test_standalone.bf | bf"

.PHONY: all clean test FORCE
