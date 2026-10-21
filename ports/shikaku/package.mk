# Original Shikaku puzzles and portable C/Nano-X frontend.
SHIKAKU := $(ROOT)/ports/shikaku
APPS += shikaku
userspace: $(BUILD)/shikaku.rxe
$(BUILD)/apps.json: $(SHIKAKU)/package.mk
$(addprefix $(BUILD)/shikaku/,$(addsuffix .asm,main engine store generator)): $(BUILD)/shikaku/%.asm: $(SHIKAKU)/%.c $(SHIKAKU)/shikaku.h $(SHIKAKU)/puzzles.inc $(NX_SRC)/.stamp $(RCC) $(USERHEADERS)
	@mkdir -p $(@D)
	@$(RCC) compile $< -o $@ $(NX_DESKTOP_FLAGS) >$@.log 2>&1 || { tail -50 $@.log; exit 1; }
$(BUILD)/shikaku/%.pobj: $(BUILD)/shikaku/%.asm $(RASM)
	@$(RASM) assemble $< -o $@ --bank-size 64000 --max-immediate 65535 >$@.log 2>&1 || { tail -45 $@.log; exit 1; }
$(BUILD)/shikaku.rxe: $(BUILD)/user/entry.pobj $(addprefix $(BUILD)/shikaku/,$(addsuffix .pobj,main engine store generator)) $(NX_CLIENT_OBJS) $(BUILD)/user/userlib.pobj $(LIBC_OBJS) $(RLINK)
	@$(RXELINK) $(filter %.pobj,$^) -f rxe --bank-size 64000 -o $@ >$@.log 2>&1 || { tail -60 $@.log; exit 1; }
$(BUILD)/shikaku/check.asm: $(SHIKAKU)/tests/engine.c $(SHIKAKU)/shikaku.h $(RCC) $(USERHEADERS)
	@mkdir -p $(@D)
	@$(RCC) compile $< -o $@ $(USERFLAGS) >$@.log 2>&1 || { tail -50 $@.log; exit 1; }
$(BUILD)/shikakucheck.rxe: $(BUILD)/user/entry.pobj $(addprefix $(BUILD)/shikaku/,$(addsuffix .pobj,check engine store generator)) $(BUILD)/user/userlib.pobj $(LIBC_OBJS) $(RLINK)
	@$(RXELINK) $(filter %.pobj,$^) -f rxe --bank-size 64000 -o $@ >$@.log 2>&1 || { tail -60 $@.log; exit 1; }
.PRECIOUS: $(BUILD)/shikaku/%.asm $(BUILD)/shikaku/%.pobj
.PHONY: shikaku
shikaku: $(BUILD)/shikaku.rxe
