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
    static const char *const javascript[] = {
        "as", "async", "await", "break", "case", "catch", "class", "const", "continue", "debugger",
        "default", "delete", "do", "else", "export", "extends", "false", "finally", "for", "from",
        "function", "get", "if", "import", "in", "instanceof", "let", "new", "null", "of",
        "return", "set", "static", "super", "switch", "this", "throw", "true", "try", "typeof",
        "undefined", "var", "void", "while", "with", "yield"
    };
    static const char *const json[] = {"false", "null", "true"};
    static const char *const basic[] = {
        "abs", "and", "append", "as", "call", "case", "close", "cls", "color", "declare",
        "else", "elseif", "end", "fix", "for", "goto", "graphicsavailable", "if", "input",
        "int", "lcase", "left", "len", "let", "locate", "mid", "mod", "next", "not", "open",
        "or", "output", "print", "randomize", "rem", "right", "rnd", "select", "sgn", "sleep",
        "step", "stop", "str", "sub", "tab", "then", "timer", "to", "ucase", "wend", "while", "write", "xor"
    };
    if (b->language == 14) return word_in(b, at, end, javascript, sizeof(javascript) / sizeof(javascript[0]), 0) ? 2 : 0;
    if (b->language == 13) return word_in(b, at, end, basic, sizeof(basic) / sizeof(basic[0]), 1) ? 2 : 0;
    if (b->language == 4) return word_in(b, at, end, pascal, sizeof(pascal) / sizeof(pascal[0]), 1) ? 2 : 0;
    if (b->language == 2 || b->language == 3 || b->language == 9) return word_in(b, at, end, script, sizeof(script) / sizeof(script[0]), 0) ? 2 : 0;
    if (b->language == 11) return word_in(b, at, end, lisp, sizeof(lisp) / sizeof(lisp[0]), 0) ? 2 : 0;
    if (b->language == 5) return word_in(b, at, end, json, sizeof(json) / sizeof(json[0]), 0) ? 2 : 0;
    return word_in(b, at, end, core, sizeof(core) / sizeof(core[0]), 0) ? 2 : 0;
}
struct Paint { unsigned long left, column, next, end, select_start, select_end, selection; unsigned width, tabstop; const unsigned long *styles; unsigned long *cells; };
static void paint(TextBuffer *b, struct Paint *out, unsigned long at, unsigned kind) {
    int ch, complete; unsigned width; unsigned long i, style; uint32_t glyph;
    if (!out) return;
    if (at < out->next) return;
    ch = text_byte(b, at);
    out->next = cluster(b, at, out->end, &width, &glyph);
    if (ch == '\t') width = out->tabstop - out->column % out->tabstop;
    style = at < out->select_end && out->next > out->select_start ? out->selection : out->styles[kind];
    complete = out->column >= out->left && out->column - out->left < out->width && width <= out->width - (out->column - out->left);
    for (i = 0; i < width; ++i) {
        if (out->column >= out->left && out->column - out->left < out->width)
            out->cells[out->column - out->left] = style | (unsigned long)(ch == '\t' || !complete ? ' ' : !i ? glyph : ru_glyph_width(glyph) == 2 ? RU_CONTINUATION : ' ');
        ++out->column;
    }
}
#include "javascript.inc"
static unsigned lex_line(TextBuffer *b, unsigned long row, unsigned state, struct Paint *out) {
    unsigned long p = text_start(b, row), end = text_endline(b, row), q;
    unsigned lang = b->language, kind; int c, quote, escaped;
    if (lang == 14) return javascript_line(b, row, state, out);
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
        if (lang == 13 && (c == '\'' || (p+3<=end &&
            (c=='R'||c=='r') && (text_byte(b,p+1)=='E'||text_byte(b,p+1)=='e') &&
            (text_byte(b,p+2)=='M'||text_byte(b,p+2)=='m') &&
            (p+3==end || (!identifier(text_byte(b,p+3)) && text_byte(b,p+3)!='$'))))) {
            while (p < end) paint(b, out, p++, 1);
        } else if (lang == 13 && c == '"') {
            paint(b, out, p++, 3);
            while (p < end) {
                c=text_byte(b,p); paint(b,out,p++,3);
                if (c=='"') {
                    if (p<end && text_byte(b,p)=='"') paint(b,out,p++,3);
                    else break;
                }
            }
        } else if (((lang == 1 || lang == 10) && starts(b, p, end, "/*")) ||
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
            if (lang==13 && q<end && text_byte(b,q)=='$') ++q;
            if (!kind && c >= 'A' && c <= 'Z' && lang!=13) kind = 6;
            while (p < q) paint(b, out, p++, kind);
        } else { paint(b, out, p++, c != ' ' && c != '\t' ? 5 : 0); }
    }
    return state;
}
void text_language(TextBuffer *b, unsigned language) {
    if (b->language != language) {
        b->language = language; b->valid_states = b->cached_states = 1;
        b->lines[0].state = 0; b->dirty_first = 0; b->dirty_until = b->nlines;
        ++b->generation;
    }
}
void text_damage(TextBuffer *b, unsigned long *bounds) {
    unsigned long until = b->dirty_until;
    /* Only revisit previously cached syntax. Unseen suffixes remain lazy.
     * Past the final edit, equality with the old incoming state proves that
     * the unchanged suffix has converged. */
    while (until && b->valid_states < b->cached_states) {
        unsigned long r = b->valid_states - 1;
        unsigned state = lex_line(b, r, b->lines[r].state, NULL);
        unsigned prior = b->lines[r + 1].state;
        b->lines[r + 1].state = state; ++b->valid_states;
        if (state != prior && until < r + 2) until = r + 2;
        if (r + 1 >= b->dirty_until && state == prior) {
            b->valid_states = b->cached_states; break;
        }
    }
    bounds[0] = until ? b->dirty_first : 0;
    bounds[1] = until ? b->dirty_layout ? 0xffffffffUL : until : 0;
    b->dirty_until = 0; b->dirty_layout = 0;
}
int text_overflow(TextBuffer *b) { return b->overflow; }
int text_render(TextBuffer *b, unsigned long row, unsigned long left, unsigned width,
                unsigned tabstop, const unsigned long *styles, unsigned long selection,
                unsigned long select_start, unsigned long select_end, unsigned long *cells) {
    unsigned i, outgoing; struct Paint out;
    if (width > 512 || !styles || !cells || tabstop < 1 || tabstop > 16) return error(b, 2);
    for (i = 0; i < width; ++i) cells[i] = styles[0] | ' ';
    b->overflow = 0;
    if (row >= b->nlines) return 0;
    while (b->valid_states <= row) {
        unsigned long r = b->valid_states - 1;
        b->lines[b->valid_states].state = lex_line(b, r, b->lines[r].state, NULL); ++b->valid_states;
    }
    out.left = left; out.column = out.next = 0; out.end = text_endline(b, row); out.width = width; out.tabstop = tabstop; out.styles = styles; out.cells = cells;
    out.selection = selection; out.select_start = select_start; out.select_end = select_end;
    if (!b->language) {
        unsigned long p = text_start(b, row), lo = 0, hi = b->wrap_count;
        if (b->wrap_rows > row) {
            while (lo < hi) { unsigned long mid = lo + (hi - lo) / 2;
                if (b->wraps[mid].row < row || (b->wraps[mid].row == row && b->wraps[mid].left <= left)) lo = mid + 1; else hi = mid;
            }
            if (lo && b->wraps[lo - 1].row == row) { p = b->wraps[lo - 1].at; out.column = b->wraps[lo - 1].left; }
        }
        while (p < out.end && out.column <= left + width) paint(b, &out, p++, 0);
        /* A clipped suffix must not be mistaken for a visible final newline. */
        if (p < out.end) out.column = left + width + 1;
        outgoing = 0;
    } else outgoing = lex_line(b, row, b->lines[row].state, &out);
    if (row + 1 < b->nlines && b->valid_states == row + 1) {
        b->lines[row + 1].state = outgoing; ++b->valid_states;
    }
    if (b->cached_states < b->valid_states) b->cached_states = b->valid_states;
    b->overflow = out.column > left && out.column - left > width;
    /* Show a selected newline as one cell after the last visible character. */
    if (text_endline(b, row) >= select_start && text_endline(b, row) < select_end && out.column >= left && out.column - left < width)
        cells[out.column - left] = selection | ' ';
    return 0;
}
