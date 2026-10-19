# Keep the maintained browser in the ordinary userspace/image inventory.
APPS += netsurf
userspace: $(BUILD)/netsurf.rxe
$(BUILD)/apps.json: $(ROOT)/ports/netsurf/package.mk
NETSURF_INPUTS := $(shell find $(ROOT)/ports/netsurf $(ROOT)/libraries/image $(ROOT)/libraries/deflate $(ROOT)/libraries/unicode -type f ! -path '*/tests/*' \( -name '*.c' -o -name '*.h' -o -name '*.inc' -o -name '*.py' -o -name '*.json' -o -name '*.css' -o -name '*.pl' -o -name '*.gperf' \))
NETSURF_INPUTS += $(ROOT)/ports/tls/ripple_http.h
NETSURF_INPUTS += $(ROOT)/ports/nanox/clipboard_adapt.py $(addprefix $(ROOT)/ports/nanox/ripple/,nx_clipboard.h clipboard-wire.h clipboard-client.inc)
$(BUILD)/netsurf.rxe: $(NETSURF_INPUTS) $(USERHEADERS) $(BUILD)/user/userlib.pobj $(LIBC_OBJS) $(FLOAT_LIB_OBJS) $(RCC) $(RASM) $(RLINK)
	python3 $(ROOT)/ports/netsurf/package.py --target ripple --compiler $(RCC) -j 3 -o $@
