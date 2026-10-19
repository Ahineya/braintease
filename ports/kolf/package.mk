# Kolf's portable engine and Nano-X frontend; eight compressed courses embedded.
GOLF := $(ROOT)/ports/kolf
APPS += kolf
userspace: $(BUILD)/kolf.rxe
$(BUILD)/apps.json: $(GOLF)/package.mk
$(addprefix $(BUILD)/kolf/,$(addsuffix .asm,main engine course putting)): $(BUILD)/kolf/%.asm: $(GOLF)/%.c $(GOLF)/golf.h $(GOLF)/putting.h $(GOLF)/dial.inc $(GOLF)/courses.inc $(ROOT)/libraries/deflate/deflate.h $(ROOT)/libraries/ini/ini.h $(NX_SRC)/.stamp $(RCC) $(USERHEADERS)
	@mkdir -p $(@D)
	@$(RCC) compile $< -o $@ $(filter-out -DNO_64BIT,$(NX_DESKTOP_FLAGS)) >$@.log 2>&1 || { tail -45 $@.log; exit 1; }
$(BUILD)/kolf/ini.asm: $(ROOT)/libraries/ini/ini.c $(ROOT)/libraries/ini/ini.h $(RCC) $(USERHEADERS)
	@mkdir -p $(@D)
	@$(RCC) compile $< -o $@ $(USERFLAGS) >$@.log 2>&1 || { tail -45 $@.log; exit 1; }
$(BUILD)/kolf/%.pobj: $(BUILD)/kolf/%.asm $(RASM)
	@$(RASM) assemble $< -o $@ --bank-size 64000 --max-immediate 65535 >$@.log 2>&1 || { tail -45 $@.log; exit 1; }
# Keep the copied game usable on kernels predating the shared-library loader.
$(BUILD)/kolf.rxe: $(BUILD)/user/entry.pobj $(addprefix $(BUILD)/kolf/,$(addsuffix .pobj,main engine course putting ini deflate)) $(NX_CLIENT_OBJS) $(BUILD)/user/userlib.pobj $(FLOAT_LIB_OBJS) $(filter-out $(BUILD)/libc/format.pobj,$(LIBC_OBJS)) $(RLINK) $(GOLF)/package.mk
	@$(RLINK) $(filter %.pobj,$^) -f rxe --bank-size 64000 -o $@ >$@.log 2>&1 || { tail -60 $@.log; exit 1; }
.PRECIOUS: $(BUILD)/kolf/%.asm $(BUILD)/kolf/%.pobj
.PHONY: kolf
kolf: $(BUILD)/kolf.rxe
$(BUILD)/kolf/check.asm: $(GOLF)/tests/engine.c $(GOLF)/golf.h $(GOLF)/putting.h $(RCC) $(USERHEADERS)
	@mkdir -p $(@D)
	@$(RCC) compile $< -o $@ $(filter-out -DNO_64BIT,$(USERFLAGS)) >$@.log 2>&1 || { tail -45 $@.log; exit 1; }
$(BUILD)/kolfcheck.rxe: $(BUILD)/user/entry.pobj $(addprefix $(BUILD)/kolf/,$(addsuffix .pobj,check engine course putting ini)) $(BUILD)/user/userlib.pobj $(FLOAT_LIB_OBJS) $(filter-out $(BUILD)/libc/format.pobj,$(LIBC_OBJS)) $(RLINK) $(GOLF)/package.mk
	@$(RLINK) $(filter %.pobj,$^) -f rxe --bank-size 64000 -o $@ >$@.log 2>&1 || { tail -60 $@.log; exit 1; }

$(BUILD)/kolf/deflate.asm: $(ROOT)/libraries/deflate/deflate.c $(ROOT)/libraries/deflate/deflate.h $(RCC) $(USERHEADERS)
	@mkdir -p $(@D)
	@$(RCC) compile $< -o $@ $(USERFLAGS) >$@.log 2>&1 || { tail -45 $@.log; exit 1; }
