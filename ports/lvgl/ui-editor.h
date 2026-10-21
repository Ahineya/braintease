/* SPDX-License-Identifier: MIT */
#ifndef GRAPHITE_UI_EDITOR_H
#define GRAPHITE_UI_EDITOR_H
#include "ui.h"
#include "../../libraries/text/text.h"

typedef struct ui_editor ui_editor;
/* A graphical viewport over a borrowed TextBuffer. Deleting its LVGL object
 * frees the viewport, never the document. No second textarea model is kept.
 * Geometry uses actual glyph advances in backing pixels, not terminal cells.
 * Root VALUE_CHANGED covers edits, caret and selection changes. */
ui_editor *ui_editor_create(lv_obj_t *parent, TextBuffer *buffer);
lv_obj_t *ui_editor_object(ui_editor *editor);
void ui_editor_buffer(ui_editor *editor, TextBuffer *buffer);
void ui_editor_changed(ui_editor *editor);
unsigned long ui_editor_cursor(ui_editor *editor);
/* Half-open, sorted octet bounds, always on extended-grapheme boundaries. */
void ui_editor_selection(ui_editor *editor, unsigned long *bounds);
void ui_editor_select(ui_editor *editor, unsigned long anchor, unsigned long cursor);
void ui_editor_wrap(ui_editor *editor, int enabled);
int ui_editor_get_wrap(ui_editor *editor);
void ui_editor_density(ui_editor *editor);
void ui_editor_focus(ui_editor *editor);
int ui_editor_focused(ui_editor *editor);
/* Insert a complete UTF-8/paste batch in one undo group, replacing selection.
 * Octets are retained by the engine; malformed input displays as replacement
 * characters. Returns 0 on success, -1 on allocation/document-limit failure. */
int ui_editor_text(ui_editor *editor, const char *text, unsigned long length);
int ui_editor_codepoint(ui_editor *editor, uint32_t codepoint);
/* Forward GX raw USB keys and shared GX_ACTION_* clipboard actions. Both
 * return nonzero when consumed; modifiers: bit0 Control/Command, bit1 Shift.
 * Text/IME input comes separately through ui_editor_codepoint, never raw keys. */
int ui_editor_key(ui_editor *editor, unsigned key, int down, unsigned modifiers);
int ui_editor_action(ui_editor *editor, unsigned action);
/* Graphical hit testing and caret geometry for accessibility/tests. Coordinates
 * are relative to the object's content viewport, in backing pixels. */
unsigned long ui_editor_hit(ui_editor *editor, int32_t x, int32_t y);
int ui_editor_caret(ui_editor *editor, int32_t *x, int32_t *y, int32_t *height);
/* Layout is performed in bounded idle batches. It is safe to poll ready for
 * tests; painting never allocates a widget for every character/document row. */
int ui_editor_ready(ui_editor *editor);
unsigned long ui_editor_display_rows(ui_editor *editor);
const char *ui_editor_error(ui_editor *editor);
#endif
