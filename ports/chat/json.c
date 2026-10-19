/* SPDX-License-Identifier: MIT */
/* Small strict JSON reader. Offsets are wide even when guest int/char are C16. */
#include "json.h"
#include <stdlib.h>
#include <string.h>

static int hex(unsigned c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
static int scalar(const char *s, unsigned long n, unsigned long *at, unsigned long *value) {
    unsigned c, need, i;
    unsigned long v, min;
    if (*at == n)
        return 0;
    c = (unsigned char)s[(*at)++];
    if (c < 128) {
        *value = c;
        return 1;
    }
    if (c >= 194 && c <= 223) {
        need = 1;
        v = c & 31;
        min = 128;
    } else if (c >= 224 && c <= 239) {
        need = 2;
        v = c & 15;
        min = 2048;
    } else if (c >= 240 && c <= 244) {
        need = 3;
        v = c & 7;
        min = 65536UL;
    } else
        return 0;
    for (i = 0; i < need; i++) {
        if (*at == n)
            return 0;
        c = (unsigned char)s[(*at)++];
        if (c < 128 || c > 191)
            return 0;
        v = (v << 6) | (c & 63);
    }
    if (v < min || v > 1114111UL || (v >= 55296UL && v <= 57343UL))
        return 0;
    *value = v;
    return 1;
}
int cj_utf8(const char *s, unsigned long n) {
    unsigned long at = 0, v;
    while (at < n)
        if (!scalar(s, n, &at, &v))
            return 0;
    return 1;
}
/* History may include untrusted terminal escapes from model/tool output. */
void cj_print(FILE *out, const char *s) {
    unsigned long at = 0, start, value, n = strlen(s);
    while (at < n) {
        start = at;
        if (!scalar(s, n, &at, &value)) { fputc('?', out); at = start + 1; }
        else if ((value < 32 && value != 9 && value != 10) || (value >= 127 && value < 160))
            fputc('?', out);
        else fwrite(s + start, 1, at - start, out);
    }
}
static int unit(CJ *j, unsigned long *v) {
    unsigned i;
    int h;
    *v = 0;
    for (i = 0; i < 4; i++) {
        if (j->at == j->size || (h = hex(j->text[j->at++])) < 0)
            return 0;
        *v = (*v << 4) | h;
    }
    return 1;
}
static int quoted(CJ *j) {
    unsigned c;
    unsigned long v, low;
    ++j->at;
    while (j->at < j->size) {
        c = (unsigned char)j->text[j->at++];
        if (c == '"')
            return 1;
        if (c < 32 || c > 255)
            return 0;
        if (c == '\\') {
            if (j->at == j->size)
                return 0;
            c = j->text[j->at++];
            if (c == 'u') {
                if (!unit(j, &v))
                    return 0;
                if (v >= 55296UL && v <= 56319UL) {
                    if (j->at + 2 > j->size || j->text[j->at++] != '\\' ||
                        j->text[j->at++] != 'u' || !unit(j, &low) || low < 56320UL || low > 57343UL)
                        return 0;
                } else if (v >= 56320UL && v <= 57343UL)
                    return 0;
            } else if (!c || !strchr("\"\\/bfnrt", c))
                return 0;
        } else if (c >= 128) {
            --j->at;
            if (!scalar(j->text, j->size, &j->at, &v))
                return 0;
        }
    }
    return 0;
}
static void space(CJ *j) {
    while (j->at < j->size && j->text[j->at] && strchr(" \r\n\t", j->text[j->at]))
        ++j->at;
}
static int value(CJ *j, unsigned depth) {
    unsigned token, c;
    unsigned long begin;
    int key;
    space(j);
    if (depth > 32 || j->count == CJ_TOKENS || j->at == j->size)
        return 0;
    token = j->count++;
    begin = j->at;
    c = j->text[j->at];
    j->tokens[token].start = begin;
    j->tokens[token].kind = c;
    if (c == '"') {
        if (!quoted(j))
            return 0;
    } else if (c == '{' || c == '[') {
        ++j->at;
        space(j);
        if (j->at < j->size && j->text[j->at] == (c == '{' ? '}' : ']'))
            ++j->at;
        else
            for (;;) {
                if (c == '{') {
                    space(j);
                    if (j->at == j->size || j->text[j->at] != '"')
                        return 0;
                    key = j->count;
                    if (!value(j, depth + 1))
                        return 0;
                    space(j);
                    if (j->at == j->size || j->text[j->at++] != ':')
                        return 0;
                    /* Duplicate names would make credential/config interpretation ambiguous. */
                    {
                        unsigned previous;
                        char *name = cj_string(j, key), *old;
                        if (!name)
                            return 0;
                        for (previous = token + 1; previous < (unsigned)key;
                             previous = j->tokens[previous + 1].after) {
                            old = cj_string(j, previous);
                            if (!old || !strcmp(name, old)) {
                                free(old);
                                free(name);
                                return 0;
                            }
                            free(old);
                        }
                        free(name);
                    }
                }
                if (!value(j, depth + 1))
                    return 0;
                space(j);
                if (j->at == j->size)
                    return 0;
                if (j->text[j->at] == (c == '{' ? '}' : ']')) {
                    ++j->at;
                    break;
                }
                if (j->text[j->at++] != ',')
                    return 0;
            }
    } else if (c == 't' || c == 'f' || c == 'n') {
        const char *literal = c == 't' ? "true" : c == 'f' ? "false" : "null";
        unsigned n = strlen(literal);
        if (j->size - j->at < n || memcmp(j->text + j->at, literal, n))
            return 0;
        j->at += n;
    } else {
        if (c == '-')
            ++j->at;
        if (j->at == j->size)
            return 0;
        if (j->text[j->at] == '0')
            ++j->at;
        else {
            if (j->text[j->at] < '1' || j->text[j->at] > '9')
                return 0;
            do {
                ++j->at;
            } while (j->at < j->size && j->text[j->at] >= '0' && j->text[j->at] <= '9');
        }
        if (j->at < j->size && j->text[j->at] == '.') {
            ++j->at;
            begin = j->at;
            while (j->at < j->size && j->text[j->at] >= '0' && j->text[j->at] <= '9')
                ++j->at;
            if (begin == j->at)
                return 0;
        }
        if (j->at < j->size && (j->text[j->at] == 'e' || j->text[j->at] == 'E')) {
            ++j->at;
            if (j->at < j->size && (j->text[j->at] == '+' || j->text[j->at] == '-'))
                ++j->at;
            begin = j->at;
            while (j->at < j->size && j->text[j->at] >= '0' && j->text[j->at] <= '9')
                ++j->at;
            if (begin == j->at)
                return 0;
        }
        j->tokens[token].kind = '0';
    }
    j->tokens[token].end = j->at;
    j->tokens[token].after = j->count;
    return 1;
}
int cj_parse(CJ *j, const char *text, unsigned long n) {
    memset(j, 0, sizeof(*j));
    j->text = text;
    j->size = n;
    j->tokens = calloc(CJ_TOKENS, sizeof(*j->tokens));
    if (!j->tokens)
        return 0;
    if (value(j, 0)) {
        space(j);
        if (j->at == n)
            return 1;
    }
    cj_free(j);
    return 0;
}
void cj_free(CJ *j) {
    free(j->tokens);
    j->tokens = NULL;
    j->count = 0;
}
int cj_field(const CJ *j, unsigned object, const char *name) {
    unsigned i;
    if (object >= j->count || j->tokens[object].kind != '{')
        return -1;
    for (i = object + 1; i < j->tokens[object].after; i = j->tokens[i + 1].after)
        if (cj_equal(j, i, name))
            return i + 1;
    return -1;
}
int cj_equal(const CJ *j, unsigned at, const char *value) {
    char *s;
    int equal;
    if (at >= j->count)
        return 0;
    s = cj_string(j, at);
    if (!s)
        return 0;
    equal = !strcmp(s, value);
    free(s);
    return equal;
}
static unsigned encode(unsigned long v, char *p) {
    if (v < 128) {
        p[0] = v;
        return 1;
    }
    if (v < 2048) {
        p[0] = 192 | (v >> 6);
        p[1] = 128 | (v & 63);
        return 2;
    }
    if (v < 65536UL) {
        p[0] = 224 | (v >> 12);
        p[1] = 128 | ((v >> 6) & 63);
        p[2] = 128 | (v & 63);
        return 3;
    }
    p[0] = 240 | (v >> 18);
    p[1] = 128 | ((v >> 12) & 63);
    p[2] = 128 | ((v >> 6) & 63);
    p[3] = 128 | (v & 63);
    return 4;
}
char *cj_string(const CJ *j, unsigned token) {
    CJ read;
    char *out;
    unsigned long used = 0, v, low;
    unsigned c;
    if (token >= j->count || j->tokens[token].kind != '"')
        return NULL;
    read = *j;
    read.at = j->tokens[token].start + 1;
    read.size = j->tokens[token].end - 1;
    out = malloc(read.size - read.at + 1);
    if (!out)
        return NULL;
    while (read.at < read.size) {
        c = (unsigned char)read.text[read.at++];
        if (c == '\\') {
            c = read.text[read.at++];
            if (c == 'u') {
                if (!unit(&read, &v))
                    goto bad;
                if (v >= 55296UL && v <= 56319UL) {
                    read.at += 2;
                    if (!unit(&read, &low))
                        goto bad;
                    v = 65536UL + ((v - 55296UL) << 10) + low - 56320UL;
                }
                if (!v)
                    goto bad;
                used += encode(v, out + used);
                continue;
            }
            if (c == 'b')
                c = 8;
            else if (c == 'f')
                c = 12;
            else if (c == 'n')
                c = 10;
            else if (c == 'r')
                c = 13;
            else if (c == 't')
                c = 9;
        }
        out[used++] = c;
    }
    out[used] = 0;
    return out;
bad:
    free(out);
    return NULL;
}
int cj_number(const CJ *j, unsigned token, unsigned long *out) {
    unsigned long i, n = 0;
    unsigned c;
    if (token >= j->count || j->tokens[token].kind != '0')
        return 0;
    for (i = j->tokens[token].start; i < j->tokens[token].end; i++) {
        c = j->text[i];
        if (c < '0' || c > '9' || n > (0xffffffffUL - (c - '0')) / 10)
            return 0;
        n = n * 10 + c - '0';
    }
    *out = n;
    return 1;
}
int cj_quote(FILE *out, const char *s) {
    unsigned c;
    if (!cj_utf8(s, strlen(s)) || fputc('"', out) == EOF)
        return 0;
    while ((c = (unsigned char)*s++) != 0) {
        if (c == '"' || c == '\\') {
            if (fputc('\\', out) == EOF || fputc(c, out) == EOF)
                return 0;
        } else if (c < 32) {
            if (fprintf(out, "\\u%04x", c) < 0)
                return 0;
        } else if (fputc(c, out) == EOF)
            return 0;
    }
    return fputc('"', out) != EOF;
}
