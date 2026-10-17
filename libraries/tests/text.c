#include "../text/text.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void equals(TextBuffer *b, const char *s) {
    unsigned long n = strlen(s), i, row = 0; unsigned char *copy = malloc(n + 1);
    assert(text_length(b) == n && text_copy(b, 0, n, copy) == 0 && !memcmp(copy, s, n));
    assert(text_start(b, 0) == 0);
    for (i = 0; i < n; ++i) { assert(text_row(b, i) == row); if (s[i] == '\n') assert(text_start(b, ++row) == i + 1); }
    assert(text_lines(b) == row + 1); free(copy);
}
static void edits(void) {
    TextBuffer *b = text_new(); unsigned long range[2];
    assert(b && !text_append(b, (const unsigned char *)"call(a, nested(b));\n", 20));
    equals(b, "call(a, nested(b));\n");
    assert(!text_object(b, 15, ')', 0, range) && range[0] == 15 && range[1] == 16);
    text_begin(b, 15); assert(!text_replace(b, range[0], range[1] - range[0], (const unsigned char *)"value", 5));
    assert(text_modified(b)); text_saved(b); assert(!text_modified(b));
    assert(text_undo(b) == 15 && text_modified(b)); equals(b, "call(a, nested(b));\n");
    assert(text_redo(b) == 20 && !text_modified(b));
    text_begin(b, 0); assert(!text_replace(b, 0, 0, (const unsigned char *)"x", 1));
    assert(!text_replace(b, 1, 0, (const unsigned char *)"y", 1)); text_end(b, 2);
    assert(text_undo(b) == 0 && !text_modified(b)); assert(text_redo(b) == 2);
    assert(text_replace(b, 0, 0, (const unsigned char *)"\0", 1) < 0);
    assert(text_replace(b, text_length(b) + 1, 0, (const unsigned char *)"x", 1) < 0);
    text_free(b);
    b = text_new(); assert(!text_append(b, (const unsigned char *)"    a\r\nb\r\n", 10));
    assert(text_endline(b, 0) == 5 && text_start(b, 1) == 7);
    text_begin(b, 5); assert(text_newline(b, 5, 1) == 11); equals(b, "    a\r\n    \r\nb\r\n");
    assert(text_undo(b) == 5); equals(b, "    a\r\nb\r\n");
    text_begin(b, 0); assert(!text_indent(b, 0, 1, 4, 0, 0)); equals(b, "        a\r\n    b\r\n");
    assert(text_undo(b) == 0); text_free(b);
}
static void structure(void) {
    TextBuffer *b = text_new(); unsigned long range[2];
    const char *s = "call(\")\", /* ) */ inner(x)); // (\n";
    assert(!text_append(b, (const unsigned char *)s, strlen(s))); text_language(b, 1);
    assert(!text_object(b, 4, ')', 0, range) && range[0] == 5 && range[1] == 26);
    assert(text_match(b, 4) == 26 && text_match(b, 26) == 4);
    assert(text_match(b, 6) < 0 && text_match(b, 13) < 0);
    text_free(b);
}
static void randomized(void) {
    TextBuffer *b = text_new(); char model[8192] = {0}, prior[8192], add[10]; unsigned n = 0, iteration;
    srand(101);
    for (iteration = 0; iteration < 1500; ++iteration) {
        unsigned at = rand() % (n + 1), remove = n - at, length = rand() % 8, i;
        if (remove > 8) remove = 8; remove = rand() % (remove + 1);
        for (i = 0; i < length; ++i) add[i] = rand() % 5 == 0 ? '\n' : 'a' + rand() % 26;
        strcpy(prior, model); text_begin(b, at);
        assert(!text_replace(b, at, remove, (const unsigned char *)add, length)); text_end(b, at + length);
        memmove(model + at + length, model + at + remove, n - at - remove + 1); memcpy(model + at, add, length); n = n - remove + length;
        equals(b, model);
        if ((remove || length) && iteration % 7 == 0) { assert(text_undo(b) == at); equals(b, prior); assert(text_redo(b) == at + length); equals(b, model); }
    }
    text_free(b);
}
static void highlighting(void) {
    TextBuffer *b = text_new(); unsigned long cells[20], styles[7] = {0,256,512,768,1024,1280,1536};
    assert(!text_append(b, (const unsigned char *)"/* comment\nstill */ let x = 42;\n", strlen("/* comment\nstill */ let x = 42;\n"))); text_language(b, 1);
    assert(!text_render(b, 1, 0, 20, 4, styles, 2048, 0, 0, cells));
    assert((cells[0] & ~255UL) == styles[1] && (cells[9] & ~255UL) == styles[2]);
    text_begin(b, 0); assert(!text_replace(b, 0, 2, (const unsigned char *)"xx", 2));
    assert(!text_render(b, 1, 0, 20, 4, styles, 2048, 0, 0, cells)); assert((cells[0] & ~255UL) == 0);
    text_free(b);
}
static void keyword_case(unsigned language, const char *token, unsigned kind) {
    TextBuffer *b = text_new(); unsigned long cells[64], styles[7] = {0,256,512,768,1024,1280,1536};
    assert(b && !text_append(b, (const unsigned char *)token, strlen(token)));
    text_language(b, language);
    assert(!text_render(b, 0, 0, 64, 4, styles, 2048, 0, 0, cells));
    assert((cells[0] & ~255UL) == styles[kind]);
    text_free(b);
}
static void keywords(void) {
    keyword_case(1, "Err", 2); keyword_case(1, "while", 2);
    keyword_case(1, "do", 2); keyword_case(1, "double", 2);
    keyword_case(1, "int", 2); keyword_case(1, "interface", 2);
    keyword_case(1, "integer", 0); keyword_case(1, "function", 0);
    keyword_case(1, "String", 2); keyword_case(1, "StringBuilder", 6);
    keyword_case(1, "identifier_longer_than_the_token_buffer_0123456789", 0);
    keyword_case(4, "BEGIN", 2); keyword_case(4, "XoR", 2);
    keyword_case(4, "beginnings", 0);
    keyword_case(2, "BEGIN", 2); keyword_case(2, "begin", 0);
    keyword_case(3, "False", 2); keyword_case(3, "false", 0);
    keyword_case(5, "false", 2); keyword_case(5, "False", 6);
    keyword_case(11, "defparameter", 2); keyword_case(11, "when", 2);
}
static void display_generation(void) {
    TextBuffer *b = text_new(); unsigned long generation, revision;
    generation = text_generation(b);
    assert(!text_append(b, (const unsigned char *)"a", 1) && text_generation(b) != generation);
    text_saved(b); text_begin(b, 1);
    assert(!text_replace(b, 1, 0, (const unsigned char *)"b", 1));
    generation = text_generation(b); revision = text_revision(b);
    assert(!text_replace(b, 2, 0, (const unsigned char *)"c", 1));
    assert(text_revision(b) == revision && text_generation(b) != generation);
    generation = text_generation(b); assert(text_undo(b) == 1 && text_generation(b) != generation);
    generation = text_generation(b); assert(text_redo(b) == 3 && text_generation(b) != generation);
    generation = text_generation(b); text_language(b, 1); assert(text_generation(b) != generation);
    generation = text_generation(b);
    assert(text_replace(b, 0, 0, (const unsigned char *)"\0", 1) < 0 && text_generation(b) == generation);
    assert(!text_replace(b, 0, 0, NULL, 0) && text_generation(b) == generation);
    text_free(b);
}
static void banks(void) {
    TextBuffer *b = text_new(); unsigned char *bytes = malloc(70000); unsigned i;
    for (i = 0; i < 70000; ++i) bytes[i] = i % 80 == 79 ? '\n' : 'a';
    assert(!text_append(b, bytes, 70000)); text_begin(b, 65535);
    assert(!text_replace(b, 65535, 5, (const unsigned char *)"BANK\n", 5)); assert(text_byte(b, 65535) == 'B');
    assert(text_undo(b) == 65535 && text_byte(b, 65535) == 'a');
    assert(text_replace(b, 0, 0, bytes, TEXT_MAX + 1) < 0); /* validate size before reading input */
    free(bytes); text_free(b);
}
int main(void) { edits(); structure(); randomized(); highlighting(); keywords(); display_generation(); banks(); puts("Text engine: grouped history, randomized edits/line index, CRLF, keyword lookup, syntax/display invalidation and bank boundary passed"); return 0; }
