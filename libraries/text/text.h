/* SPDX-License-Identifier: MIT */
#ifndef RIPPLE_TEXT_H
#define RIPPLE_TEXT_H
#include <stddef.h>
typedef struct TextBuffer TextBuffer;
#define TEXT_MAX 524288UL
#define TEXT_LINES 65536UL
/* Opaque gap buffer; all offsets and lengths count octets, not code points.
 * Mutation is transactional on allocation/validation failure. Undo groups retain
 * edits rather than whole-file snapshots. File and terminal I/O are caller policy. */
TextBuffer *text_new(void);
void text_free(TextBuffer *b);
const char *text_error(TextBuffer *b);
unsigned long text_length(TextBuffer *b);
int text_byte(TextBuffer *b, unsigned long at);
int text_copy(TextBuffer *b, unsigned long at, unsigned long n, unsigned char *out);
int text_append(TextBuffer *b, const unsigned char *s, unsigned long n);
int text_replace(TextBuffer *b, unsigned long at, unsigned long remove, const unsigned char *s, unsigned long n);
void text_begin(TextBuffer *b, unsigned long cursor);
void text_end(TextBuffer *b, unsigned long cursor);
void text_saved(TextBuffer *b);
unsigned long text_revision(TextBuffer *b);
/* Display invalidation token: changes on every mutation (including edits within
 * one undo group), undo, redo and language changes. Compare for equality only. */
unsigned long text_generation(TextBuffer *b);
int text_modified(TextBuffer *b);
long text_undo(TextBuffer *b);
long text_redo(TextBuffer *b);
unsigned long text_lines(TextBuffer *b);
unsigned long text_row(TextBuffer *b, unsigned long at);
unsigned long text_start(TextBuffer *b, unsigned long row);
unsigned long text_endline(TextBuffer *b, unsigned long row);
unsigned long text_visual(TextBuffer *b, unsigned long at, unsigned tabstop);
unsigned long text_column(TextBuffer *b, unsigned long row, unsigned long column, unsigned tabstop);
unsigned long text_word(TextBuffer *b, unsigned long at, int motion, unsigned count);
long text_find(TextBuffer *b, unsigned long at, const unsigned char *s, unsigned long n, int backward);
long text_find_char(TextBuffer *b, unsigned long at, int ch, int backward, int till, unsigned count);
long text_newline(TextBuffer *b, unsigned long at, int indent);
long text_open_line(TextBuffer *b, unsigned long at, int above, int indent);
int text_indent(TextBuffer *b, unsigned long first, unsigned long last, unsigned width, int remove, int tabs);
/* Object bounds are half-open; supports iw/aw, i()/a(), [] {} <> and quotes. */
int text_object(TextBuffer *b, unsigned long at, int object, int around, unsigned long *bounds);
long text_match(TextBuffer *b, unsigned long at);
/* language: 0 text, 1 C/C++/RFL/Rust/Java/JS, 2 shell/Tcl/awk, 3 Python,
 * 4 Pascal, 5 JSON, 6 INI, 7 Markdown, 8 Brainfuck, 9 make, 10 HTML/CSS,
 * 11 Lisp, 12 assembler. Styles: plain/comment/keyword/string/number/operator/type. */
void text_language(TextBuffer *b, unsigned language);
int text_render(TextBuffer *b, unsigned long row, unsigned long left, unsigned width,
                unsigned tabstop, const unsigned long *styles, unsigned long selection,
                unsigned long select_start, unsigned long select_end, unsigned long *cells);
unsigned long text_hash(const unsigned char *bytes, unsigned long n, unsigned long seed);
#endif
