/* SPDX-License-Identifier: MIT */
#include "ui.h"
#include "calc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gx.h>

static lv_obj_t *entry, *result, *title, *header, *keypad, *buttons[20];
static const int32_t columns[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
                                  LV_GRID_TEMPLATE_LAST};
static const int32_t lines[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
                                LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
static int evaluated, replace_all, trace;
static const char *keys[20] = {"C", "(", ")", "/", "7", "8", "9", "*", "4", "5",
                               "6", "-", "1", "2", "3", "+", "0", ".", "<", "="};

static void evaluate(void) {
    double value;
    char text[64];
    const char *error = calc_evaluate(lv_textarea_get_text(entry), &value);
    if (error) {
        lv_label_set_text(result, error);
        if (trace)
            printf("CALC_ERROR %s\n", error);
        return;
    }
    snprintf(text, sizeof(text), "%.12g", value);
    lv_textarea_set_text(entry, text);
    lv_textarea_set_cursor_pos(entry, LV_TEXTAREA_CURSOR_LAST);
    lv_label_set_text(result, "=");
    evaluated = 1;
    replace_all = 0;
    if (trace)
        printf("CALC_RESULT %s\n", text);
}

static void insert(const char *text) {
    if (replace_all ||
        (evaluated && (*text == '.' || *text == '(' || (*text >= '0' && *text <= '9'))))
        lv_textarea_set_text(entry, "");
    evaluated = replace_all = 0;
    lv_label_set_text(result, "");
    lv_group_focus_obj(entry);
    lv_textarea_add_text(entry, text);
    if (trace)
        printf("CALC_INPUT %s\n", lv_textarea_get_text(entry));
}
static void clear(void) {
    lv_textarea_set_text(entry, "");
    lv_label_set_text(result, "");
    evaluated = replace_all = 0;
}
static void press(lv_event_t *event) {
    unsigned i = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    if (i == 0)
        clear();
    else if (i == 18) {
        if (replace_all)
            clear();
        else
            lv_textarea_delete_char(entry);
        evaluated = 0;
        lv_label_set_text(result, "");
    } else if (i == 19)
        evaluate();
    else
        insert(keys[i]);
}
static void text(uint32_t scalar) {
    char s[2];
    if (scalar == '=' || scalar == '\n') {
        evaluate();
        return;
    }
    if (scalar == 'c' || scalar == 'C') {
        clear();
        return;
    }
    if (scalar > 127 || !strchr("0123456789.+-*/()eE ", (char)scalar) || !scalar)
        return;
    s[0] = scalar;
    s[1] = 0;
    insert(s);
}
static void entry_key(lv_event_t *event) {
    uint32_t key = lv_event_get_key(event);
    if (key == LV_KEY_ENTER)
        evaluate();
    else if (key == LV_KEY_ESC)
        clear();
    else if (key == LV_KEY_BACKSPACE || key == LV_KEY_DEL) {
        if (replace_all)
            clear();
        evaluated = 0;
        lv_label_set_text(result, "");
    } else if (key == LV_KEY_LEFT || key == LV_KEY_RIGHT || key == LV_KEY_HOME ||
               key == LV_KEY_END) {
        replace_all = 0;
        lv_textarea_clear_selection(entry);
    }
}
static void button_key(lv_event_t *event) {
    if (lv_event_get_key(event) == LV_KEY_ESC)
        clear();
}
static void action(unsigned code) {
    char buffer[130];
    /* The calculator's display is one expression. Copy/cut/select-all apply
     * to that whole expression, including after a result is calculated. */
    if (code == GX_ACTION_PASTE) {
        if (gx_lv_clipboard(buffer, sizeof(buffer), 0) == 0) {
            unsigned i;
            for (i = 0; buffer[i]; i++)
                if (!strchr("0123456789.+-*/()eE \t", buffer[i])) {
                    lv_label_set_text(result, "Paste a numeric expression");
                    return;
                }
            insert(buffer);
        } else
            lv_label_set_text(result, "Clipboard text is too long or unavailable");
    } else if (code == GX_ACTION_COPY || code == GX_ACTION_CUT) {
        strcpy(buffer, lv_textarea_get_text(entry));
        if (gx_lv_clipboard(buffer, sizeof(buffer), 1) == 0 && code == GX_ACTION_CUT)
            clear();
    } else if (code == GX_ACTION_SELECT_ALL) {
        /* Selecting the expression makes the next insertion replace it. */
        replace_all = 1;
        lv_group_focus_obj(entry);
        lv_label_set_text_selection_start(lv_textarea_get_label(entry), 0);
        lv_label_set_text_selection_end(lv_textarea_get_label(entry),
                                        strlen(lv_textarea_get_text(entry)));
    }
}
static void resize(unsigned w, unsigned h) {
    (void)w;
    (void)h;
    gx_lv_height(header, 24);
    ui_input_height(entry, 44);
    gx_lv_height(result, 20);
}
static void ui(void) {
    unsigned i;
    lv_obj_t *screen = lv_screen_active(), *mode;
    ui_screen();
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    header = ui_stack(screen, LV_FLEX_FLOW_ROW, UI_SPACE);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    title = ui_label(header, "Calculator", UI_HEADING);
    mode = ui_label(header, "Standard", UI_SECONDARY);
    (void)mode;
    entry = ui_input(screen, "0");
    ui_text_role(entry, UI_HEADING);
    lv_obj_set_width(entry, lv_pct(100));
    lv_textarea_set_max_length(entry, 128);
    lv_textarea_set_accepted_chars(entry, "0123456789.+-*/()eE \t");
    lv_obj_add_event_cb(entry, entry_key, LV_EVENT_KEY, NULL);
    result = ui_label(screen, "", UI_SECONDARY);
    lv_obj_set_width(result, lv_pct(100));
    lv_label_set_long_mode(result, LV_LABEL_LONG_MODE_DOTS);
    keypad = ui_stack(screen, LV_FLEX_FLOW_COLUMN, UI_SPACE);
    lv_obj_set_height(keypad, 0);
    lv_obj_set_flex_grow(keypad, 1);
    lv_obj_set_grid_dsc_array(keypad, columns, lines);
    for (i = 0; i < 20; i++) {
        buttons[i] = ui_button(keypad, keys[i], i == 19 ? UI_PRIMARY : UI_BUTTON);
        lv_obj_set_style_min_width(buttons[i], 0, 0);
        lv_obj_set_grid_cell(buttons[i], LV_GRID_ALIGN_STRETCH, i % 4, 1, LV_GRID_ALIGN_STRETCH,
                             i / 4, 1);
        lv_obj_add_event_cb(buttons[i], press, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        lv_obj_add_event_cb(buttons[i], button_key, LV_EVENT_KEY, NULL);
    }
    lv_group_focus_obj(entry);
    resize(300, 360);
}

int main(int argc, char **argv) {
    int status;
    trace = argc > 1 && !strcmp(argv[1], "--trace");
    if (argc > 1 && !trace) {
        puts("Usage: lvcalc [--trace]\nCalculator: type expressions; Enter evaluates, Esc clears, "
             "Tab selects buttons.");
        return !strcmp(argv[1], "--help") ? 0 : 1;
    }
    gx_lv_application("org.riplix.Calculator", "Calculator", "calculator");
    gx_lv_minimum(260, 320);
    if (gx_lv_open("Calculator", 300, 360) < 0)
        return 1;
    ui();
    gx_lv_handlers(text, action, resize);
    if (trace) {
        lv_color_t c = lv_obj_get_style_bg_color(lv_screen_active(), LV_PART_MAIN);
        lv_obj_update_layout(lv_screen_active());
        printf("CALC_READY bg=%u,%u,%u entry=%ld,%ld %ldx%ld\n", c.red, c.green, c.blue,
               (long)lv_obj_get_x(entry), (long)lv_obj_get_y(entry), (long)lv_obj_get_width(entry),
               (long)lv_obj_get_height(entry));
    }
    status = gx_lv_run();
    gx_lv_close();
    return status;
}
