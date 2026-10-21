/* SPDX-License-Identifier: MIT */
#ifndef GRAPHITE_UI_FORMS_H
#define GRAPHITE_UI_FORMS_H
#include "ui-data.h"

#define UI_CHOICE_LIMIT 65
typedef struct ui_choice ui_choice;
/* Owns copies of the UTF-8 labels. Selection changes emit VALUE_CHANGED on
 * the root. Arrows/Home/End select; Enter/Space opens the shared popup menu.
 * The root owns cleanup; its width belongs to the surrounding layout. */
ui_choice *ui_choice_create(lv_obj_t *parent, const char *const *labels,
                            unsigned count, unsigned selected);
int ui_choice_bind(ui_choice *choice, const char *const *labels,
                    unsigned count, unsigned selected);
lv_obj_t *ui_choice_object(ui_choice *choice);
unsigned ui_choice_selected(ui_choice *choice);
void ui_choice_select(ui_choice *choice, unsigned selected);
void ui_choice_density(ui_choice *choice);

enum ui_path_mode { UI_PATH_OPEN, UI_PATH_SAVE };
#define UI_PATH_LIMIT 2048
typedef struct ui_path ui_path;
/* A non-NULL path has passed regular-file/parent-directory validation; return
 * 1 to accept, or set ui_path_error and return 0 to keep editing. NULL means
 * cancelled (return ignored). Callback paths are borrowed until it returns.
 * Save confirms replacement within the same modal, before calling the app.
 * This validates intent, not race-free file creation: the app owns its actual
 * write/atomic replacement and must report failures here. Do not close the
 * dialog from inside the callback; return its desired closing state instead. */
typedef int (*ui_path_fn)(ui_path *, const char *path, void *user);
typedef void (*ui_path_browse_fn)(ui_path *, void *user);
ui_path *ui_path_open(const char *title, unsigned mode, const char *initial,
                       ui_path_fn accept, void *user);
lv_obj_t *ui_path_input(ui_path *path);
/* The shared guest-namespace browser is built into every Open/Save dialog.
 * The existing input accepts a filename or full path; relative names resolve
 * against the displayed folder. Existing directories navigate on Enter.
 * Accepted paths are absolute. Scanning never changes process cwd. */
ui_table *ui_path_list(ui_path *path);
lv_obj_t *ui_path_location_input(ui_path *path);
const char *ui_path_directory(ui_path *path);
int ui_path_loading(ui_path *path);
/* Replaces the input exactly, never silently truncates. Overlength initial
 * or replacement text remains editable and is rejected on submission. */
void ui_path_set(ui_path *path, const char *text);
void ui_path_error(ui_path *path, const char *message);
/* Optional platform/file-picker integration. The browse callback may later
 * feed a chosen path through ui_path_set. It must not open another modal. */
lv_obj_t *ui_path_browse(ui_path *path, ui_path_browse_fn browse, void *user);
void ui_path_submit(ui_path *path);
void ui_path_cancel(ui_path *path);
void ui_path_density(ui_path *path);

typedef struct ui_progress ui_progress;
typedef void (*ui_progress_cancel_fn)(ui_progress *, void *user);
/* Modeless bounded-work status. The app performs work in bounded steps and
 * updates progress; this widget never runs jobs or fabricates progress.
 * total=0 hides the bar. A non-NULL cancel callback adds a Cancel button.
 * Deleting the root owns cleanup. Cancel is delivered at most once. */
ui_progress *ui_progress_create(lv_obj_t *parent, ui_progress_cancel_fn cancel, void *user);
lv_obj_t *ui_progress_object(ui_progress *progress);
void ui_progress_update(ui_progress *progress, const char *text,
                         uint32_t done, uint32_t total);
/* Job boundaries: starting clears cancellation and enables Cancel; stopping
 * disables it without changing the last displayed progress. */
void ui_progress_running(ui_progress *progress, int running);
/* Returns 1 when an available Cancel button takes keyboard focus. */
int ui_progress_focus_cancel(ui_progress *progress);
int ui_progress_cancelled(ui_progress *progress);
void ui_progress_density(ui_progress *progress);
#endif
