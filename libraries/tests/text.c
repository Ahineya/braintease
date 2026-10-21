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
    b = text_new(); assert(!text_append(b, (const unsigned char *)"a\tb", 3));
    assert(!text_render(b, 0, 0, 4, 4, styles, 2048, 0, 0, cells) && text_overflow(b));
    assert(!text_render(b, 0, 1, 4, 4, styles, 2048, 0, 0, cells) && !text_overflow(b));
    assert(cells[3] == 'b'); text_free(b);
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
    keyword_case(13, "PRINT", 2); keyword_case(13, "eLsEiF", 2);
    keyword_case(13, "printable", 0);
    keyword_case(13, "' comment with (brackets)", 1);
    keyword_case(13, "rEm comment", 1);
    keyword_case(13, "remember", 0);
    keyword_case(13, "\"a\"\"b\\c\"", 3);
}

static void javascript_token(TextBuffer *b, unsigned long row, const char *line, const char *token, unsigned kind) {
    unsigned long cells[160], styles[7] = {0,256,512,768,1024,1280,1536};
    const char *at = strstr(line, token); unsigned i;
    assert(at && !text_render(b, row, 0, 160, 4, styles, 2048, 0, 0, cells));
    for (i = 0; i < strlen(token); ++i) assert((cells[at - line + i] & ~255UL) == styles[kind]);
}
static void syma(void) {
    const char *first = "{R \"a\" {Foo x_ xs..} {Add -1.2e+3 x_} :guard True} ; comment";
    const char *source = "{R \"a\" {Foo x_ xs..} {Add -1.2e+3 x_} :guard True} ; comment\n"
        "/* block\nstill */ {Module Demo}\n{Print r\"raw\\\" tail}\n\"multi\nline\"\n";
    TextBuffer *b = text_new();
    assert(!text_append(b, (const unsigned char *)source, strlen(source))); text_language(b, 15);
    javascript_token(b,0,first,"R",2); javascript_token(b,0,first,"\"a\"",3);
    javascript_token(b,0,first,"x_",6); javascript_token(b,0,first,"xs..",6);
    javascript_token(b,0,first,"-1.2e+3",4); javascript_token(b,0,first,":guard",2);
    javascript_token(b,0,first,"; comment",1);
    javascript_token(b,2,"still */ {Module Demo}","still */",1);
    javascript_token(b,2,"still */ {Module Demo}","Module",2);
    javascript_token(b,3,"{Print r\"raw\\\" tail}","r\"raw\\\"",3);
    javascript_token(b,3,"{Print r\"raw\\\" tail}","tail",0);
    javascript_token(b,5,"line\"","line\"",3);
    assert(!text_replace(b,0,0,(const unsigned char *)"/*",2));
    javascript_token(b,0,"/*{R", "R",1);
    assert(!text_replace(b,0,2,NULL,0)); javascript_token(b,0,first,"R",2);
    text_free(b);
    source="{Outer r\"}\\\" ; }\n /* } */ {Inner \"}\"}}";
    b=text_new();assert(!text_append(b,(const unsigned char *)source,strlen(source)));text_language(b,15);
    assert(text_match(b,0)==(long)strlen(source)-1);
    assert(text_match(b,strstr(source,"/* }")-source+3)<0);
    text_free(b);
}
static void javascript(void) {
    const char *first = "async function load($value) { return /a[\\/]/gu.test($value) ? 1.5e-3 : 0xffn; } // tail";
    const char *source = "async function load($value) { return /a[\\/]/gu.test($value) ? 1.5e-3 : 0xffn; } // tail\n"
        "const note = `hello\nworld ${name}`;\n/* block\nstill */ var end = true;\nlet $return = total / count;\n";
    TextBuffer *b = text_new(); unsigned long bounds[2];
    assert(b && !text_append(b, (const unsigned char *)source, strlen(source))); text_language(b, 14);
    javascript_token(b, 0, first, "async", 2); javascript_token(b, 0, first, "function", 2);
    javascript_token(b, 0, first, "$value", 0); javascript_token(b, 0, first, "/a[\\/]/gu", 3);
    javascript_token(b, 0, first, "1.5e-3", 4); javascript_token(b, 0, first, "0xffn", 4);
    javascript_token(b, 0, first, "// tail", 1);
    javascript_token(b, 2, "world ${name}`;", "world ${name}`", 3);
    javascript_token(b, 4, "still */ var end = true;", "still */", 1);
    javascript_token(b, 4, "still */ var end = true;", "var", 2);
    javascript_token(b, 5, "let $return = total / count;", "$return", 0);
    javascript_token(b, 5, "let $return = total / count;", "/", 5);
    text_begin(b, 0); assert(!text_replace(b, 0, 0, (const unsigned char *)"/*", 2)); text_damage(b, bounds);
    javascript_token(b, 0, "/*async function", "async", 1);
    assert(text_undo(b) == 0);
    javascript_token(b, 0, first, "function", 2);
    text_free(b);
    keyword_case(14, "instanceof", 2); keyword_case(14, "yield", 2); keyword_case(14, "$return", 0);
    keyword_case(14, "fn", 0); keyword_case(14, "#!/bin/js", 1);
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
static void damage_rendering(unsigned language) {
    TextBuffer *b = text_new(); unsigned iteration, row, edit;
    unsigned long damage[2], cells[24][80], expected[80];
    unsigned long styles[7] = {0,256,512,768,1024,1280,1536};
    const char *parts[] = {"x", "/*", "*/", "\n", "\"\"\"", "(*", "*)", "'", "`", "\\", "\r\n"};
    unsigned char copy[8192];
    const char *initial = "fn main() {\n  let x = 12;\n  /* comment\n     continues */\n}\n";
    assert(b && !text_append(b, (const unsigned char *)initial, strlen(initial)));
    text_language(b, language); text_damage(b, damage);
    for (row = 0; row < 24; ++row) assert(!text_render(b, row, 0, 80, 4, styles, 2048, 0, 0, cells[row]));
    /* A local non-syntax edit converges immediately and touches one row. */
    assert(!text_replace(b, 0, 0, (const unsigned char *)"x", 1));
    text_damage(b, damage); assert(damage[0] == 0 && damage[1] == 1);
    assert(!text_render(b, 0, 0, 80, 4, styles, 2048, 0, 0, cells[0]));
    srand(438 + language);
    for (iteration = 0; iteration < 400; ++iteration) {
        TextBuffer *reference;
        for (edit = 0; edit < 3; ++edit) {
            unsigned long n = text_length(b), at = rand() % (n + 1), remove = n - at;
            const char *part = parts[rand() % (sizeof(parts)/sizeof(parts[0]))];
            if (remove > 4) remove = 4;
            remove = rand() % (remove + 1);
            text_begin(b, at);
            assert(!text_replace(b, at, remove, (const unsigned char *)part, strlen(part)));
            text_end(b, at + strlen(part));
        }
        if (iteration % 7 == 0) { assert(text_undo(b) >= 0); }
        if (iteration % 14 == 0) { assert(text_redo(b) >= 0); }
        text_damage(b, damage);
        assert(text_length(b) < sizeof(copy) && !text_copy(b, 0, text_length(b), copy));
        reference = text_new(); assert(reference);
        assert(!text_append(reference, copy, text_length(b))); text_language(reference, language);
        for (row = 0; row < 24; ++row) {
            if (row >= damage[0] && row < damage[1])
                assert(!text_render(b, row, 0, 80, 4, styles, 2048, 0, 0, cells[row]));
            assert(!text_render(reference, row, 0, 80, 4, styles, 2048, 0, 0, expected));
            assert(!memcmp(cells[row], expected, sizeof(expected)));
        }
        text_free(reference);
    }
    text_free(b);
}
static void wrapping(void) {
    TextBuffer *b = text_new(); unsigned long info[4], index; unsigned char out[128];
    const unsigned char *text = (const unsigned char *)"one two three four\n12345678\n\n";
    assert(b && !text_append(b, text, strlen((const char *)text)));
    assert(text_wrap_info(b, 0, 8, 4, info) == 1 && info[0] == 0 && info[1] == 0 && info[2] == 8 && info[3] == 0);
    assert(text_wrap_info(b, 1, 8, 4, info) == 1 && info[0] == 0 && info[1] == 8 && info[2] == 6 && info[3] == 8);
    assert(text_wrap_info(b, 2, 8, 4, info) == 1 && info[3] == 14);
    assert(text_wrap_at(b, 15, 8, 4) == 2);
    assert(text_wrap_column(b, 1, 2, 8, 4) == 10);
    assert(text_wrap_info(b, 4, 8, 4, info) == 1 && info[0] == 1 && !info[2]);
    assert(text_wrap_info(b, 7, 8, 4, info) == 0);
    text_begin(b, 0); assert(!text_replace(b, 0, 0, (const unsigned char *)"a\n", 2)); text_end(b, 2);
    assert(text_wrap_at(b, 17, 8, 4) == 3);
    assert(text_undo(b) == 0 && text_wrap_at(b, 15, 8, 4) == 2);
    assert(text_redo(b) == 2 && text_wrap_at(b, 17, 8, 4) == 3);
    assert(text_wrap_info(b, 0, 0, 4, info) == -1);
    text_free(b); b = text_new();
    assert(!text_append(b, (const unsigned char *)"one two three four", 18));
    text_begin(b, 18); assert(text_format(b, 18, 8, 4) == 18); text_end(b, 18);
    assert(!text_copy(b, 0, 18, out) && !memcmp(out, "one two\nthree\nfour", 18));
    assert(text_undo(b) == 18 && !text_copy(b, 0, 18, out) && !memcmp(out, "one two three four", 18));
    assert(text_format(b, 18, 0, 4) == 18);
    text_free(b); b = text_new();
    assert(!text_append(b, (const unsigned char *)"one two three\r\n", 15));
    text_begin(b, 13); assert(text_format(b, 13, 7, 4) == 14); text_end(b, 14);
    assert(!text_copy(b, 0, 16, out) && !memcmp(out, "one two\r\nthree\r\n", 16));
    assert(text_undo(b) == 13 && text_length(b) == 15);
    text_free(b); b = text_new();
    /* UTF-8 graphemes (CJK, combining mark, emoji ZWJ) never split. */
    text = (const unsigned char *)"\xe4\xb8\xad" "e\xcc\x81" "\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb";
    assert(!text_append(b, text, strlen((const char *)text)));
    index = text_wrap_at(b, 6, 3, 4);
    assert(index == 1 && text_wrap_info(b, index, 3, 4, info) == 1 && info[3] == 6);
    assert(text_wrap_column(b, 0, 2, 3, 4) == 3);
    assert(text_length(b) == strlen((const char *)text));
    text_free(b);
}
int main(void) { edits(); structure(); randomized(); highlighting(); keywords(); javascript(); syma(); display_generation(); banks(); damage_rendering(1); damage_rendering(3); damage_rendering(4); damage_rendering(14); damage_rendering(15); wrapping(); puts("Text engine: history, randomized edits/line index, CRLF, syntax convergence, accumulated damage, wrapping and bank boundary passed"); return 0; }
