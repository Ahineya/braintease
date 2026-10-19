/* SPDX-License-Identifier: MIT */
/* Compact line state: low nibble is comment/template/continued quote;
 * bit 4 means the preceding significant token can end an expression.
 * Templates are painted as strings, including ${...}; regexp recognition is
 * lexical (a parser is needed for every ambiguous slash in JavaScript). */
static int js_identifier(int c) { return identifier(c) || c == '$' || c >= 128; }
static unsigned javascript_line(TextBuffer *b, unsigned long row, unsigned saved, struct Paint *out) {
    static const char *const expressions[] = {
        "await", "case", "delete", "do", "else", "in", "instanceof", "new", "of", "return", "throw", "typeof", "void", "yield"
    };
    unsigned state = saved & 15, ended = saved & 16, kind;
    unsigned long p = text_start(b, row), end = text_endline(b, row), q;
    int c, quote, escaped, bracket;
    while (p < end) {
        c = text_byte(b, p);
        if (state == 1) {
            if (starts(b, p, end, "*/")) { paint(b, out, p++, 1); paint(b, out, p++, 1); state = 0; }
            else paint(b, out, p++, 1);
            continue;
        }
        if (state == 7 || state == 8 || state == 9) {
            quote = state == 7 ? '`' : state == 8 ? '\'' : '"';
            escaped = 0;
            while (p < end) {
                c = text_byte(b, p); paint(b, out, p++, 3);
                if (!escaped && c == quote) { state = 0; ended = 16; break; }
                if (!escaped && c == '\\') escaped = 1; else escaped = 0;
            }
            if (state != 7 && !escaped) state = 0;
            continue;
        }
        if ((!row && p == 0 && starts(b, p, end, "#!")) || starts(b, p, end, "//")) {
            while (p < end) paint(b, out, p++, 1);
        } else if (starts(b, p, end, "/*")) {
            paint(b, out, p++, 1); paint(b, out, p++, 1); state = 1;
        } else if (c == '\'' || c == '"' || c == '`') {
            state = c == '`' ? 7 : c == '\'' ? 8 : 9;
            paint(b, out, p++, 3); ended = 16;
        } else if (c == '/' && !ended) {
            paint(b, out, p++, 3); escaped = bracket = 0;
            while (p < end) {
                c = text_byte(b, p); paint(b, out, p++, 3);
                if (!escaped) {
                    if (c == '/' && !bracket) break;
                    if (c == '[') bracket = 1;
                    if (c == ']') bracket = 0;
                }
                if (!escaped && c == '\\') escaped = 1; else escaped = 0;
            }
            while (p < end && js_identifier(text_byte(b, p))) paint(b, out, p++, 3);
            ended = 16;
        } else if ((c >= '0' && c <= '9') || (c == '.' && p + 1 < end && text_byte(b, p + 1) >= '0' && text_byte(b, p + 1) <= '9')) {
            do {
                c = text_byte(b, p); paint(b, out, p++, 4);
                if ((c == 'e' || c == 'E') && p < end && (text_byte(b, p) == '+' || text_byte(b, p) == '-')) paint(b, out, p++, 4);
            } while (p < end && (identifier(text_byte(b, p)) || text_byte(b, p) == '.'));
            ended = 16;
        } else if (js_identifier(c)) {
            q = p + 1; while (q < end && js_identifier(text_byte(b, q))) ++q;
            kind = keyword(b, p, q);
            ended = word_in(b, p, q, expressions, sizeof(expressions) / sizeof(expressions[0]), 0) ? 0 : 16;
            if (!kind && c >= 'A' && c <= 'Z') kind = 6;
            while (p < q) paint(b, out, p++, kind);
        } else {
            if (c != ' ' && c != '\t') {
                /* Postfix ++/-- retain expression context, prefix forms retain
                 * operand context. Division itself starts the next operand. */
                if ((c == '+' || c == '-') && p + 1 < end && text_byte(b, p + 1) == c) {
                    paint(b, out, p++, 5); paint(b, out, p++, 5); continue;
                }
                ended = c == ')' || c == ']' || c == '}' ? 16 : 0;
            }
            paint(b, out, p++, c == ' ' || c == '\t' ? 0 : 5);
        }
    }
    return state | ended;
}
