# Included by the FUZIX makefile; generated sources preserve the pinned import.
NX := $(ROOT)/ports/nanox
NX_SRC := $(BUILD)/nanox/src
NX_NAMES := devopen devdraw devclip devrgn devblit devfont genfont rom8x16 screen nxdraw
NX_OBJS := $(addprefix $(BUILD)/nanox/,$(addsuffix .pobj,$(NX_NAMES)))
NX_FLAGS := $(USERFLAGS) -I$(NX_SRC)/include -I$(NX_SRC)/drivers -I$(NX)/ripple -DMWCONFIG_FILE='"nxr_config.h"' -DMWPIXEL_FORMAT=MWPF_TRUECOLOR565
NX_INPUTS := $(wildcard $(NX)/upstream/src/include/*.h) $(wildcard $(NX)/upstream/src/engine/dev*.c) $(NX)/upstream/src/engine/devfont.h $(NX)/upstream/src/drivers/genfont.c $(NX)/upstream/src/drivers/fb.h $(NX)/upstream/src/fonts/compiled/rom8x16.c
$(NX_SRC)/.stamp: $(NX)/adapt.py $(NX_INPUTS)
	@python3 $(NX)/adapt.py $(NX_SRC)
	@touch $@
$(BUILD)/nanox/%.asm: $(NX_SRC)/.stamp $(USERHEADERS) $(RCC) $(NX)/ripple/nxr_config.h $(NX)/ripple/screen.h $(NX)/engine.mk
	@mkdir -p $(@D)
	@printf "  RCC Nano-X %s\n" "$*"
	@$(RCC) compile $(if $(filter genfont,$*),$(NX_SRC)/drivers/genfont.c,$(if $(filter rom8x16,$*),$(NX_SRC)/fonts/compiled/rom8x16.c,$(if $(filter screen nxdraw,$*),$(NX)/ripple/$*.c,$(NX_SRC)/engine/$*.c))) -o $@ $(NX_FLAGS) $(if $(filter devblit,$*),-DGdBlit=nxr_engine_blit) >$@.log 2>&1 || { tail -60 $@.log; exit 1; }
$(BUILD)/nanox/screen.asm: $(NX)/ripple/screen.c
$(BUILD)/nanox/nxdraw.asm: $(NX)/ripple/nxdraw.c
$(BUILD)/nxdraw.rxe: $(BUILD)/user/entry.pobj $(NX_OBJS) $(BUILD)/user/userlib.pobj $(LIBC_OBJS) $(RLINK)
	@$(RLINK) $(filter %.pobj,$^) -f rxe --bank-size 64000 -o $@ >$@.log 2>&1 || { tail -60 $@.log; exit 1; }
userspace: $(BUILD)/nxdraw.rxe
.PRECIOUS: $(BUILD)/nanox/%.asm $(BUILD)/nanox/%.pobj
.PHONY: test-nanox
test-nanox: images
	python3 $(NX)/tools/test_engine.py --build $(BUILD)

include $(ROOT)/ports/nanox/desktop.mk
