/* SPDX-License-Identifier: MIT */
static int identifier(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }
static int starts(TextBuffer *b, unsigned long at, unsigned long end, const char *s) {
    while (*s) { if (at >= end || text_byte(b, at++) != *s++) return 0; } return 1;
}
/* All built-in keywords fit in 31 octets. Read the buffer once per token,
 * then search sorted tables instead of traversing every keyword per character. */
static int word_in(TextBuffer *b, unsigned long at, unsigned long end,
                   const char *const *words, unsigned count, int folded) {
    char token[32];
    unsigned n, i, lo = 0, hi = count, mid;
    int c, difference;
    if (end - at >= sizeof(token)) return 0;
    n = (unsigned)(end - at);
    for (i = 0; i < n; ++i) {
        c = text_byte(b, at + i);
        token[i] = folded && c >= 'A' && c <= 'Z' ? c + 32 : c;
    }
    while (lo < hi) {
        const char *word;
        mid = lo + (hi - lo) / 2;
        word = words[mid]; i = 0;
        while (i < n && word[i] && token[i] == word[i]) ++i;
        difference = i == n ? word[i] ? -1 : 0 : (unsigned char)token[i] - (unsigned char)word[i];
        if (!difference) return 1;
        if (difference < 0) hi = mid; else lo = mid + 1;
    }
    return 0;
}
static unsigned keyword(TextBuffer *b, unsigned long at, unsigned long end) {
    static const char *const core[] = {
        "Err", "None", "Ok", "Option", "Result", "Self", "Some", "String", "Vec", "async", "auto", "await",
        "bool", "break", "case", "catch", "char", "class", "const", "continue", "default", "delete", "do",
        "double", "else", "enum", "export", "extends", "extern", "f32", "f64", "false", "finally", "float",
        "fn", "for", "from", "goto", "i16", "i32", "i64", "i8", "if", "impl", "implements", "import", "int",
        "interface", "let", "long", "loop", "match", "move", "mut", "namespace", "new", "null", "nullptr",
        "owned", "package", "private", "protected", "public", "register", "restrict", "return", "self",
        "short", "signed", "sizeof", "static", "struct", "switch", "this", "throw", "throws", "trait",
        "true", "try", "type", "typedef", "typeof", "u16", "u32", "u64", "u8", "union", "unit", "unsafe",
        "unsigned", "using", "usize", "void", "volatile", "while"
    };
    static const char *const pascal[] = {
        "and", "array", "begin", "boolean", "byte", "case", "char", "const", "div", "do", "downto", "else",
        "end", "external", "false", "file", "for", "forward", "function", "if", "implementation", "integer",
        "interface", "longint", "mod", "nil", "not", "of", "or", "packed", "procedure", "program", "real",
        "record", "repeat", "set", "string", "then", "to", "true", "type", "unit", "until", "uses", "var",
        "while", "with", "word", "xor"
    };
    static const char *const script[] = {
        "BEGIN", "END", "False", "None", "True", "and", "as", "break", "case", "catch", "class", "close",
        "continue", "def", "do", "done", "elif", "else", "esac", "eval", "except", "export", "expr", "fi",
        "finally", "for", "foreach", "from", "function", "global", "if", "import", "in", "is", "lambda",
        "local", "next", "not", "open", "or", "pass", "print", "printf", "proc", "puts", "raise", "read",
        "return", "set", "source", "switch", "then", "try", "unset", "until", "while", "with", "write",
        "yield"
    };
    static const char *const lisp[] = {
        "and", "apply", "begin", "car", "cdr", "cond", "cons", "define", "defmacro", "defparameter", "defun",
        "defvar", "do", "else", "eval", "if", "lambda", "let", "let*", "letrec", "list", "loop", "map",
        "nil", "not", "or", "progn", "quasiquote", "quote", "set!", "t", "unless", "unquote", "when"
    };
    static const char *const json[] = {"false", "null", "true"};
    if (b->language == 4) return word_in(b, at, end, pascal, sizeof(pascal) / sizeof(pascal[0]), 1) ? 2 : 0;
    if (b->language == 2 || b->language == 3 || b->language == 9) return word_in(b, at, end, script, sizeof(script) / sizeof(script[0]), 0) ? 2 : 0;
    if (b->language == 11) return word_in(b, at, end, lisp, sizeof(lisp) / sizeof(lisp[0]), 0) ? 2 : 0;
    if (b->language == 5) return word_in(b, at, end, json, sizeof(json) / sizeof(json[0]), 0) ? 2 : 0;
    return word_in(b, at, end, core, sizeof(core) / sizeof(core[0]), 0) ? 2 : 0;
}
struct Paint { unsigned long left, column, select_start, select_end, selection; unsigned width, tabstop; const unsigned long *styles; unsigned long *cells; };
static void paint(TextBuffer *b, struct Paint *out, unsigned long at, unsigned kind) {
    int ch; unsigned long width, i, style;
    if (!out) return;
    ch = text_byte(b, at);
    width = ch == '\t' ? out->tabstop - out->column % out->tabstop : 1;
    style = at >= out->select_start && at < out->select_end ? out->selection : out->styles[kind];
    for (i = 0; i < width; ++i) {
        if (out->column >= out->left && out->column - out->left < out->width)
            out->cells[out->column - out->left] = style | (unsigned long)(ch == '\t' ? ' ' : ch >= 32 && ch <= 126 ? ch : '?');
        ++out->column;
    }
}
static unsigned lex_line(TextBuffer *b, unsigned long row, unsigned state, struct Paint *out) {
    unsigned long p = text_start(b, row), end = text_endline(b, row), q;
    unsigned lang = b->language, kind; int c, quote, escaped;
    if (!lang) { while (p < end) paint(b, out, p++, 0); return 0; }
    if (lang == 8) {
        while (p < end) { c = text_byte(b, p); kind = c == '[' || c == ']' ? 2 : c == '<' || c == '>' ? 5 : c == '+' || c == '-' ? 4 : c == '.' || c == ',' ? 3 : 1; paint(b, out, p++, kind); }
        return 0;
    }
    while (p < end) {
        c = text_byte(b, p);
        if (state) {
            const char *close = state == 1 ? "*/" : state == 2 ? "*)" : state == 3 ? "}" : state == 4 ? "-->" : state == 5 ? "'''" : state == 6 ? "\"\"\"" : "`";
            kind = state >= 5 ? 3 : 1;
            if (starts(b, p, end, close)) { for (q = 0; close[q]; ++q) paint(b, out, p++, kind); state = 0; }
            else { paint(b, out, p++, kind); if (state == 7 && c == '\\' && p < end) paint(b, out, p++, kind); }
            continue;
        }
        if (((lang == 1 || lang == 10) && starts(b, p, end, "/*")) ||
            (lang == 4 && starts(b, p, end, "(*")) || (lang == 4 && c == '{') ||
            (lang == 10 && starts(b, p, end, "<!--"))) {
            state = lang == 4 ? c == '{' ? 3 : 2 : lang == 10 && c == '<' ? 4 : 1;
            q = state == 3 ? 1 : state == 4 ? 4 : 2;
            while (q--) paint(b, out, p++, 1);
        } else if (((lang == 1 || lang == 10 || lang == 4) && starts(b, p, end, "//")) ||
                   ((lang == 2 || lang == 3 || lang == 6 || lang == 9 || lang == 12) && c == '#') ||
                   ((lang == 6 || lang == 11 || lang == 12) && c == ';')) {
            while (p < end) paint(b, out, p++, 1);
        } else if (lang == 3 && (starts(b, p, end, "'''") || starts(b, p, end, "\"\"\""))) {
            state = c == '\'' ? 5 : 6; for (q = 0; q < 3; ++q) paint(b, out, p++, 3);
        } else if (c == '\'' || c == '"' || ((lang == 1 || lang == 7 || lang == 11) && c == '`')) {
            if (c == '`' && lang == 1) { state = 7; paint(b, out, p++, 3); continue; }
            quote = c; escaped = 0; paint(b, out, p++, 3);
            while (p < end) { c = text_byte(b, p); paint(b, out, p++, 3); if (!escaped && c == quote) break; if (!escaped && c == '\\') escaped = 1; else escaped = 0; }
        } else if ((lang == 7 && c == '#') || (lang == 6 && c == '[')) {
            while (p < end) paint(b, out, p++, 2);
        } else if (c >= '0' && c <= '9') {
            do { paint(b, out, p++, 4); c = text_byte(b, p); } while (p < end && (identifier(c) || c == '.'));
        } else if (identifier(c)) {
            q = p + 1; while (q < end && identifier(text_byte(b, q))) ++q;
            kind = keyword(b, p, q);
            if (!kind && c >= 'A' && c <= 'Z') kind = 6;
            while (p < q) paint(b, out, p++, kind);
        } else { paint(b, out, p++, c != ' ' && c != '\t' ? 5 : 0); }
    }
    return state;
}
void text_language(TextBuffer *b, unsigned language) { if (b->language != language) { b->language = language; b->valid_states = 1; b->lines[0].state = 0; ++b->generation; } }
int text_render(TextBuffer *b, unsigned long row, unsigned long left, unsigned width,
                unsigned tabstop, const unsigned long *styles, unsigned long selection,
                unsigned long select_start, unsigned long select_end, unsigned long *cells) {
    unsigned i; struct Paint out;
    if (width > 512 || !styles || !cells || tabstop < 1 || tabstop > 16) return error(b, 2);
    for (i = 0; i < width; ++i) cells[i] = styles[0] | ' ';
    if (row >= b->nlines) return 0;
    while (b->valid_states <= row) {
        unsigned long r = b->valid_states - 1;
        b->lines[b->valid_states].state = lex_line(b, r, b->lines[r].state, NULL); ++b->valid_states;
    }
    out.left = left; out.column = 0; out.width = width; out.tabstop = tabstop; out.styles = styles; out.cells = cells;
    out.selection = selection; out.select_start = select_start; out.select_end = select_end;
    lex_line(b, row, b->lines[row].state, &out);
    /* Show a selected newline as one cell after the last visible character. */
    if (text_endline(b, row) >= select_start && text_endline(b, row) < select_end && out.column >= left && out.column - left < width)
        cells[out.column - left] = selection | ' ';
    return 0;
}
