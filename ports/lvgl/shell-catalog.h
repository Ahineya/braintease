/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef GRAPHITE_SHELL_CATALOG_H
#define GRAPHITE_SHELL_CATALOG_H
#include <stddef.h>
#include <sys/types.h>

typedef struct ShellCatalog ShellCatalog;
enum ShellAppKind { SHELL_APP_GUI, SHELL_APP_TERMINAL };
typedef struct ShellApp {
    char *id, *name, *icon, *category, *keywords;
    char *executable, *fallback;
    char **arguments;
    size_t argument_count;
    enum ShellAppKind kind;
    unsigned terminal_cols, terminal_rows;
    int pinned_default;
} ShellApp;
enum ShellLaunchState { SHELL_LAUNCH_STARTED = 1, SHELL_LAUNCH_FAILED, SHELL_LAUNCH_EXITED };
enum ShellRunMode { SHELL_RUN_AUTO, SHELL_RUN_TERMINAL };
typedef struct ShellLaunchEvent {
    pid_t pid;
    enum ShellLaunchState state;
    int error, status;
    char app_id[64];
} ShellLaunchEvent;

ShellCatalog *shell_catalog_new(void);
void shell_catalog_free(ShellCatalog *catalog);
/* Append a single .ini or all .ini files in a directory atomically. Duplicate
 * IDs/keys, unknown keys, malformed arguments and unsafe executable paths fail.
 * Entries sort by category, display name and ID; no arbitrary entry limit. */
int shell_catalog_load(ShellCatalog *catalog, const char *path);
const char *shell_catalog_error(const ShellCatalog *catalog);
size_t shell_catalog_count(const ShellCatalog *catalog);
const ShellApp *shell_catalog_at(const ShellCatalog *catalog, size_t index);
const ShellApp *shell_catalog_find(const ShellCatalog *catalog, const char *id);
int shell_app_matches(const ShellApp *app, const char *query);
/* Returns the selected executable path or NULL, checking X_OK on each call. */
const char *shell_app_available(const ShellCatalog *catalog, const ShellApp *app);
int shell_catalog_terminal(ShellCatalog *catalog, const char *installed,
                           const char *fallback);

/* [pins] app.id=true/false overrides each application's pinned default.
 * Unknown valid IDs survive load/save so unavailable apps retain preferences.
 * A missing file is an empty override. Save atomically renames a sibling file. */
int shell_pins_load(ShellCatalog *catalog, const char *path);
int shell_pin_set(ShellCatalog *catalog, const char *id, int pinned);
int shell_app_pinned(const ShellCatalog *catalog, const ShellApp *app);
int shell_pins_save(ShellCatalog *catalog, const char *path);

/* Exact argv only: never invokes a shell. Terminal applications use gterm with
 * explicit application metadata. No blocking wait/read in the parent's path. */
pid_t shell_catalog_launch(ShellCatalog *catalog, const ShellApp *app);
/* Run a quoted command line (up to 4096 cells), without shell evaluation.
 * Spaces/tabs separate arguments; single/double quotes and backslash preserve
 * literal arguments. No variables, globs, pipes or substitutions are expanded.
 * Resolve catalog ID/name/executable first, then explicit paths or parent PATH
 * (fallback /usr/bin:/bin). AUTO preserves known app kind and directly executes
 * unknown programs; TERMINAL forces gterm. Errors use shell_catalog_error().
 * Lifecycle events share shell_catalog_poll(); returned PID identifies the run. */
pid_t shell_catalog_run(ShellCatalog *catalog, const char *command,
                        enum ShellRunMode mode);
/* 1 event, 0 idle; STARTED/FAILED precedes EXITED for each child. Reaps only
 * children launched by this catalog. Returned status is waitpid's raw status. */
int shell_catalog_poll(ShellCatalog *catalog, ShellLaunchEvent *event);
#endif
