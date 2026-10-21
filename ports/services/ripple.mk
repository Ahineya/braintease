# Explicit opt-in targets only. Never added to APPS, init, disk or x86 builds.
SERVICE_SRC := $(ROOT)/ports/services
SERVICE_OUT := $(ROOT)/build/services/ripple
$(SERVICE_OUT)/%.asm: $(SERVICE_SRC)/%.c $(SERVICE_SRC)/service.h $(ROOT)/libraries/ini/ini.h $(USERHEADERS) $(RCC)
	@mkdir -p $(@D)
	$(RCC) compile $< -o $@ $(USERFLAGS) -I$(ROOT)/libraries/ini
$(SERVICE_OUT)/ini.asm: $(ROOT)/libraries/ini/ini.c $(ROOT)/libraries/ini/ini.h $(USERHEADERS) $(RCC)
	@mkdir -p $(@D)
	$(RCC) compile $< -o $@ $(USERFLAGS)
$(SERVICE_OUT)/%.pobj: $(SERVICE_OUT)/%.asm
	$(RASM) assemble $< -o $@ --bank-size 64000 --max-immediate 65535
$(SERVICE_OUT)/svcd.rxe: $(BUILD)/user/entry.pobj $(SERVICE_OUT)/manager.pobj $(SERVICE_OUT)/config.pobj $(SERVICE_OUT)/ini.pobj $(BUILD)/user/userlib.pobj $(LIBC_OBJS) $(RLINK)
	$(RXELINK) $(filter %.pobj,$^) -f rxe --bank-size 64000 -o $@
$(SERVICE_OUT)/svc.rxe: $(BUILD)/user/entry.pobj $(SERVICE_OUT)/client.pobj $(SERVICE_OUT)/config.pobj $(SERVICE_OUT)/ini.pobj $(BUILD)/user/userlib.pobj $(LIBC_OBJS) $(RLINK)
	$(RXELINK) $(filter %.pobj,$^) -f rxe --bank-size 64000 -o $@

$(SERVICE_OUT)/console-session.rxe: $(BUILD)/user/entry.pobj $(SERVICE_OUT)/console.pobj $(SERVICE_OUT)/config.pobj $(SERVICE_OUT)/ini.pobj $(BUILD)/user/userlib.pobj $(LIBC_OBJS) $(RLINK)
	$(RXELINK) $(filter %.pobj,$^) -f rxe --bank-size 64000 -o $@

$(SERVICE_OUT)/helper.asm: $(SERVICE_SRC)/tests/helper.c $(SERVICE_SRC)/service.h $(USERHEADERS) $(RCC)
	@mkdir -p $(@D)
	$(RCC) compile $< -o $@ $(USERFLAGS)
$(SERVICE_OUT)/sv-helper.rxe: $(BUILD)/user/entry.pobj $(SERVICE_OUT)/helper.pobj $(BUILD)/user/userlib.pobj $(LIBC_OBJS) $(RLINK)
	$(RXELINK) $(filter %.pobj,$^) -f rxe --bank-size 64000 -o $@
