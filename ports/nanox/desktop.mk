# Real upstream Nano-X server/client split with the private C16 local protocol.
# Register new clients here so adding one does not invalidate every Riplix
# application through the main Makefile dependency.
FDE := $(ROOT)/ports/fusede
APPS += nxroach nxdsktop nxmsg nxselect nxcalc nxtetris nxview fdeshowcase fdectl
userspace: $(addprefix $(BUILD)/,$(addsuffix .rxe,nxroach nxdsktop nxmsg nxselect nxcalc nxtetris nxview fdeshowcase fdectl))
$(BUILD)/apps.json: $(ROOT)/ports/nanox/desktop.mk
NX_SERVER_NAMES := srvmain srvfunc srvutil srvevent srvclip srvnet devmouse devkbd devtimer server image-server workspace-server clipboard-server raster
NX_CLIENT_NAMES := client nxproto nxdraw nxpaintnc nxutil client-os skin
NX_WM_NAMES := wmaction wmclients wmevents wmutil
NX_SERVER_OBJS := $(addprefix $(BUILD)/nanox/desktop/,$(addsuffix .pobj,$(NX_SERVER_NAMES)))
NX_CLIENT_OBJS := $(addprefix $(BUILD)/nanox/client/,$(addsuffix .pobj,$(NX_CLIENT_NAMES)))
NX_WM_OBJS := $(addprefix $(BUILD)/nanox/client/,$(addsuffix .pobj,$(NX_WM_NAMES)))
NX_ENGINE_OBJS := $(filter-out $(BUILD)/nanox/screen.pobj $(BUILD)/nanox/nxdraw.pobj,$(NX_OBJS)) $(BUILD)/nanox/desktop/screen.pobj
NX_DESKTOP_FLAGS := $(NX_FLAGS) -I$(NX_SRC)/nanox -I$(NX_SRC)/demos/nanox -I$(FDE)/include -I$(BUILD)/fusede -DNXR_DESKTOP=1
$(NX_SRC)/.stamp: $(NX)/image_adapt.py $(NX)/clipboard_adapt.py
$(BUILD)/nanox/desktop/clipboard-server.asm: $(NX)/ripple/clipboard-server.c $(NX)/ripple/clipboard-wire.h $(NX)/ripple/nx_clipboard.h
$(BUILD)/nanox/desktop/server.asm $(BUILD)/nanox/app/nxterm.asm: $(NX)/ripple/nx_clipboard.h
$(BUILD)/nanox/client/client.asm: $(NX)/ripple/clipboard-client.inc $(NX)/ripple/clipboard-wire.h $(NX)/ripple/nx_clipboard.h
$(NX_SRC)/.stamp: $(NX)/workspace_adapt.py
$(NX_SRC)/.stamp: $(NX)/event_adapt.py
$(BUILD)/nanox/desktop/srvevent.asm: $(NX)/ripple/event-queue.inc
$(NX_SRC)/.stamp: $(NX)/paint_adapt.py $(NX)/server_adapt.py $(wildcard $(NX)/upstream/src/nanox/*.[ch]) $(wildcard $(NX)/upstream/src/demos/nanox/nxlaunch.*) $(NX)/upstream/src/demos/nanox/nanowm.c $(wildcard $(NX)/upstream/src/engine/dev*.c)
$(NX_SRC)/.stamp: $(NX)/apps_adapt.py $(wildcard $(NX)/upstream/src/demos/nanox/nxeyes.*) $(NX)/upstream/src/demos/nanox/nxclock.c
$(NX_SRC)/.stamp: $(NX)/roach_adapt.py $(wildcard $(NX)/upstream/src/demos/nanox/nxroach.*) $(wildcard $(NX)/upstream/src/images/demos/nanox/nxroach/*.xbm)
$(NX_SRC)/.stamp: $(NX)/desktop_adapt.py $(addprefix $(NX)/upstream/src/demos/nanox/,$(addsuffix .c,nxdsktop nxmsg nxselect nxcalc))
$(NX_SRC)/.stamp: $(NX)/tetris_adapt.py $(NX)/upstream/src/demos/nanox/nxtetris.c $(NX)/upstream/src/demos/nanox/nxtetris.h
$(NX_SERVER_OBJS:.pobj=.asm) $(BUILD)/nanox/desktop/screen.asm: $(BUILD)/nanox/desktop/%.asm: $(NX_SRC)/.stamp $(USERHEADERS) $(RCC) $(NX)/ripple/server.h $(NX)/desktop.mk
	@mkdir -p $(@D)
	@printf '  RCC Nano-X server %s\n' '$*'
	@$(RCC) compile $(if $(filter raster,$*),$(FDE)/raster.c,$(if $(filter server screen image-server workspace-server clipboard-server,$*),$(NX)/ripple/$*.c,$(if $(filter devmouse devkbd devtimer,$*),$(NX_SRC)/engine/$*.c,$(NX_SRC)/nanox/$*.c))) -o $@ $(NX_DESKTOP_FLAGS) -DNXR_SERVER=1 >$@.log 2>&1 || { tail -45 $@.log; exit 1; }
$(BUILD)/nanox/desktop/server.asm: $(NX)/ripple/server.c $(NX)/ripple/requests.inc
$(BUILD)/nanox/desktop/screen.asm: $(NX)/ripple/screen.c
$(BUILD)/nanox/desktop/image-server.asm: $(NX)/ripple/image-server.c $(NX)/ripple/image-wire.h $(FDE)/include/fde_raster.h $(ROOT)/libraries/image/pixel.h
$(BUILD)/nanox/desktop/raster.asm: $(FDE)/raster.c $(FDE)/include/fde_raster.h
$(BUILD)/nanox/desktop/workspace-server.asm: $(NX)/ripple/workspace-server.c $(NX)/ripple/workspace-wire.h $(FDE)/include/fde_workspace.h
$(BUILD)/nanox/client/client.asm: $(NX)/ripple/workspace-client.inc $(NX)/ripple/workspace-wire.h $(FDE)/include/fde_workspace.h
$(BUILD)/nanox/client/wmaction.asm: $(NX)/ripple/wm-workspace.inc $(FDE)/include/fde_workspace.h
$(BUILD)/nanox/client/wmaction.asm: $(BUILD)/fusede/theme.h
$(NX_WM_OBJS:.pobj=.asm) $(BUILD)/nanox/app/nanowm.asm: $(FDE)/include/fde_workspace.h
$(NX_CLIENT_OBJS:.pobj=.asm) $(NX_WM_OBJS:.pobj=.asm): $(BUILD)/nanox/client/%.asm: $(NX_SRC)/.stamp $(USERHEADERS) $(RCC) $(NX)/desktop.mk
	@mkdir -p $(@D)
	@printf '  RCC Nano-X client %s\n' '$*'
	@$(RCC) compile $(if $(filter skin,$*),$(FDE)/skin.c,$(if $(filter client-os,$*),$(NX)/ripple/client-os.c,$(NX_SRC)/nanox/$*.c)) -o $@ $(NX_DESKTOP_FLAGS) >$@.log 2>&1 || { tail -45 $@.log; exit 1; }
$(BUILD)/nanox/client/client.asm: $(NX)/ripple/image-client.inc $(NX)/ripple/image-wire.h $(FDE)/image-file.inc $(wildcard $(FDE)/include/*.h)
$(BUILD)/nanox/client/skin.asm: $(FDE)/skin.c $(FDE)/include/fde_skin.h
$(BUILD)/nanox/desktop/%.pobj: $(BUILD)/nanox/desktop/%.asm $(RASM)
	@$(RASM) assemble $< -o $@ --bank-size 64000 --max-immediate 65535 >$@.log 2>&1 || { tail -45 $@.log; exit 1; }
$(BUILD)/nanox/client/%.pobj: $(BUILD)/nanox/client/%.asm $(RASM)
	@$(RASM) assemble $< -o $@ --bank-size 64000 --max-immediate 65535 >$@.log 2>&1 || { tail -45 $@.log; exit 1; }
$(BUILD)/nano-X.rxe: $(BUILD)/user/entry.pobj $(NX_SERVER_OBJS) $(NX_ENGINE_OBJS) $(BUILD)/nanox/client/nxdraw.pobj $(BUILD)/user/userlib.pobj $(LIBC_OBJS) $(RLINK)
	@$(RXELINK) $(filter %.pobj,$^) -f rxe --bank-size 64000 -o $@ >$@.log 2>&1 || { tail -60 $@.log; exit 1; }
.PRECIOUS: $(BUILD)/nanox/desktop/%.asm $(BUILD)/nanox/desktop/%.pobj $(BUILD)/nanox/client/%.asm $(BUILD)/nanox/client/%.pobj

NX_APP_NAMES := nanowm nxlaunch nxterm nxcheck startx nxeyes nxclock nxroach nxroachcheck nxdsktop nxmsg nxselect nxcalc nxtetris nxview fdeshowcase fdectl
NX_APP_OBJS := $(addprefix $(BUILD)/nanox/app/,$(addsuffix .pobj,$(NX_APP_NAMES)))
$(NX_APP_OBJS:.pobj=.asm): $(BUILD)/nanox/app/%.asm: $(NX_SRC)/.stamp $(USERHEADERS) $(RCC) $(NX)/desktop.mk
	@mkdir -p $(@D)
	@$(RCC) compile $(if $(filter fdectl,$*),$(FDE)/workspace.c,$(if $(filter fdeshowcase,$*),$(FDE)/showcase.c,$(if $(filter nanowm nxlaunch nxeyes nxclock nxroach nxdsktop nxmsg nxselect nxcalc nxtetris,$*),$(NX_SRC)/demos/nanox/$*.c,$(NX)/ripple/$*.c))) -o $@ $(if $(filter nxeyes nxroach nxcalc,$*),$(filter-out -DNO_64BIT,$(NX_DESKTOP_FLAGS)),$(NX_DESKTOP_FLAGS)) >$@.log 2>&1 || { tail -50 $@.log; exit 1; }
$(BUILD)/fusede/theme.h: $(FDE)/tools/build_theme.py $(FDE)/tools/preview.html $(FDE)/themes/slate.json $(FDE)/skin.c $(FDE)/include/fde_skin.h
	python3 $(FDE)/tools/build_theme.py --output $(BUILD)/fusede
$(BUILD)/fusede/icons.h: $(FDE)/tools/build_icons.py $(FDE)/icons/masks.json $(wildcard $(FDE)/icons/lucide/*)
	python3 $(FDE)/tools/build_icons.py --output $(BUILD)/fusede
$(BUILD)/nanox/app/fdeshowcase.asm: $(FDE)/showcase.c $(FDE)/imagecheck.inc $(wildcard $(FDE)/include/*.h) $(BUILD)/fusede/theme.h $(BUILD)/fusede/icons.h
$(BUILD)/apps/gterm.asm $(BUILD)/nanox/app/nxterm.asm: $(ROOT)/ports/graphics/terminal-input.h $(ROOT)/ports/graphics/terminal-selection.h
$(BUILD)/nanox/app/nxterm.asm: $(NX)/ripple/nxterm.c $(ROOT)/ports/graphics/terminal.c $(ROOT)/ports/graphics/terminal.h
$(BUILD)/nanox/app/nxcheck.asm: $(NX)/ripple/nxcheck.c
$(BUILD)/nanox/app/startx.asm: $(NX)/ripple/startx.c
$(BUILD)/nanox/app/nxroach.asm: $(NX)/ripple/roach-platform.inc
$(BUILD)/nanox/app/nxroachcheck.asm: $(NX)/ripple/nxroachcheck.c
$(BUILD)/nanox/app/nxdsktop.asm: $(NX)/ripple/nxdsktop-platform.h $(NX)/ripple/nxdsktop-platform.inc
$(BUILD)/nanox/app/nxcalc.asm: $(NX)/ripple/calc-platform.inc
$(BUILD)/nanox/app/nxtetris.asm: $(NX)/ripple/tetris-platform.h $(NX)/ripple/tetris-platform.inc
$(BUILD)/nanox/app/nxview.asm: $(NX)/ripple/nxview.c $(NX)/ripple/nxview-model.h $(ROOT)/libraries/image/image.h $(ROOT)/libraries/image/nano.h
NX_IMAGE_INPUTS := $(wildcard $(ROOT)/libraries/image/*.[ch] $(ROOT)/libraries/image/*.inc $(ROOT)/libraries/image/vendor/* $(ROOT)/libraries/deflate/*.[ch])
$(BUILD)/nanox/image/.stamp: $(ROOT)/libraries/image/adapt.py $(wildcard $(ROOT)/libraries/image/vendor/*)
	@mkdir -p $(@D)
	@python3 $(ROOT)/libraries/image/adapt.py
	@touch $@
$(addprefix $(BUILD)/nanox/image/,$(addsuffix .asm,decode nano)): $(BUILD)/nanox/image/%.asm: $(ROOT)/libraries/image/%.c $(NX_IMAGE_INPUTS) $(BUILD)/nanox/image/.stamp $(NX_SRC)/.stamp $(RCC) $(USERHEADERS) $(NX)/desktop.mk
	@mkdir -p $(@D)
	@$(RCC) compile $< -o $@ $(NX_DESKTOP_FLAGS) >$@.log 2>&1 || { tail -50 $@.log; exit 1; }
$(BUILD)/nanox/image/%.pobj: $(BUILD)/nanox/image/%.asm $(RASM)
	@$(RASM) assemble $< -o $@ --bank-size 64000 --max-immediate 65535 >$@.log 2>&1 || { tail -45 $@.log; exit 1; }
$(BUILD)/nxview.rxe: $(BUILD)/user/entry.pobj $(BUILD)/nanox/app/nxview.pobj $(BUILD)/nanox/image/decode.pobj $(BUILD)/nanox/image/nano.pobj $(NX_CLIENT_OBJS) $(BUILD)/user/userlib.pobj $(LIBC_OBJS) $(RLINK)
	@$(RXELINK) $(filter %.pobj,$^) -f rxe --bank-size 64000 -o $@ >$@.log 2>&1 || { tail -60 $@.log; exit 1; }
.PRECIOUS: $(BUILD)/nanox/image/%.asm $(BUILD)/nanox/image/%.pobj
$(BUILD)/nanox/app/%.pobj: $(BUILD)/nanox/app/%.asm $(RASM)
	@$(RASM) assemble $< -o $@ --bank-size 64000 --max-immediate 65535 >$@.log 2>&1 || { tail -45 $@.log; exit 1; }
$(BUILD)/nanowm.rxe: $(BUILD)/user/entry.pobj $(BUILD)/nanox/app/nanowm.pobj $(NX_WM_OBJS) $(NX_CLIENT_OBJS) $(BUILD)/user/userlib.pobj $(LIBC_OBJS) $(RLINK)
	@$(RXELINK) $(filter %.pobj,$^) -f rxe --bank-size 64000 -o $@ >$@.log 2>&1 || { tail -60 $@.log; exit 1; }
$(BUILD)/nxlaunch.rxe $(BUILD)/nxterm.rxe $(BUILD)/nxcheck.rxe $(BUILD)/nxclock.rxe $(BUILD)/nxroachcheck.rxe $(BUILD)/nxdsktop.rxe $(BUILD)/nxmsg.rxe $(BUILD)/nxselect.rxe $(BUILD)/nxtetris.rxe $(BUILD)/fdeshowcase.rxe $(BUILD)/fdectl.rxe: $(BUILD)/%.rxe: $(BUILD)/user/entry.pobj $(BUILD)/nanox/app/%.pobj $(NX_CLIENT_OBJS) $(BUILD)/user/userlib.pobj $(LIBC_OBJS) $(RLINK)
	@$(RXELINK) $(filter %.pobj,$^) -f rxe --bank-size 64000 -o $@ >$@.log 2>&1 || { tail -60 $@.log; exit 1; }
$(BUILD)/nxeyes.rxe $(BUILD)/nxroach.rxe $(BUILD)/nxcalc.rxe: $(BUILD)/%.rxe: $(BUILD)/user/entry.pobj $(BUILD)/nanox/app/%.pobj $(NX_CLIENT_OBJS) $(BUILD)/user/userlib.pobj $(FLOAT_LIB_OBJS) $(filter-out $(BUILD)/libc/format.pobj,$(LIBC_OBJS)) $(RLINK)
	@$(RXELINK) $(filter %.pobj,$^) -f rxe --bank-size 64000 -o $@ >$@.log 2>&1 || { tail -60 $@.log; exit 1; }
$(BUILD)/startx.rxe: $(BUILD)/user/entry.pobj $(BUILD)/nanox/app/startx.pobj $(BUILD)/user/userlib.pobj $(LIBC_OBJS) $(RLINK)
	@$(RXELINK) $(filter %.pobj,$^) -f rxe --bank-size 64000 -o $@ >$@.log 2>&1 || { tail -60 $@.log; exit 1; }
.PRECIOUS: $(BUILD)/nanox/app/%.asm $(BUILD)/nanox/app/%.pobj

$(BUILD)/nanox/client/client-os.asm: $(NX)/ripple/client-os.c

.PHONY: test-desktop test-roach test-nxdsktop test-tetris
test-desktop test-roach test-nxdsktop test-tetris: kernel bios $(addprefix $(BUILD)/,$(addsuffix .rxe,init sh echo cat ls kill sleep stty ps true vi levee gterm nano-X nanowm nxlaunch nxterm startx nxcheck nxeyes nxclock nxroach nxroachcheck nxdsktop nxmsg nxselect nxcalc nxtetris nxview klotski))
test-desktop:
	python3 $(NX)/tools/test_desktop.py --build $(BUILD)
test-roach:
	python3 $(NX)/tools/test_desktop.py --build $(BUILD) --only roach
test-nxdsktop:
	python3 $(NX)/tools/test_desktop.py --build $(BUILD) --only nxdsktop
test-tetris:
	python3 $(NX)/tools/test_desktop.py --build $(BUILD) --only tetris

$(BUILD)/nanox/app/fdectl.asm: $(FDE)/workspace.c $(FDE)/tests/workspacecheck.inc $(FDE)/include/fde_workspace.h

include $(ROOT)/ports/netsurf/package.mk
include $(ROOT)/ports/kolf/package.mk
include $(ROOT)/ports/klotski/package.mk
