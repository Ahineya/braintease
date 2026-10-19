/* SPDX-License-Identifier: MIT */
/* UTF-8/grapheme editor: cursor offsets always refer to the original text. */
#include "composer.h"
#include <ripple_unicode.h>
#include <string.h>

int chat_editor_insert(ChatEditor *e, const char *text, unsigned n) {
    unsigned size = strlen(e->text);
    if (n > CHAT_INPUT - size) return 0;
    memmove(e->text + e->cursor + n, e->text + e->cursor, size - e->cursor + 1);
    memcpy(e->text + e->cursor, text, n);
    e->cursor += n;
    return 1;
}
static unsigned previous(ChatEditor *e, unsigned at) {
    return ru_previous_grapheme((const unsigned char *)e->text, strlen(e->text), at);
}
static unsigned next(ChatEditor *e, unsigned at) {
    unsigned width;
    return ru_next_grapheme((const unsigned char *)e->text, strlen(e->text), at, &width);
}
static int category(ChatEditor *e, unsigned at) {
    size_t position = at;
    uint32_t cp = ru_next((const unsigned char *)e->text, strlen(e->text), &position);
    if (cp == ' ' || (cp >= 9 && cp <= 13) || cp == 160 || cp == 0x1680 ||
        (cp >= 0x2000 && cp <= 0x200a) || cp == 0x2028 || cp == 0x2029 ||
        cp == 0x202f || cp == 0x205f || cp == 0x3000) return 0;
    return cp >= 128 || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') ||
        (cp >= '0' && cp <= '9') || cp == '_' ? 1 : 2;
}
static void erase(ChatEditor *e, unsigned start, unsigned end, int save) {
    if (save && end > start) {
        memcpy(e->killed, e->text + start, end - start);
        e->killed[end - start] = 0;
    }
    memmove(e->text + start, e->text + end, strlen(e->text + end) + 1);
    e->cursor = start;
}
void chat_editor_action(ChatEditor *e, unsigned action) {
    unsigned at = e->cursor, size = strlen(e->text), end;
    switch (action) {
    case EDIT_LEFT: e->cursor = previous(e, at); break;
    case EDIT_RIGHT: e->cursor = next(e, at); break;
    case EDIT_WORD_LEFT: case EDIT_WORD_ERASE:
        while (at && (action == EDIT_WORD_ERASE ? category(e, previous(e, at)) == 0
                                               : category(e, previous(e, at)) != 1)) at = previous(e, at);
        while (at && (action == EDIT_WORD_ERASE ? category(e, previous(e, at)) != 0
                                               : category(e, previous(e, at)) == 1)) at = previous(e, at);
        if (action == EDIT_WORD_ERASE) erase(e, at, e->cursor, 1);
        else e->cursor = at;
        break;
    case EDIT_WORD_RIGHT: case EDIT_WORD_DELETE:
        while (at < size && category(e, at) != 1) at = next(e, at);
        while (at < size && category(e, at) == 1) at = next(e, at);
        if (action == EDIT_WORD_DELETE) erase(e, e->cursor, at, 1);
        else e->cursor = at;
        break;
    case EDIT_BACKSPACE: erase(e, previous(e, at), at, 0); break;
    case EDIT_DELETE: erase(e, at, next(e, at), 0); break;
    case EDIT_HOME:
        while (at && e->text[at - 1] != '\n') --at;
        e->cursor = at; break;
    case EDIT_END:
        while (at < size && e->text[at] != '\n') ++at;
        e->cursor = at; break;
    case EDIT_KILL_BEFORE: erase(e, 0, at, 1); break;
    case EDIT_KILL_AFTER:
        end = at;
        while (end < size && e->text[end] != '\n') ++end;
        if (end == at && end < size) ++end;
        erase(e, at, end, 1); break;
    case EDIT_YANK: chat_editor_insert(e, e->killed, strlen(e->killed)); break;
    }
}
