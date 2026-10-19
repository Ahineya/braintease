/* SPDX-License-Identifier: MIT */
#include "ini.h"
#include <stdlib.h>
#include <string.h>
struct IniReader {
    char *text, *position, *end, *key, *value;
    char *section;
    size_t max_line;
    unsigned long max_entries;
    unsigned long line;
    unsigned long entries;
    int error;
};
static int space(int c) { return c == ' ' || c == '\t' || c == '\r'; }
static char *trim(char *s) {
    char *e;
    while (space(*s)) ++s;
    e = s + strlen(s);
    while (e > s && space(e[-1])) --e;
    *e = 0;
    return s;
}
IniReader *ini_reader(const char *text, size_t length) {
    return ini_reader_with_limits(text, length, 65536UL, 1024, 256);
}
IniReader *ini_reader_with_limits(const char *text, size_t length,
    size_t max_input, size_t max_line, unsigned long max_entries) {
    IniReader *r = calloc(1, sizeof(*r));
    size_t section_size = max_line < length ? max_line : length;
    if (!r) return NULL;
    r->max_line = max_line; r->max_entries = max_entries;
    if (length > max_input || length == (size_t)-1) { r->error = INI_LIMIT; return r; }
    if (!text && length) { r->error = INI_SYNTAX; return r; }
    r->text = malloc(length + 1);
    r->section = calloc(section_size + 1, 1);
    if (!r->text || !r->section) { ini_free(r); return NULL; }
    if (length) memcpy(r->text, text, length);
    r->text[length] = 0;
    r->position = r->text; r->end = r->text + length;
    return r;
}
void ini_free(IniReader *r) { if (r) { free(r->section); free(r->text); free(r); } }
int ini_next(IniReader *r) {
    char *s, *end, *equal, *value;
    size_t length;
    if (!r) return INI_MEMORY;
    if (r->error) return r->error;
    while (r->position < r->end) {
        ++r->line;
        s = r->position; end = s;
        while (end < r->end && *end != '\n') {
            if (!*end) return r->error = INI_SYNTAX;
            ++end;
        }
        length = end - s;
        if (length > r->max_line) return r->error = INI_LIMIT;
        r->position = end < r->end ? end + 1 : end;
        *end = 0; s = trim(s);
        if (!*s || *s == '#' || *s == ';') continue;
        if (*s == '[') {
            length = strlen(s);
            if (length < 3 || s[length - 1] != ']') return r->error = INI_SYNTAX;
            s[length - 1] = 0; s = trim(s + 1);
            if (!*s) return r->error = INI_SYNTAX;
            strcpy(r->section, s);
            continue;
        }
        equal = strchr(s, '=');
        if (!equal) return r->error = INI_SYNTAX;
        *equal = 0; r->key = trim(s);
        if (!*r->key) return r->error = INI_SYNTAX;
        value = trim(equal + 1); length = strlen(value);
        if (length >= 2 && ((*value == '"' && value[length - 1] == '"') ||
                           (*value == '\'' && value[length - 1] == '\''))) {
            value[length - 1] = 0; ++value;
        }
        if (r->entries == r->max_entries) return r->error = INI_LIMIT;
        ++r->entries; r->value = value;
        return 1;
    }
    return 0;
}
unsigned long ini_line(const IniReader *r) { return r ? r->line : 0; }
const char *ini_section(const IniReader *r) { return r->section; }
const char *ini_key(const IniReader *r) { return r->key; }
const char *ini_value(const IniReader *r) { return r->value; }
