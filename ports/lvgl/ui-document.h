/* SPDX-License-Identifier: MIT */
#ifndef GRAPHITE_UI_DOCUMENT_H
#define GRAPHITE_UI_DOCUMENT_H
#include "../../libraries/text/text.h"
#define UI_DOCUMENT_TABS 16
#define UI_DOCUMENT_RECENTS 8
#define UI_DOCUMENT_PATH 2048
/* A GUI-independent document session. Files remain engine-native UTF-8 octets;
 * file operations never replace a buffer until the whole operation succeeds. */
typedef struct ui_documents ui_documents;
typedef struct ui_document ui_document;
enum ui_document_result { UI_DOCUMENT_ERROR = -1, UI_DOCUMENT_OK = 0,
                           UI_DOCUMENT_CHANGED = 1, UI_DOCUMENT_EXISTS = 2,
                           UI_DOCUMENT_DIRTY = 3 };
enum ui_document_save_flags { UI_DOCUMENT_REPLACE = 1, UI_DOCUMENT_FORCE = 2 };
/* Friendly choices may share an engine lexer. Automatic follows the filename;
 * explicit choices belong to this open document, survive save/reload, and are
 * not persisted in HOME settings. Changing syntax never edits text/history. */
enum ui_document_language_id {
    UI_LANGUAGE_AUTOMATIC, UI_LANGUAGE_TEXT, UI_LANGUAGE_C, UI_LANGUAGE_CPP,
    UI_LANGUAGE_RFL, UI_LANGUAGE_RUST, UI_LANGUAGE_JAVA, UI_LANGUAGE_SHELL,
    UI_LANGUAGE_TCL, UI_LANGUAGE_AWK, UI_LANGUAGE_PYTHON, UI_LANGUAGE_PASCAL,
    UI_LANGUAGE_JSON, UI_LANGUAGE_INI, UI_LANGUAGE_MARKDOWN, UI_LANGUAGE_BRAINFUCK,
    UI_LANGUAGE_MAKEFILE, UI_LANGUAGE_HTML, UI_LANGUAGE_CSS, UI_LANGUAGE_LISP,
    UI_LANGUAGE_SCHEME, UI_LANGUAGE_ASSEMBLY, UI_LANGUAGE_BASIC,
    UI_LANGUAGE_JAVASCRIPT, UI_LANGUAGE_SYMA, UI_LANGUAGE_COUNT
};
struct ui_document_language { unsigned id, lexer; const char *name; };
unsigned ui_document_language_count(void);
const struct ui_document_language *ui_document_language_at(unsigned index);
unsigned ui_document_get_language(ui_document *document);
unsigned ui_document_effective_language(ui_document *document);
const char *ui_document_language_name(ui_document *document);
/* 0 success, -1 invalid document/choice. Cursor, selection, dirty state and
 * undo history are unchanged; graphical consumers should invalidate syntax. */
int ui_document_set_language(ui_document *document, unsigned language);
ui_documents *ui_documents_create(const char *home);
void ui_documents_delete(ui_documents *session);
const char *ui_documents_error(ui_documents *session);
unsigned ui_documents_count(ui_documents *session);
ui_document *ui_documents_at(ui_documents *session, unsigned index);
ui_document *ui_documents_active(ui_documents *session);
int ui_documents_select(ui_documents *session, ui_document *document);
ui_document *ui_documents_new(ui_documents *session);
ui_document *ui_documents_open(ui_documents *session, const char *path);
/* Closing a dirty document requires explicit discard. No replacement tab is
 * silently created: the application chooses its empty-workspace policy. */
int ui_documents_close(ui_documents *session, ui_document *document, int discard);
TextBuffer *ui_document_text(ui_document *document);
const char *ui_document_path(ui_document *document);
const char *ui_document_name(ui_document *document);
void ui_document_view(ui_document *document, unsigned long cursor, unsigned long anchor);
void ui_document_get_view(ui_document *document, unsigned long *cursor, unsigned long *anchor);
/* Save uses a same-directory exclusive temporary and rename, preserving mode.
 * Existing symbolic links are rejected; choose a regular-file path instead.
 * REPLACE authorizes replacing a different existing destination; FORCE also
 * authorizes changes to the original file since its last successful load/save.
 * The guest provides atomic replacement, not a power-loss durability guarantee.
 * A final external-change recheck narrows the race; this is not an OS file lock. */
int ui_document_save(ui_documents *session, ui_document *document, const char *path,
                     unsigned flags);
int ui_document_reload(ui_documents *session, ui_document *document, int discard);
/* 1 changed/deleted/replaced, 0 unchanged, -1 I/O error. Content hash catches
 * same-size edits even when the guest's timestamp resolution is one second. */
int ui_document_external(ui_documents *session, ui_document *document);
unsigned ui_documents_recent_count(ui_documents *session);
const char *ui_documents_recent(ui_documents *session, unsigned index);
void ui_documents_clear_recent(ui_documents *session);
int ui_documents_wrap(ui_documents *session);
void ui_documents_set_wrap(ui_documents *session, int wrap);
/* HOME/.lvedit; bounded parsing, escaped paths, atomic 0600 replacement. */
int ui_documents_settings_save(ui_documents *session);
#endif
