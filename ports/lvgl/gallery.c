/* SPDX-License-Identifier: MIT */
#include "ui.h"
#include "ui-data.h"
#include "ui-app.h"
#include "ui-forms.h"
#include "ui-plot.h"
#include "ui-editor.h"
#include "ui-document.h"
#include "ui-tabs.h"
#include <gx.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static lv_obj_t *sidebar, *main_column, *heading, *subtitle, *content, *status;
static lv_obj_t *navigation[9], *field, *invalid_field, *validation_hint, *check, *list, *items[48];
static lv_obj_t *icon_preview;
static lv_obj_t *icon_buttons[3], *icon_grid;
static ui_table *demo_table;
static lv_obj_t *demo_menu, *demo_title, *demo_input, *demo_compact_input, *demo_error;
static int demo_desc, demo_sort_column = -1, demo_compact;
static char demo_name[200] = "Measurements";
static unsigned preview_folder;
static ui_actions *workspace_actions;
static ui_split *workspace_split, *workspace_preview;
static lv_obj_t *workspace_menu, *workspace_result, *workspace_counter, *workspace_allow;
static unsigned workspace_runs;
static ui_plot *demo_plot;
static plot_model *demo_plot_model;
static ui_choice *plot_mode_choice, *plot_state_choice, *forms_choice;
static ui_path *forms_path;
static ui_progress *forms_progress;
static lv_obj_t *forms_start;
static lv_obj_t *plot_buttons[3], *forms_open_buttons[2];
static lv_timer_t *forms_timer;
static uint32_t forms_done, forms_checksum;
static ui_documents *editor_documents;
static ui_editor *demo_editor;
static ui_tabs *editor_tabs;
static lv_obj_t *editor_wrap;
#define FORMS_SAMPLES 8388608UL
static unsigned page, selected;
static int trace, replace;
static int hidden;
static char project_name[260] = "A small idea";
static char validation_path[260] = "/missing/folder";
static const char *pages[] = {"Controls", "Typography", "Lists", "Icons", "Data", "Workspace", "Plots", "Forms", "Editor"};
static lv_obj_t *line(lv_obj_t *parent);
static const char *descriptions[] = {
    "Familiar controls. Clear feedback.", "A compact, readable hierarchy.",
    "Dense information. Room to navigate.", "Lucide. Clear shapes, shared colours.",
    "Tables, menus and dialogs. Try 100,000 rows.",
    "Shared commands. Resizable, persistent panes.",
    "Explore samples. Keep gaps and peaks visible.",
    "Choose files. Validate paths. Keep work responsive.",
    "Edit text. Keep documents, selection and undo together."};
static void tell(const char *message) {
    lv_label_set_text(status, message);
    if (trace)
        fprintf(stderr, "GALLERY_STATUS %s\n", message);
}
static const char *demo_value(void *unused, uint32_t row, unsigned col) {
    static char text[80];
    uint32_t n = demo_desc ? 99999UL - row : row;
    (void)unused;
    if (col == 0)
        snprintf(text, sizeof(text), "%lu", (unsigned long)n + 1);
    else if (col == 1)
        snprintf(text, sizeof(text), "Sample %06lu", (unsigned long)n + 1);
    else if (col == 2)
        snprintf(text, sizeof(text), "%lu.%02u", (unsigned long)n / 10, (unsigned)(n % 10) * 10);
    else
        snprintf(text, sizeof(text), "Column %u / %06lu", col + 1, (unsigned long)n + 1);
    return text;
}
static void demo_selected(void *unused, uint32_t row, unsigned col) {
    char text[100];
    (void)unused;
    snprintf(text, sizeof(text), "Row %lu of 100000  |  Column %u", (unsigned long)row + 1,
             col + 1);
    tell(text);
    if (trace)
        fprintf(stderr, "GALLERY_TABLE_SELECT %lu %u\n", (unsigned long)row, col);
}
static void demo_sort(void *unused, unsigned col) {
    (void)unused;
    demo_desc = demo_sort_column == (int)col ? !demo_desc : 0;
    demo_sort_column = col;
    ui_table_sort_marker(demo_table, col, demo_desc);
    ui_table_select(demo_table, 0, col, 1);
    if (trace)
        fprintf(stderr, "GALLERY_TABLE_SORT %u %d\n", col, demo_desc);
}
static int demo_options_result(ui_dialog *dialog, unsigned action, void *unused) {
    const char *text;
    unsigned i;
    (void)dialog;
    (void)unused;
    if (!action)
        return 1;
    text = lv_textarea_get_text(demo_input);
    if (!text[0]) {
        ui_input_error(demo_input, 1);
        lv_label_set_text(demo_error, "Enter a dataset title.");
        return 0;
    }
    strcpy(demo_name, text);
    demo_compact = ui_radio_selected(demo_compact_input) == 1;
    lv_label_set_text(demo_title, demo_name);
    for (i = 0; i < 8; i++)
        ui_table_column_width(demo_table, i, demo_compact ? 96 : 144);
    if (trace)
        fprintf(stderr, "GALLERY_DIALOG_APPLY %s compact=%d\n", demo_name, demo_compact);
    return 1;
}
static void demo_command(unsigned action, void *unused) {
    (void)unused;
    if (action == 1) {
        char text[100];
        strcpy(text, demo_value(NULL, ui_table_row(demo_table), ui_table_column(demo_table)));
        gx_lv_clipboard(text, sizeof(text), 1);
        tell("Cell copied");
    } else if (action == 2)
        ui_table_select(demo_table, 0, 0, 1);
    else if (action == 3)
        ui_table_select(demo_table, 99999UL, 7, 1);
    else if (action == 4) {
        ui_dialog *dialog = ui_dialog_open("Table options", 400, demo_options_result, NULL);
        lv_obj_t *body;
        if (!dialog)
            return;
        body = ui_dialog_body(dialog);
        ui_label(body, "Dataset title", UI_SECONDARY);
        demo_input = ui_input(body, "Title");
        lv_obj_set_width(demo_input, lv_pct(100));
        lv_textarea_set_max_length(demo_input, 48);
        lv_textarea_set_text(demo_input, demo_name);
        {
            static const char *const choices[] = {"Standard columns", "Compact columns"};
            demo_compact_input = ui_radio_list(body, choices, 2, demo_compact ? 1 : 0);
        }

        demo_error = ui_label(body, "", UI_ERROR);
        ui_dialog_button(dialog, "Cancel", 0, 0);
        ui_dialog_button(dialog, "Apply", 1, 1);
        lv_group_focus_obj(demo_input);
        if (trace)
            fprintf(stderr, "GALLERY_DIALOG options\n");
    } else
        ui_message("Shared components", "This table recycles a bounded pool of visible cells. Its "
                                        "menus and dialogs own keyboard focus until dismissed.");
}
static void demo_menu_open(lv_event_t *event) {
    struct ui_menu_item items[] = {{"Copy cell", "Ctrl+C", "file", 1, 0},
                                   {"First cell", "Ctrl+Home", "arrow-up", 2, 0},
                                   {"Last cell", "Ctrl+End", "arrow-right", 3, 0},
                                   {"Table options...", "", "settings", 4, 0},
                                   {"About components", "", "info", 5, 0}};
    (void)event;
    if (ui_menu_open(demo_menu, items, 5, demo_command, NULL) && trace)
        fprintf(stderr, "GALLERY_MENU data\n");
}
static void data_page(void) {
    static const char *names[] = {"Record", "Name",  "Reading", "Region",
                                  "Site",   "Batch", "Label",   "Notes"};
    lv_obj_t *row = line(content);
    unsigned i;
    demo_menu = ui_icon_button(row, "Actions", "grid-2x2", UI_BUTTON);
    gx_lv_width(demo_menu, 112);
    lv_obj_add_event_cb(demo_menu, demo_menu_open, LV_EVENT_CLICKED, NULL);
    demo_title = ui_label(row, demo_name, UI_BODY);
    demo_table = ui_table_create(content, demo_value, NULL);
    lv_obj_set_height(ui_table_object(demo_table), 0);
    lv_obj_set_flex_grow(ui_table_object(demo_table), 1);
    ui_table_callbacks(demo_table, demo_selected, demo_sort);
    for (i = 0; i < 8; i++) {
        ui_table_heading(demo_table, i, names[i]);
        ui_table_column_width(demo_table, i, demo_compact ? 96 : 144);
    }
    demo_desc = 0;
    demo_sort_column = -1;
    ui_table_bind(demo_table, 100000UL, 8);
    lv_obj_update_layout(content);
    ui_table_density(demo_table);
    ui_table_focus(demo_table);
    if (trace)
        fprintf(stderr, "GALLERY_TABLE_READY widgets=%u\n", ui_table_widget_count(demo_table));
}
static void action_button(lv_event_t *e) { tell((const char *)lv_event_get_user_data(e)); }
static void checkbox_event(lv_event_t *e) {
    hidden = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    tell(lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED)
             ? "Hidden files are visible"
             : "Hidden files are hidden");
}
static void field_event(lv_event_t *e) {
    lv_obj_t *input = lv_event_get_target_obj(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_FOCUSED || code == LV_EVENT_DEFOCUSED || code == LV_EVENT_PRESSED) {
        replace = 0;
        lv_textarea_clear_selection(input);
        if (trace && code != LV_EVENT_PRESSED)
            fprintf(stderr, "GALLERY_FOCUS %s %s\n", input == field ? "project" : "validation",
                    code == LV_EVENT_FOCUSED ? "focused" : "blurred");
    }
    if (code == LV_EVENT_KEY) {
        uint32_t key = lv_event_get_key(e);
        if (key == LV_KEY_BACKSPACE || key == LV_KEY_DEL) {
            if (replace)
                lv_textarea_set_text(input, "");
            replace = 0;
        } else if (key == LV_KEY_LEFT || key == LV_KEY_RIGHT || key == LV_KEY_HOME ||
                   key == LV_KEY_END) {
            replace = 0;
            lv_textarea_clear_selection(input);
        } else if (key == LV_KEY_ENTER)
            tell(input == field ? "Project name updated" : "Folder path updated");
    }
}
static lv_obj_t *active_input(void) {
    lv_obj_t *input = lv_group_get_focused(gx_lv_group());
    return input && (input == field || input == invalid_field) ? input : NULL;
}
static void validate_folder(lv_event_t *event) {
    struct stat st;
    const char *path = lv_textarea_get_text(invalid_field);
    int valid = path[0] && !stat(path, &st) && S_ISDIR(st.st_mode);
    (void)event;
    ui_input_error(invalid_field, !valid);
    lv_label_set_text(validation_hint, valid ? "" : "Choose an existing folder.");
    if (trace)
        fprintf(stderr, "GALLERY_VALIDATION %s\n", valid ? "valid" : "invalid");
}
static lv_obj_t *line(lv_obj_t *parent) {
    lv_obj_t *obj = ui_stack(parent, LV_FLEX_FLOW_ROW, UI_SPACE);
    lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return obj;
}
static void controls(void) {
    lv_obj_t *row, *obj;
    ui_label(content, "Actions", UI_BODY);
    row = line(content);
    obj = ui_button(row, "Secondary", UI_BUTTON);
    lv_obj_add_event_cb(obj, action_button, LV_EVENT_CLICKED, "Secondary action selected");
    obj = ui_button(row, "Primary", UI_PRIMARY);
    lv_obj_add_event_cb(obj, action_button, LV_EVENT_CLICKED, "Primary action selected");
    obj = ui_button(row, "Unavailable", UI_BUTTON);
    lv_obj_add_event_cb(obj, action_button, LV_EVENT_CLICKED, "Unavailable action selected");
    lv_obj_add_state(obj, LV_STATE_DISABLED);
    ui_label(content, "Button states", UI_SECONDARY);
    row = line(content);
    obj = ui_button(row, "Hover", UI_BUTTON);
    lv_obj_add_state(obj, LV_STATE_HOVERED);
    lv_group_remove_obj(obj);
    obj = ui_button(row, "Pressed", UI_BUTTON);
    lv_obj_add_state(obj, LV_STATE_PRESSED);
    lv_group_remove_obj(obj);
    obj = ui_button(row, "Focus", UI_BUTTON);
    lv_obj_add_state(obj, LV_STATE_FOCUS_KEY);
    lv_group_remove_obj(obj);
    ui_label(content, "Project name", UI_SECONDARY);
    field = ui_input(content, "Give your project a name");
    lv_obj_set_width(field, lv_pct(100));
    lv_textarea_set_text(field, project_name);
    lv_textarea_set_max_length(field, 64);
    lv_obj_add_event_cb(field, field_event, LV_EVENT_ALL, NULL);
    ui_label(content, "Validation example", UI_SECONDARY);
    invalid_field = ui_input(content, NULL);
    lv_obj_set_width(invalid_field, lv_pct(100));
    lv_textarea_set_text(invalid_field, validation_path);
    lv_textarea_set_max_length(invalid_field, 64);
    validation_hint = ui_label(content, "Choose an existing folder.", UI_ERROR);
    gx_lv_height(validation_hint, 20);
    lv_obj_add_event_cb(invalid_field, field_event, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(invalid_field, validate_folder, LV_EVENT_VALUE_CHANGED, NULL);
    validate_folder(NULL);
    check = ui_checkbox(content, "Show hidden files");
    if (hidden)
        lv_obj_add_state(check, LV_STATE_CHECKED);
    lv_obj_add_event_cb(check, checkbox_event, LV_EVENT_VALUE_CHANGED, NULL);
    obj = ui_checkbox(content, "Sync settings");
    lv_obj_add_state(obj, LV_STATE_DISABLED);
}
static void typography(void) {
    static const char *names[] = {"Canvas", "Panel", "Border", "Text", "Secondary", "Accent"};
    unsigned i;
    ui_label(content, "A clear hierarchy", UI_HEADING);
    ui_label(content, "The quick brown fox jumps over the lazy dog.", UI_BODY);
    ui_label(content, "Secondary details stay quiet and readable.", UI_SECONDARY);
    ui_label(content, "Українська: Ґґ Єє Іі Її", UI_BODY);
    ui_label(content, "Українська: Ґґ Єє Іі Її", UI_SECONDARY);
    ui_label(content, "Greek: α β γ δ π  ·  Café, naïve, déjà vu", UI_BODY);
    ui_label(content, "Palette", UI_HEADING);
    for (i = 0; i < 6; i++) {
        lv_obj_t *row = line(content);
        ui_swatch(row, i);
        ui_label(row, names[i], UI_BODY);
    }
}
static void preview_click(lv_event_t *event) {
    (void)event;
    preview_folder = !preview_folder;
    ui_icon_set(icon_preview, preview_folder ? "folder" : "file");
    tell(preview_folder ? "Folder icon" : "File icon");
}
static void icons(void) {
    static const int32_t columns[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
                                      LV_GRID_TEMPLATE_LAST};
    static const int32_t rows[] = {LV_GRID_CONTENT, LV_GRID_CONTENT,      LV_GRID_CONTENT,
                                   LV_GRID_CONTENT, LV_GRID_CONTENT,      LV_GRID_CONTENT,
                                   LV_GRID_CONTENT, LV_GRID_TEMPLATE_LAST};
    lv_obj_t *row, *button, *grid;
    unsigned i;
    ui_label(content, "Actions", UI_BODY);
    row = line(content);
    button = ui_icon_button(row, "Preview", "file", UI_BUTTON);
    icon_buttons[0] = button;
    gx_lv_width(button, 112);
    icon_preview = lv_obj_get_child(button, 0);
    preview_folder = 0;
    lv_obj_add_event_cb(button, preview_click, LV_EVENT_CLICKED, NULL);
    button = ui_icon_button(row, "Open", "folder", UI_PRIMARY);
    icon_buttons[1] = button;
    gx_lv_width(button, 88);
    lv_obj_add_event_cb(button, action_button, LV_EVENT_CLICKED, "Open selected");
    button = ui_icon_button(row, "Delete", "trash", UI_BUTTON);
    icon_buttons[2] = button;
    gx_lv_width(button, 96);
    lv_obj_add_state(button, LV_STATE_DISABLED);
    lv_obj_add_event_cb(button, action_button, LV_EVENT_CLICKED, "Delete selected");
    ui_label(content, "System icons", UI_BODY);
    grid = ui_stack(content, LV_FLEX_FLOW_COLUMN, UI_SPACE);
    icon_grid = grid;
    lv_obj_set_grid_dsc_array(grid, columns, rows);
    for (i = 0; i < ui_icon_count(); i++) {
        const char *name = ui_icon_name(i);
        row = line(grid);
        lv_obj_set_grid_cell(row, LV_GRID_ALIGN_STRETCH, i % 3, 1, LV_GRID_ALIGN_CENTER, i / 3, 1);
        gx_lv_height(row, UI_ROW_HEIGHT);
        ui_icon(row, name);
        ui_label(row, name, UI_SECONDARY);
    }
}
static void select_item(unsigned index) {
    char text[80];
    lv_obj_remove_state(items[selected], LV_STATE_CHECKED);
    selected = index < 48 ? index : 47;
    lv_obj_add_state(items[selected], LV_STATE_CHECKED);
    lv_obj_scroll_to_view(items[selected], LV_ANIM_OFF);
    snprintf(text, sizeof(text), "Selected: document-%02u.txt", selected + 1);
    tell(text);
}
static void item_click(lv_event_t *e) {
    select_item((unsigned)(uintptr_t)lv_event_get_user_data(e));
    lv_group_focus_obj(list);
}
static void list_key(lv_event_t *e) {
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_UP && selected)
        select_item(selected - 1);
    else if (key == LV_KEY_DOWN && selected < 47)
        select_item(selected + 1);
    else if (key == LV_KEY_HOME)
        select_item(0);
    else if (key == LV_KEY_END)
        select_item(47);
    else if (key == LV_KEY_ENTER)
        tell("Document selected");
}
static void lists(void) {
    unsigned i;
    ui_label(content, "Documents", UI_BODY);
    list = ui_list(content);
    lv_obj_set_width(list, lv_pct(100));
    lv_obj_set_height(list, 0);
    lv_obj_set_flex_grow(list, 1);
    lv_group_add_obj(gx_lv_group(), list);
    lv_obj_add_event_cb(list, list_key, LV_EVENT_KEY, NULL);
    for (i = 0; i < 48; i++) {
        char name[48], detail[24];
        snprintf(name, sizeof(name), "document-%02u.txt", i + 1);
        snprintf(detail, sizeof(detail), "%u KiB", i + 1);
        items[i] = ui_button(list, NULL, UI_ROW);
        lv_obj_set_width(items[i], lv_pct(100));
        lv_group_remove_obj(items[i]);
        ui_row_labels(items[i], name, detail);
        lv_obj_add_event_cb(items[i], item_click, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    }
    selected = 0;
    lv_obj_add_state(items[0], LV_STATE_CHECKED);
}
static void workspace_command(unsigned id, void *unused) {
    char text[100];
    (void)unused;
    if (id == 1) {
        workspace_runs++;
        snprintf(text, sizeof(text), "Result %u: 3 * x^2 + 2", workspace_runs);
        lv_label_set_text(workspace_result, text);
        snprintf(text, sizeof(text), "%u evaluations", workspace_runs);
        lv_label_set_text(workspace_counter, text);
        tell("Cell evaluated");
    } else if (id == 2) {
        workspace_runs = 0;
        lv_label_set_text(workspace_result, "Run the example to see its result.");
        lv_label_set_text(workspace_counter, "No evaluations");
        ui_split_position(workspace_split, 400);
        ui_split_position(workspace_preview, 600);
        tell("Workspace reset");
    } else if (id == 3) {
        ui_split_show_second(workspace_split, !ui_split_second_visible(workspace_split));
        tell(ui_split_second_visible(workspace_split) ? "Preview shown" : "Preview hidden");
    } else if (id == 4)
        ui_message("Workspace", "Commands share their state across buttons, menus and shortcuts.");
    if (trace)
        fprintf(stderr, "GALLERY_ACTION %u runs=%u\n", id, workspace_runs);
}
static void workspace_enable(lv_event_t *event) {
    int enabled = lv_obj_has_state(lv_event_get_target_obj(event), LV_STATE_CHECKED);
    ui_actions_enable(workspace_actions, 1, enabled);
    if (trace)
        fprintf(stderr, "GALLERY_ACTION_ENABLED %d\n", enabled);
}
static void workspace_items(lv_event_t *event) {
    static const unsigned ids[] = {1, 2, 3, 4};
    if (lv_event_get_code(event) == LV_EVENT_KEY && lv_event_get_key(event) != LV_KEY_DOWN)
        return;
    if (ui_actions_menu(workspace_actions, workspace_menu, ids, 4) && trace)
        fprintf(stderr, "GALLERY_MENU workspace\n");
}
static void workspace_changed(lv_event_t *event) {
    (void)event;
    if (trace)
        fprintf(stderr, "GALLERY_SPLIT %u %u visible=%d\n",
                ui_split_get_position(workspace_split), ui_split_get_position(workspace_preview),
                ui_split_second_visible(workspace_split));
}
static lv_obj_t *workspace_panel(lv_obj_t *pane) {
    lv_obj_t *panel = ui_panel(pane);
    lv_obj_set_size(panel, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);
    return panel;
}
static void workspace_page(void) {
    static const struct ui_action_spec specs[] = {
        {1, "Run", "Ctrl+R", "play", 21, 1, 1},
        {2, "Reset", "Ctrl+Backspace", "refresh-cw", 42, 1, 1},
        {3, "Preview", "Ctrl+D", "file", 7, 1, 1},
        {4, "About workspace", "Ctrl+I", "info", 12, 1, 1}
    };
    lv_obj_t *row, *panel, *label;
    workspace_runs = 0;
    workspace_actions = ui_actions_create(specs, 4, workspace_command, NULL);
    if (!workspace_actions) {
        ui_label(content, "Could not create workspace commands.", UI_ERROR);
        return;
    }
    row = line(content);
    ui_actions_button(workspace_actions, row, 1, UI_PRIMARY);
    ui_actions_button(workspace_actions, row, 2, UI_BUTTON);
    workspace_menu = ui_icon_button(row, "Actions", "settings", UI_BUTTON);
    lv_obj_add_event_cb(workspace_menu, workspace_items, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(workspace_menu, workspace_items, LV_EVENT_KEY, NULL);
    workspace_allow = ui_checkbox(content, "Allow run");
    lv_obj_add_state(workspace_allow, LV_STATE_CHECKED);
    lv_obj_add_event_cb(workspace_allow, workspace_enable, LV_EVENT_VALUE_CHANGED, NULL);
    ui_label(content, "Drag a divider, or focus it and use the arrow keys.", UI_SECONDARY);
    workspace_split = ui_split_create(content, UI_SPLIT_HORIZONTAL);
    lv_obj_set_height(ui_split_object(workspace_split), 0);
    lv_obj_set_flex_grow(ui_split_object(workspace_split), 1);
    ui_split_minimum(workspace_split, 140, 180);
    ui_split_position(workspace_split, 400);
    panel = workspace_panel(ui_split_pane(workspace_split, 0));
    ui_label(panel, "Document", UI_HEADING);
    label = ui_label(panel, "Differentiate\nx^3 + 2*x", UI_BODY);
    lv_obj_set_width(label, lv_pct(100));
    ui_actions_button(workspace_actions, panel, 1, UI_BUTTON);
    workspace_preview = ui_split_create(ui_split_pane(workspace_split, 1), UI_SPLIT_VERTICAL);
    lv_obj_set_size(ui_split_object(workspace_preview), lv_pct(100), lv_pct(100));
    ui_split_minimum(workspace_preview, 72, 56);
    ui_split_position(workspace_preview, 600);
    panel = workspace_panel(ui_split_pane(workspace_preview, 0));
    ui_label(panel, "Result preview", UI_HEADING);
    workspace_result = ui_label(panel, "Run the example to see its result.", UI_BODY);
    lv_obj_set_width(workspace_result, lv_pct(100));
    panel = workspace_panel(ui_split_pane(workspace_preview, 1));
    ui_label(panel, "Session", UI_HEADING);
    workspace_counter = ui_label(panel, "No evaluations", UI_SECONDARY);
    lv_obj_set_width(workspace_counter, lv_pct(100));
    lv_obj_add_event_cb(ui_split_object(workspace_split), workspace_changed,
                         LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(ui_split_object(workspace_preview), workspace_changed,
                         LV_EVENT_VALUE_CHANGED, NULL);
}
static void plot_selected(void *unused, uint32_t row) {
    char text[96];
    (void)unused;
    if (row == PLOT_NO_ROW)
        tell("Selection cleared");
    else {
        snprintf(text, sizeof(text), "Sample %lu selected", (unsigned long)row + 1);
        tell(text);
    }
    if (trace)
        fprintf(stderr, "GALLERY_PLOT_SELECT %lu\n", (unsigned long)row);
}
static void plot_mode_changed(lv_event_t *event) {
    unsigned mode = ui_choice_selected(plot_mode_choice);
    (void)event;
    if (!demo_plot)
        return;
    ui_plot_mode(demo_plot, mode ? PLOT_SCATTER : PLOT_LINE);
    if (trace)
        fprintf(stderr, "GALLERY_PLOT_MODE %u\n", mode);
}
static void plot_state_changed(lv_event_t *event) {
    unsigned state = ui_choice_selected(plot_state_choice);
    (void)event;
    if (!demo_plot)
        return;
    ui_plot_bind(demo_plot, state ? NULL : demo_plot_model);
    ui_plot_message(demo_plot, state == 1 ? "No samples to plot."
                                        : state == 2 ? "Choose columns containing numeric samples." : "");
    tell(state == 1 ? "Empty dataset" : state == 2 ? "No numeric samples" : "Ready");
    if (trace)
        fprintf(stderr, "GALLERY_PLOT_STATE %u\n", state);
}
static void plot_command(lv_event_t *event) {
    unsigned command = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    if (!demo_plot)
        return;
    if (command == 0)
        ui_plot_fit(demo_plot);
    else
        ui_plot_zoom(demo_plot, command == 1 ? 0.75 : 1.3333333333333333);
    if (trace)
        fprintf(stderr, "GALLERY_PLOT_COMMAND %u\n", command);
}
static void note(lv_obj_t *parent, const char *text) {
    lv_obj_t *label = ui_label(parent, text, UI_SECONDARY);
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
}
static void plots_page(void) {
    static const char *const modes[] = {"Line", "Scatter"};
    static const char *const states[] = {"Sample data", "Empty dataset", "Non-numeric data"};
    static const char *const commands[] = {"Fit", "Zoom in", "Zoom out"};
    lv_obj_t *row = line(content), *button;
    unsigned i;
    int ready;
    plot_mode_choice = ui_choice_create(row, modes, 2, 0);
    if (!plot_mode_choice)
        return;
    gx_lv_width(ui_choice_object(plot_mode_choice), 116);
    lv_obj_add_event_cb(ui_choice_object(plot_mode_choice), plot_mode_changed,
                         LV_EVENT_VALUE_CHANGED, NULL);
    for (i = 0; i < 3; i++) {
        button = ui_button(row, commands[i], UI_BUTTON);
        plot_buttons[i] = button;
        lv_obj_add_event_cb(button, plot_command, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    }
    row = line(content);
    plot_state_choice = ui_choice_create(row, states, 3, 0);
    if (!plot_state_choice)
        return;
    gx_lv_width(ui_choice_object(plot_state_choice), 196);
    ui_label(row, "1,000 samples · one gap · one peak", UI_SECONDARY);
    demo_plot = ui_plot_create(content);
    if (!demo_plot) {
        ui_label(content, "Could not create the plot.", UI_ERROR);
        return;
    }
    lv_obj_set_height(ui_plot_object(demo_plot), 0);
    lv_obj_set_flex_grow(ui_plot_object(demo_plot), 1);
    ui_plot_labels(demo_plot, "Time", "Reading");
    ui_plot_callback(demo_plot, plot_selected, NULL);
    demo_plot_model = plot_model_create(1000);
    if (!demo_plot_model) {
        ui_plot_message(demo_plot, "Not enough memory for sample data.");
        return;
    }
    for (i = 0; i < 1000; i++) {
        double y = sin((double)i / 40.0) * 3.0 + cos((double)i / 11.0) * 0.5 + (double)i / 500.0;
        if (i == 700)
            y = 18.0;
        if (plot_model_append(demo_plot_model, (double)i / 10.0, y, i,
                                i < 480 || i >= 520) < 0)
            break;
    }
    do {
        ready = plot_model_prepare_step(demo_plot_model, 256);
    } while (ready == 0);
    if (ready < 0) {
        ui_plot_message(demo_plot, "Could not prepare sample data.");
        return;
    }
    ui_plot_bind(demo_plot, demo_plot_model);
    lv_obj_add_event_cb(ui_choice_object(plot_state_choice), plot_state_changed,
                         LV_EVENT_VALUE_CHANGED, NULL);
    note(content, "Drag to pan. Use Zoom in/out or +/- keys. Click a sample to inspect it.");
    if (trace)
        fprintf(stderr, "GALLERY_PLOT_READY count=%lu valid=%lu peak=700\n",
                (unsigned long)plot_model_count(demo_plot_model),
                (unsigned long)plot_model_valid(demo_plot_model));
}
static int forms_path_result(ui_path *dialog, const char *path, void *user) {
    unsigned mode = (unsigned)(uintptr_t)user;
    (void)dialog;
    forms_path = NULL;
    if (path) {
        /* This gallery demonstrates intent/validation only; it never writes
         * or replaces the file selected by the Save dialog. */
        tell(path);
        if (trace)
            fprintf(stderr, "GALLERY_PATH_ACCEPT %s %s\n", mode ? "save" : "open", path);
    } else if (trace)
        fprintf(stderr, "GALLERY_PATH_CANCEL %s\n", mode ? "save" : "open");
    return 1;
}
static void forms_open_path(lv_event_t *event) {
    unsigned mode = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    const char *home = getenv("HOME");
    char initial[UI_PATH_LIMIT];
    if (forms_path || ui_overlay_active())
        return;
    initial[0] = 0;
    if (mode) {
        if (home && home[0] == '/' && strlen(home) + 12 < sizeof(initial))
            snprintf(initial, sizeof(initial), "%s/sample.txt", home);
        else
            strcpy(initial, "sample.txt");
    }
    forms_path = ui_path_open(mode ? "Save file" : "Open file", mode ? UI_PATH_SAVE : UI_PATH_OPEN,
                               initial, forms_path_result, (void *)(uintptr_t)mode);
    if (forms_path && trace)
        fprintf(stderr, "GALLERY_PATH_OPEN %s\n", mode ? "save" : "open");
}
static void forms_choice_changed(lv_event_t *event) {
    char text[80];
    unsigned value = ui_choice_selected(forms_choice);
    (void)event;
    snprintf(text, sizeof(text), "Column %u selected", value + 1);
    tell(text);
    if (trace)
        fprintf(stderr, "GALLERY_CHOICE %u\n", value);
}
static void forms_job_stop(void) {
    if (forms_timer) {
        lv_timer_delete(forms_timer);
        forms_timer = NULL;
    }
    if (forms_start)
        lv_obj_remove_state(forms_start, LV_STATE_DISABLED);
    if (forms_progress)
        ui_progress_running(forms_progress, 0);
}
static void forms_job_cancel(ui_progress *progress, void *unused) {
    char text[96];
    (void)progress;
    (void)unused;
    forms_job_stop();
    snprintf(text, sizeof(text), "Cancelled after %lu samples", (unsigned long)forms_done);
    ui_progress_update(forms_progress, text, forms_done, FORMS_SAMPLES);
    tell("Sample processing cancelled");
    if (trace)
        fprintf(stderr, "GALLERY_JOB_CANCEL %lu\n", (unsigned long)forms_done);
}
static void forms_job_step(lv_timer_t *timer) {
    unsigned i;
    char text[100];
    (void)timer;
    /* One bounded batch of real work, followed by a return to input/rendering.
     * Unsigned arithmetic deliberately specifies a reproducible rolling hash. */
    for (i = 0; i < 4096 && forms_done < FORMS_SAMPLES; i++, forms_done++)
        forms_checksum = (forms_checksum ^ (forms_done * 2654435761UL)) * 16777619UL;
    snprintf(text, sizeof(text), "%lu of %lu samples", (unsigned long)forms_done,
             (unsigned long)FORMS_SAMPLES);
    ui_progress_update(forms_progress, text, forms_done, FORMS_SAMPLES);
    if (forms_done == FORMS_SAMPLES) {
        forms_job_stop();
        snprintf(text, sizeof(text), "Complete · checksum %lu", (unsigned long)forms_checksum);
        ui_progress_update(forms_progress, text, forms_done, FORMS_SAMPLES);
        tell("Sample processing complete");
        if (trace)
            fprintf(stderr, "GALLERY_JOB_DONE %lu checksum=%lu\n", (unsigned long)forms_done,
                    (unsigned long)forms_checksum);
    }
}
static void forms_job_start(lv_event_t *event) {
    (void)event;
    if (forms_timer || lv_obj_has_state(forms_start, LV_STATE_DISABLED))
        return;
    forms_done = 0;
    forms_checksum = 2166136261UL;
    ui_progress_running(forms_progress, 1);
    ui_progress_update(forms_progress, "Processing samples", 0, FORMS_SAMPLES);
    lv_obj_add_state(forms_start, LV_STATE_DISABLED);
    forms_timer = lv_timer_create(forms_job_step, 1, NULL);
    if (!forms_timer) {
        forms_job_stop();
        tell("Could not start sample processing");
        return;
    }
    ui_progress_focus_cancel(forms_progress);
    if (trace)
        fprintf(stderr, "GALLERY_JOB_START %lu\n", (unsigned long)FORMS_SAMPLES);
}
static void forms_page(void) {
    lv_obj_t *row = line(content), *button;
    char **labels = calloc(UI_CHOICE_LIMIT, sizeof(*labels));
    unsigned i;
    button = ui_icon_button(row, "Open file...", "folder", UI_BUTTON);
    forms_open_buttons[0] = button;
    lv_obj_add_event_cb(button, forms_open_path, LV_EVENT_CLICKED, NULL);
    button = ui_icon_button(row, "Save file...", "save", UI_BUTTON);
    forms_open_buttons[1] = button;
    lv_obj_add_event_cb(button, forms_open_path, LV_EVENT_CLICKED, (void *)(uintptr_t)1);
    note(content, "File choices are displayed here; this example does not write files.");
    ui_label(content, "Column", UI_BODY);
    if (!labels)
        return;
    for (i = 0; i < UI_CHOICE_LIMIT; i++) {
        labels[i] = malloc(32);
        if (!labels[i])
            break;
        snprintf(labels[i], 32, "Column %u", i + 1);
    }
    if (i == UI_CHOICE_LIMIT)
        forms_choice = ui_choice_create(content, (const char *const *)labels, UI_CHOICE_LIMIT, 0);
    for (i = 0; i < UI_CHOICE_LIMIT; i++)
        free(labels[i]);
    free(labels);
    if (!forms_choice) {
        ui_label(content, "Could not create the column selector.", UI_ERROR);
        return;
    }
    gx_lv_width(ui_choice_object(forms_choice), 240);
    lv_obj_add_event_cb(ui_choice_object(forms_choice), forms_choice_changed,
                         LV_EVENT_VALUE_CHANGED, NULL);
    note(content, "Use arrows or Home/End, or open the list to choose among 65 columns.");
    ui_label(content, "Sample processing", UI_HEADING);
    note(content, "Calculate a checksum over 8 million generated samples.");
    forms_start = ui_icon_button(content, "Start", "play", UI_PRIMARY);
    lv_obj_add_event_cb(forms_start, forms_job_start, LV_EVENT_CLICKED, NULL);
    forms_progress = ui_progress_create(content, forms_job_cancel, NULL);
    if (!forms_progress) {
        lv_obj_add_state(forms_start, LV_STATE_DISABLED);
        return;
    }
    ui_progress_running(forms_progress, 0);
    ui_progress_update(forms_progress, "Ready", 0, 0);
    if (trace)
        fprintf(stderr, "GALLERY_FORMS_READY choices=65 samples=%lu\n", (unsigned long)FORMS_SAMPLES);
}
static void editor_tabs_refresh(void) {
    struct ui_tab_spec specs[UI_DOCUMENT_TABS];
    ui_document *active = ui_documents_active(editor_documents);
    unsigned i, count = ui_documents_count(editor_documents), selected = 1;
    for (i = 0; i < count; i++) {
        ui_document *document = ui_documents_at(editor_documents, i);
        specs[i].id = i + 1;
        specs[i].text = ui_document_name(document);
        specs[i].dirty = text_modified(ui_document_text(document));
        specs[i].closable = 1;
        if (document == active)
            selected = i + 1;
    }
    ui_tabs_bind(editor_tabs, specs, count, selected);
}
static void editor_remember(void) {
    unsigned long bounds[2], cursor;
    ui_document *document = ui_documents_active(editor_documents);
    if (!document || !demo_editor)
        return;
    cursor = ui_editor_cursor(demo_editor);
    ui_editor_selection(demo_editor, bounds);
    ui_document_view(document, cursor, bounds[0] == cursor ? bounds[1] : bounds[0]);
}
static void editor_activate(ui_document *document) {
    unsigned long cursor, anchor;
    ui_documents_select(editor_documents, document);
    ui_editor_buffer(demo_editor, ui_document_text(document));
    ui_document_get_view(document, &cursor, &anchor);
    ui_editor_select(demo_editor, anchor, cursor);
    editor_tabs_refresh();
    ui_editor_focus(demo_editor);
}
static void editor_tab_changed(unsigned id, int close, void *unused) {
    ui_document *document = ui_documents_at(editor_documents, id - 1);
    ui_document *replacement;
    (void)unused;
    if (!document)
        return;
    editor_remember();
    if (!close)
        editor_activate(document);
    else {
        if (ui_documents_count(editor_documents) == 1)
            replacement = ui_documents_new(editor_documents);
        else
            replacement = ui_documents_at(editor_documents, id == 1 ? 1 : 0);
        if (!replacement)
            return;
        if (document == ui_documents_active(editor_documents) ||
            ui_documents_count(editor_documents) == 2)
            editor_activate(replacement);
        ui_documents_close(editor_documents, document, 1);
        editor_tabs_refresh();
    }
    if (trace)
        fprintf(stderr, "GALLERY_EDITOR_TAB %u close=%d count=%u\n", id, close,
                ui_documents_count(editor_documents));
}
static void editor_changed(lv_event_t *event) {
    char message[128];
    TextBuffer *buffer;
    unsigned long cursor;
    (void)event;
    if (!demo_editor || !editor_documents)
        return;
    buffer = ui_document_text(ui_documents_active(editor_documents));
    cursor = ui_editor_cursor(demo_editor);
    editor_tabs_refresh();
    snprintf(message, sizeof(message), "Line %lu  |  Column %lu  |  %lu bytes%s",
             text_row(buffer, cursor) + 1, text_visual(buffer, cursor, 4) + 1,
             text_length(buffer), text_modified(buffer) ? "  |  Unsaved" : "");
    tell(message);
    if (trace)
        fprintf(stderr, "GALLERY_EDITOR bytes=%lu cursor=%lu dirty=%d\n",
                text_length(buffer), cursor, text_modified(buffer));
}
static void editor_demo_action(lv_event_t *event) {
    unsigned command = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    TextBuffer *buffer = ui_document_text(ui_documents_active(editor_documents));
    long cursor;
    if (command == 3) {
        text_saved(buffer);
        ui_editor_changed(demo_editor);
    } else {
        cursor = command == 1 ? text_undo(buffer) : text_redo(buffer);
        if (cursor >= 0) {
            ui_editor_changed(demo_editor);
            ui_editor_select(demo_editor, cursor, cursor);
        }
    }
    ui_editor_focus(demo_editor);
    editor_changed(NULL);
}
static void editor_wrap_changed(lv_event_t *event) {
    (void)event;
    ui_editor_wrap(demo_editor, lv_obj_has_state(editor_wrap, LV_STATE_CHECKED));
    ui_editor_focus(demo_editor);
}
static void editor_page(void) {
    static const char source[] =
        "/* Graphite text editor: a shared Rift buffer. */\n"
        "int main(void) {\n"
        "    const char *greeting = \"Привіт, світе!\";\n"
        "    return greeting[0] != 0;\n"
        "}\n";
    static const char notes[] =
        "A document keeps its own text and undo history.\n\n"
        "Click to place the caret. Drag to select. Paste a paragraph, then undo it. "
        "Visual wrapping follows the viewport without adding line endings to your text.\n\n"
        "Combining marks: é. Ukrainian: Україна.\n";
    ui_document *first, *second;
    lv_obj_t *row, *button;
    editor_documents = ui_documents_create(NULL);
    if (!editor_documents)
        return;
    first = ui_documents_new(editor_documents);
    second = ui_documents_new(editor_documents);
    if (!first || !second)
        return;
    text_append(ui_document_text(first), (const unsigned char *)source, strlen(source));
    text_language(ui_document_text(first), 1);
    text_saved(ui_document_text(first));
    text_append(ui_document_text(second), (const unsigned char *)notes, strlen(notes));
    text_saved(ui_document_text(second));
    ui_documents_select(editor_documents, first);
    row = line(content);
    button = ui_icon_button(row, "Undo", "arrow-left", UI_BUTTON);
    lv_obj_add_event_cb(button, editor_demo_action, LV_EVENT_CLICKED, (void *)(uintptr_t)1);
    button = ui_icon_button(row, "Redo", "arrow-right", UI_BUTTON);
    lv_obj_add_event_cb(button, editor_demo_action, LV_EVENT_CLICKED, (void *)(uintptr_t)2);
    button = ui_button(row, "Mark saved", UI_BUTTON);
    lv_obj_add_event_cb(button, editor_demo_action, LV_EVENT_CLICKED, (void *)(uintptr_t)3);
    editor_wrap = ui_checkbox(row, "Wrap");
    lv_obj_add_state(editor_wrap, LV_STATE_CHECKED);
    lv_obj_add_event_cb(editor_wrap, editor_wrap_changed, LV_EVENT_VALUE_CHANGED, NULL);
    editor_tabs = ui_tabs_create(content, editor_tab_changed, NULL);
    demo_editor = ui_editor_create(content, ui_document_text(first));
    if (!editor_tabs || !demo_editor)
        return;
    gx_lv_height(ui_editor_object(demo_editor), 260);
    lv_obj_set_width(ui_editor_object(demo_editor), lv_pct(100));
    lv_obj_add_event_cb(ui_editor_object(demo_editor), editor_changed, LV_EVENT_VALUE_CHANGED, NULL);
    editor_tabs_refresh();
    note(content, "Each tab has its own document. Mark saved only changes this demo's status.");
    if (trace)
        fprintf(stderr, "GALLERY_EDITOR_READY tabs=2\n");
}
static void release_page(void) {
    forms_job_stop();
    if (forms_path)
        ui_path_cancel(forms_path);
    forms_path = NULL;
    ui_overlay_close();
    if (demo_plot)
        ui_plot_bind(demo_plot, NULL);
    demo_plot = NULL;
    ui_actions_delete(workspace_actions);
    workspace_actions = NULL;
    workspace_split = workspace_preview = NULL;
    lv_obj_clean(content);
    demo_editor = NULL;
    editor_tabs = NULL;
    editor_wrap = NULL;
    ui_documents_delete(editor_documents);
    editor_documents = NULL;
    plot_model_delete(demo_plot_model);
    demo_plot_model = NULL;
    plot_mode_choice = plot_state_choice = forms_choice = NULL;
    forms_progress = NULL;
    forms_start = NULL;
    plot_buttons[0] = plot_buttons[1] = plot_buttons[2] = NULL;
    forms_open_buttons[0] = forms_open_buttons[1] = NULL;
}
static void trace_rect(const char *name, lv_obj_t *obj) {
    lv_area_t area;
    unsigned density = gx_lv_density();
    if (!trace || !obj)
        return;
    lv_obj_get_coords(obj, &area);
    fprintf(stderr, "GALLERY_RECT %s %ld %ld %ld %ld\n", name,
            (long)(area.x1 / (int32_t)density), (long)(area.y1 / (int32_t)density),
            (long)(area.x2 / (int32_t)density), (long)(area.y2 / (int32_t)density));
}
static void trace_new_controls(void) {
    if (!trace)
        return;
    lv_obj_update_layout(content);
    if (demo_plot) {
        trace_rect("plot", ui_plot_object(demo_plot));
        trace_rect("plot-mode", ui_choice_object(plot_mode_choice));
        trace_rect("plot-state", ui_choice_object(plot_state_choice));
        trace_rect("plot-fit", plot_buttons[0]);
        trace_rect("plot-zoom-in", plot_buttons[1]);
        trace_rect("plot-zoom-out", plot_buttons[2]);
    }
    if (forms_choice) {
        trace_rect("forms-choice", ui_choice_object(forms_choice));
        trace_rect("forms-open", forms_open_buttons[0]);
        trace_rect("forms-save", forms_open_buttons[1]);
        trace_rect("forms-start", forms_start);
        trace_rect("forms-progress", ui_progress_object(forms_progress));
    }
    if (demo_editor) {
        trace_rect("editor", ui_editor_object(demo_editor));
        trace_rect("editor-tabs", ui_tabs_object(editor_tabs));
        trace_rect("editor-wrap", editor_wrap);
    }
}
static void show(unsigned next) {
    unsigned i;
    if (next >= sizeof(pages) / sizeof(pages[0]))
        return;
    if (field)
        strcpy(project_name, lv_textarea_get_text(field));
    if (invalid_field)
        strcpy(validation_path, lv_textarea_get_text(invalid_field));
    field = invalid_field = validation_hint = check = list = icon_preview = NULL;
    demo_table = NULL;
    icon_grid = NULL;
    icon_buttons[0] = icon_buttons[1] = icon_buttons[2] = NULL;
    replace = 0;
    release_page();
    page = next;
    for (i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
        if (i == page)
            lv_obj_add_state(navigation[i], LV_STATE_CHECKED);
        else
            lv_obj_remove_state(navigation[i], LV_STATE_CHECKED);
    }
    lv_label_set_text(heading, pages[page]);
    lv_label_set_text(subtitle, descriptions[page]);
    lv_obj_scroll_to(content, 0, 0, LV_ANIM_OFF);
    if (page == 0)
        controls();
    else if (page == 1)
        typography();
    else if (page == 2)
        lists();
    else if (page == 3)
        icons();
    else if (page == 4)
        data_page();
    else if (page == 5)
        workspace_page();
    else if (page == 6)
        plots_page();
    else if (page == 7)
        forms_page();
    else
        editor_page();
    tell("Ready");
    trace_new_controls();
    if (trace)
        fprintf(stderr, "GALLERY_PAGE %u\n", page);
}
static void navigate(lv_event_t *e) { show((unsigned)(uintptr_t)lv_event_get_user_data(e)); }
static void resize(unsigned w, unsigned h) {
    unsigned i;
    (void)w;
    (void)h;
    gx_lv_width(sidebar, 140);
    if (field)
        ui_input_height(field, UI_CONTROL);
    if (invalid_field)
        ui_input_height(invalid_field, UI_CONTROL);
    if (validation_hint)
        gx_lv_height(validation_hint, 20);
    if (demo_table) {
        gx_lv_width(demo_menu, 112);
        ui_table_density(demo_table);
    }
    if (workspace_split) {
        ui_split_density(workspace_split);
        ui_split_density(workspace_preview);
    }
    if (demo_plot) {
        ui_plot_density(demo_plot);
        ui_choice_density(plot_mode_choice);
        ui_choice_density(plot_state_choice);
        gx_lv_width(ui_choice_object(plot_mode_choice), 116);
        gx_lv_width(ui_choice_object(plot_state_choice), 196);
    }
    if (forms_choice) {
        ui_choice_density(forms_choice);
        gx_lv_width(ui_choice_object(forms_choice), 240);
    }
    if (forms_progress)
        ui_progress_density(forms_progress);
    if (forms_path)
        ui_path_density(forms_path);
    if (demo_editor) {
        ui_editor_density(demo_editor);
        ui_tabs_density(editor_tabs);
        gx_lv_height(ui_editor_object(demo_editor), 260);
    }
    if (icon_grid) {
        gx_lv_width(icon_buttons[0], 112);
        gx_lv_width(icon_buttons[1], 88);
        gx_lv_width(icon_buttons[2], 96);
        for (i = 0; i < lv_obj_get_child_count(icon_grid); i++)
            gx_lv_height(lv_obj_get_child(icon_grid, i), UI_ROW_HEIGHT);
    }
    trace_new_controls();
}
static void text(uint32_t scalar) {
    char utf8[5];
    unsigned n;
    lv_obj_t *input = active_input();
    if (demo_editor && ui_editor_focused(demo_editor)) {
        ui_editor_codepoint(demo_editor, scalar);
        return;
    }
    if (!input || scalar < 32 || scalar == 127)
        return;
    if (scalar < 128) {
        utf8[0] = scalar;
        n = 1;
    } else if (scalar < 2048) {
        utf8[0] = 0xc0 | (scalar >> 6);
        utf8[1] = 0x80 | (scalar & 63);
        n = 2;
    } else if (scalar < 65536 && !(scalar >= 0xd800 && scalar <= 0xdfff)) {
        utf8[0] = 0xe0 | (scalar >> 12);
        utf8[1] = 0x80 | ((scalar >> 6) & 63);
        utf8[2] = 0x80 | (scalar & 63);
        n = 3;
    } else if (scalar >= 65536UL && scalar <= 0x10ffffUL) {
        utf8[0] = 0xf0 | (scalar >> 18);
        utf8[1] = 0x80 | ((scalar >> 12) & 63);
        utf8[2] = 0x80 | ((scalar >> 6) & 63);
        utf8[3] = 0x80 | (scalar & 63);
        n = 4;
    } else
        return;
    utf8[n] = 0;
    if (replace)
        lv_textarea_set_text(input, "");
    replace = 0;
    lv_textarea_add_text(input, utf8);
    if (trace)
        fprintf(stderr, "%s %s\n", input == field ? "GALLERY_TEXT" : "GALLERY_VALIDATION_TEXT",
                lv_textarea_get_text(input));
}
static void edit(unsigned code) {
    char buffer[260];
    lv_obj_t *input = active_input();
    if (demo_editor && ui_editor_focused(demo_editor)) {
        ui_editor_action(demo_editor, code);
        return;
    }
    if (!input && demo_table && ui_table_focused(demo_table) && code == GX_ACTION_COPY) {
        demo_command(1, NULL);
        return;
    }
    if (!input)
        return;
    if (code == GX_ACTION_SELECT_ALL) {
        const unsigned char *p = (const unsigned char *)lv_textarea_get_text(input);
        unsigned length = 0;
        for (; *p; p++)
            if ((*p & 0xc0) != 0x80)
                length++;
        lv_label_set_text_selection_start(lv_textarea_get_label(input), 0);
        lv_label_set_text_selection_end(lv_textarea_get_label(input), length);
        replace = 1;
        return;
    }
    if (code == GX_ACTION_PASTE) {
        if (!gx_lv_clipboard(buffer, sizeof(buffer), 0) && !strpbrk(buffer, "\r\n\t")) {
            if (replace)
                lv_textarea_set_text(input, "");
            replace = 0;
            lv_textarea_add_text(input, buffer);
        }
    } else if (code == GX_ACTION_COPY || code == GX_ACTION_CUT) {
        strcpy(buffer, lv_textarea_get_text(input));
        if (!gx_lv_clipboard(buffer, sizeof(buffer), 1) && code == GX_ACTION_CUT)
            lv_textarea_set_text(input, "");
    }
}
static int shortcut(unsigned key, int down, unsigned modifiers) {
    if (demo_editor && ui_editor_key(demo_editor, key, down, modifiers))
        return 1;
    if (ui_actions_key(workspace_actions, key, down, modifiers))
        return 1;
    if (demo_table && ui_table_focused(demo_table)) {
        if ((modifiers & 1) && (key == 74 || key == 77)) {
            if (down)
                ui_table_select(demo_table, key == 74 ? 0 : 99999UL, key == 74 ? 0 : 7, 1);
            return 1;
        }
        if (key == 75 || key == 78) {
            if (down)
                ui_table_page(demo_table, key == 75 ? -1 : 1);
            return 1;
        }
    }
    if (key == 41) {
        if (down)
            gx_lv_finish();
        return 1;
    }
    if (key >= 58 && key <= 66) {
        if (down)
            show(key - 58);
        return 1;
    }
    return 0;
}
static void ui(void) {
    unsigned i;
    lv_obj_t *screen = lv_screen_active();
    ui_screen();
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_ROW);
    sidebar = ui_panel(screen);
    lv_obj_set_height(sidebar, lv_pct(100));
    lv_obj_set_flex_flow(sidebar, LV_FLEX_FLOW_COLUMN);
    ui_label(sidebar, "Graphite", UI_HEADING);
    ui_label(sidebar, "Interface gallery", UI_SECONDARY);
    for (i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
        navigation[i] = ui_button(sidebar, pages[i], UI_ROW);
        lv_obj_set_width(navigation[i], lv_pct(100));
        lv_obj_add_event_cb(navigation[i], navigate, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    }
    main_column = ui_stack(screen, LV_FLEX_FLOW_COLUMN, UI_SPACE);
    lv_obj_set_width(main_column, 0);
    lv_obj_set_height(main_column, lv_pct(100));
    lv_obj_set_flex_grow(main_column, 1);
    heading = ui_label(main_column, "", UI_HEADING);
    subtitle = ui_label(main_column, "", UI_SECONDARY);
    content = ui_panel(main_column);
    lv_obj_set_width(content, lv_pct(100));
    lv_obj_set_height(content, 0);
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    status = ui_label(main_column, "Ready", UI_SECONDARY);
    lv_obj_set_width(status, lv_pct(100));
    lv_label_set_long_mode(status, LV_LABEL_LONG_MODE_DOTS);
    show(0);
    resize(760, 560);
}
int main(int argc, char **argv) {
    int code;
    trace = argc > 1 && !strcmp(argv[1], "--trace");
    if (argc > 1 && !trace) {
        puts("Usage: lvgallery [--trace]\nF1 Controls, F2 Typography, F3 Lists, F4 Icons, F5 Data, F6 Workspace, "
             "F7 Plots, F8 Forms, F9 Editor; "
             "Esc "
             "closes.");
        return !strcmp(argv[1], "--help") ? 0 : 1;
    }
    gx_lv_minimum(640, 460);
    if (gx_lv_open("Graphite Gallery", 760, 560) < 0)
        return 1;
    ui();
    gx_lv_handlers(text, edit, resize);
    gx_lv_shortcuts(shortcut);
    code = gx_lv_run();
    release_page();
    gx_lv_close();
    return code;
}
