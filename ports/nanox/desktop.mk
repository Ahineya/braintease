# Real upstream Nano-X server/client split with the private C16 local protocol.
# Register new clients here so adding one does not invalidate every FUZIX
# application through the main Makefile dependency.
APPS += nxroach
userspace: $(BUILD)/nxroach.rxe
$(BUILD)/apps.json: $(ROOT)/ports/nanox/desktop.mk
NX_SERVER_NAMES := srvmain srvfunc srvutil srvevent srvclip srvnet devmouse devkbd devtimer server
NX_CLIENT_NAMES := client nxproto nxdraw nxpaintnc nxutil client-os
NX_WM_NAMES := wmaction wmclients wmevents wmutil
NX_SERVER_OBJS := $(addprefix $(BUILD)/nanox/desktop/,$(addsuffix .pobj,$(NX_SERVER_NAMES)))
NX_CLIENT_OBJS := $(addprefix $(BUILD)/nanox/client/,$(addsuffix .pobj,$(NX_CLIENT_NAMES)))
NX_WM_OBJS := $(addprefix $(BUILD)/nanox/client/,$(addsuffix .pobj,$(NX_WM_NAMES)))
NX_ENGINE_OBJS := $(filter-out $(BUILD)/nanox/screen.pobj $(BUILD)/nanox/nxdraw.pobj,$(NX_OBJS)) $(BUILD)/nanox/desktop/screen.pobj
NX_DESKTOP_FLAGS := $(NX_FLAGS) -I$(NX_SRC)/nanox -I$(NX_SRC)/demos/nanox -DNXR_DESKTOP=1
$(NX_SRC)/.stamp: $(NX)/server_adapt.py $(wildcard $(NX)/upstream/src/nanox/*.[ch]) $(wildcard $(NX)/upstream/src/demos/nanox/nxlaunch.*) $(NX)/upstream/src/demos/nanox/nanowm.c $(wildcard $(NX)/upstream/src/engine/dev*.c)
$(NX_SRC)/.stamp: $(NX)/apps_adapt.py $(wildcard $(NX)/upstream/src/demos/nanox/nxeyes.*) $(NX)/upstream/src/demos/nanox/nxclock.c
$(NX_SRC)/.stamp: $(NX)/roach_adapt.py $(wildcard $(NX)/upstream/src/demos/nanox/nxroach.*) $(wildcard $(NX)/upstream/src/images/demos/nanox/nxroach/*.xbm)
$(NX_SERVER_OBJS:.pobj=.asm) $(BUILD)/nanox/desktop/screen.asm: $(BUILD)/nanox/desktop/%.asm: $(NX_SRC)/.stamp $(USERHEADERS) $(RCC) $(NX)/ripple/server.h $(NX)/desktop.mk
	@mkdir -p $(@D)
	@printf '  RCC Nano-X server %s\n' '$*'
	@$(RCC) compile $(if $(filter server screen,$*),$(NX)/ripple/$*.c,$(if $(filter devmouse devkbd devtimer,$*),$(NX_SRC)/engine/$*.c,$(NX_SRC)/nanox/$*.c)) -o $@ $(NX_DESKTOP_FLAGS) -DNXR_SERVER=1 >$@.log 2>&1 || { tail -45 $@.log; exit 1; }
$(BUILD)/nanox/desktop/server.asm: $(NX)/ripple/server.c $(NX)/ripple/requests.inc
$(BUILD)/nanox/desktop/screen.asm: $(NX)/ripple/screen.c
$(NX_CLIENT_OBJS:.pobj=.asm) $(NX_WM_OBJS:.pobj=.asm): $(BUILD)/nanox/client/%.asm: $(NX_SRC)/.stamp $(USERHEADERS) $(RCC) $(NX)/desktop.mk
	@mkdir -p $(@D)
	@printf '  RCC Nano-X client %s\n' '$*'
	@$(RCC) compile $(if $(filter client-os,$*),$(NX)/ripple/client-os.c,$(NX_SRC)/nanox/$*.c) -o $@ $(NX_DESKTOP_FLAGS) >$@.log 2>&1 || { tail -45 $@.log; exit 1; }
$(BUILD)/nanox/desktop/%.pobj: $(BUILD)/nanox/desktop/%.asm $(RASM)
	@$(RASM) assemble $< -o $@ --bank-size 64000 --max-immediate 65535 >$@.log 2>&1 || { tail -45 $@.log; exit 1; }
$(BUILD)/nanox/client/%.pobj: $(BUILD)/nanox/client/%.asm $(RASM)
	@$(RASM) assemble $< -o $@ --bank-size 64000 --max-immediate 65535 >$@.log 2>&1 || { tail -45 $@.log; exit 1; }
$(BUILD)/nano-X.rxe: $(BUILD)/user/entry.pobj $(NX_SERVER_OBJS) $(NX_ENGINE_OBJS) $(BUILD)/nanox/client/nxdraw.pobj $(BUILD)/user/userlib.pobj $(LIBC_OBJS) $(RLINK)
	@$(RLINK) $(filter %.pobj,$^) -f rxe --bank-size 64000 -o $@ >$@.log 2>&1 || { tail -60 $@.log; exit 1; }
.PRECIOUS: $(BUILD)/nanox/desktop/%.asm $(BUILD)/nanox/desktop/%.pobj $(BUILD)/nanox/client/%.asm $(BUILD)/nanox/client/%.pobj

NX_APP_NAMES := nanowm nxlaunch nxterm nxcheck startx nxeyes nxclock nxroach nxroachcheck
NX_APP_OBJS := $(addprefix $(BUILD)/nanox/app/,$(addsuffix .pobj,$(NX_APP_NAMES)))
$(NX_APP_OBJS:.pobj=.asm): $(BUILD)/nanox/app/%.asm: $(NX_SRC)/.stamp $(USERHEADERS) $(RCC) $(NX)/desktop.mk
	@mkdir -p $(@D)
	@$(RCC) compile $(if $(filter nanowm nxlaunch nxeyes nxclock nxroach,$*),$(NX_SRC)/demos/nanox/$*.c,$(NX)/ripple/$*.c) -o $@ $(if $(filter nxeyes nxroach,$*),$(filter-out -DNO_64BIT,$(NX_DESKTOP_FLAGS)),$(NX_DESKTOP_FLAGS)) >$@.log 2>&1 || { tail -50 $@.log; exit 1; }
$(BUILD)/nanox/app/nxterm.asm: $(NX)/ripple/nxterm.c $(ROOT)/ports/graphics/terminal.c $(ROOT)/ports/graphics/terminal.h
$(BUILD)/nanox/app/nxcheck.asm: $(NX)/ripple/nxcheck.c
$(BUILD)/nanox/app/startx.asm: $(NX)/ripple/startx.c
$(BUILD)/nanox/app/nxroach.asm: $(NX)/ripple/roach-platform.inc
$(BUILD)/nanox/app/nxroachcheck.asm: $(NX)/ripple/nxroachcheck.c
$(BUILD)/nanox/app/%.pobj: $(BUILD)/nanox/app/%.asm $(RASM)
	@$(RASM) assemble $< -o $@ --bank-size 64000 --max-immediate 65535 >$@.log 2>&1 || { tail -45 $@.log; exit 1; }
$(BUILD)/nanowm.rxe: $(BUILD)/user/entry.pobj $(BUILD)/nanox/app/nanowm.pobj $(NX_WM_OBJS) $(NX_CLIENT_OBJS) $(BUILD)/user/userlib.pobj $(LIBC_OBJS) $(RLINK)
	@$(RLINK) $(filter %.pobj,$^) -f rxe --bank-size 64000 -o $@ >$@.log 2>&1 || { tail -60 $@.log; exit 1; }
$(BUILD)/nxlaunch.rxe $(BUILD)/nxterm.rxe $(BUILD)/nxcheck.rxe $(BUILD)/nxclock.rxe $(BUILD)/nxroachcheck.rxe: $(BUILD)/%.rxe: $(BUILD)/user/entry.pobj $(BUILD)/nanox/app/%.pobj $(NX_CLIENT_OBJS) $(BUILD)/user/userlib.pobj $(LIBC_OBJS) $(RLINK)
	@$(RLINK) $(filter %.pobj,$^) -f rxe --bank-size 64000 -o $@ >$@.log 2>&1 || { tail -60 $@.log; exit 1; }
$(BUILD)/nxeyes.rxe $(BUILD)/nxroach.rxe: $(BUILD)/%.rxe: $(BUILD)/user/entry.pobj $(BUILD)/nanox/app/%.pobj $(NX_CLIENT_OBJS) $(BUILD)/user/userlib.pobj $(FLOAT_LIB_OBJS) $(filter-out $(BUILD)/libc/format.pobj,$(LIBC_OBJS)) $(RLINK)
	@$(RLINK) $(filter %.pobj,$^) -f rxe --bank-size 64000 -o $@ >$@.log 2>&1 || { tail -60 $@.log; exit 1; }
$(BUILD)/startx.rxe: $(BUILD)/user/entry.pobj $(BUILD)/nanox/app/startx.pobj $(BUILD)/user/userlib.pobj $(LIBC_OBJS) $(RLINK)
	@$(RLINK) $(filter %.pobj,$^) -f rxe --bank-size 64000 -o $@ >$@.log 2>&1 || { tail -60 $@.log; exit 1; }
.PRECIOUS: $(BUILD)/nanox/app/%.asm $(BUILD)/nanox/app/%.pobj

$(BUILD)/nanox/client/client-os.asm: $(NX)/ripple/client-os.c

.PHONY: test-desktop test-roach
test-desktop test-roach: kernel bios $(addprefix $(BUILD)/,$(addsuffix .rxe,init sh echo cat ls kill sleep stty ps true vi gterm nano-X nanowm nxlaunch nxterm startx nxcheck nxeyes nxclock nxroach nxroachcheck))
test-desktop:
	python3 $(NX)/tools/test_desktop.py --build $(BUILD)
test-roach:
	python3 $(NX)/tools/test_desktop.py --build $(BUILD) --only roach
