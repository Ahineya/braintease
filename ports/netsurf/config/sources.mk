# Evaluate upstream's source lists without its host package discovery/build.
NETSURF_USE_DUKTAPE := NO
NETSURF_USE_CURL := NO
NETSURF_FS_BACKING_STORE := NO
include content/Makefile
include utils/Makefile
include utils/http/Makefile
include utils/nsurl/Makefile
include desktop/Makefile
.PHONY: sources
sources:
	@$(foreach source,$(S_CONTENT) $(S_FETCHERS) $(S_UTILS) $(S_HTTP) $(S_NSURL) $(S_DESKTOP) $(S_BROWSER) $(S_IMAGE),$(info $(source)))
