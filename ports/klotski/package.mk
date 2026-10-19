# Original GNOME Klotski puzzles/rules/art with a shared portable C/Nano-X UI.
KLOTSKI := $(ROOT)/ports/klotski
APPS += klotski
userspace: $(BUILD)/klotski.rxe
$(BUILD)/apps.json: $(KLOTSKI)/package.mk
$(addprefix $(BUILD)/klotski/,$(addsuffix .asm,main engine store)): $(BUILD)/klotski/%.asm: $(KLOTSKI)/%.c $(KLOTSKI)/klotski.h $(KLOTSKI)/levels.inc $(KLOTSKI)/tiles.inc $(NX_SRC)/.stamp $(RCC) $(USERHEADERS)
	@mkdir -p $(@D)
	@$(RCC) compile $< -o $@ $(NX_DESKTOP_FLAGS) >$@.log 2>&1 || { tail -50 $@.log; exit 1; }
$(BUILD)/klotski/%.pobj: $(BUILD)/klotski/%.asm $(RASM)
	@$(RASM) assemble $< -o $@ --bank-size 64000 --max-immediate 65535 >$@.log 2>&1 || { tail -45 $@.log; exit 1; }
$(BUILD)/klotski.rxe: $(BUILD)/user/entry.pobj $(addprefix $(BUILD)/klotski/,$(addsuffix .pobj,main engine store)) $(NX_CLIENT_OBJS) $(BUILD)/user/userlib.pobj $(LIBC_OBJS) $(RLINK)
	@$(RXELINK) $(filter %.pobj,$^) -f rxe --bank-size 64000 -o $@ >$@.log 2>&1 || { tail -60 $@.log; exit 1; }
$(BUILD)/klotski/check.asm: $(KLOTSKI)/tests/engine.c $(KLOTSKI)/tests/solution.inc $(KLOTSKI)/klotski.h $(RCC) $(USERHEADERS)
	@mkdir -p $(@D)
	@$(RCC) compile $< -o $@ $(USERFLAGS) >$@.log 2>&1 || { tail -50 $@.log; exit 1; }
$(BUILD)/klotskicheck.rxe: $(BUILD)/user/entry.pobj $(addprefix $(BUILD)/klotski/,$(addsuffix .pobj,check engine store)) $(BUILD)/user/userlib.pobj $(LIBC_OBJS) $(RLINK)
	@$(RXELINK) $(filter %.pobj,$^) -f rxe --bank-size 64000 -o $@ >$@.log 2>&1 || { tail -60 $@.log; exit 1; }
.PRECIOUS: $(BUILD)/klotski/%.asm $(BUILD)/klotski/%.pobj
.PHONY: klotski
klotski: $(BUILD)/klotski.rxe
$(BUILD)/klotski/control.asm: $(KLOTSKI)/tests/control.c $(NX_SRC)/.stamp $(RCC) $(USERHEADERS)
	@mkdir -p $(@D)
	@$(RCC) compile $< -o $@ $(NX_DESKTOP_FLAGS) >$@.log 2>&1 || { tail -50 $@.log; exit 1; }
$(BUILD)/klotskictl.rxe: $(BUILD)/user/entry.pobj $(BUILD)/klotski/control.pobj $(NX_CLIENT_OBJS) $(BUILD)/user/userlib.pobj $(LIBC_OBJS) $(RLINK)
	@$(RXELINK) $(filter %.pobj,$^) -f rxe --bank-size 64000 -o $@ >$@.log 2>&1 || { tail -60 $@.log; exit 1; }
