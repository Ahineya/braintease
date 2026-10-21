/* SPDX-License-Identifier: MIT */
/* Syma symbols are delimited by whitespace, comma, parentheses and braces.
 * Preserve block-comment and ordinary/raw string state across physical lines. */
static int syma_delimiter(int c) {
    return c < 0 || c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
           c == ',' || c == '(' || c == ')' || c == '{' || c == '}' || c == ';' || c == '"';
}
static int syma_numeric(TextBuffer *b, unsigned long at, unsigned long end) {
    unsigned digits = 0; int c = text_byte(b, at);
    if (c == '+' || c == '-') ++at;
    while (at < end && (c = text_byte(b, at)) >= '0' && c <= '9') { ++at; ++digits; }
    if (at < end && text_byte(b, at) == '.') {
        ++at;
        while (at < end && (c = text_byte(b, at)) >= '0' && c <= '9') { ++at; ++digits; }
    }
    if (!digits) return 0;
    if (at < end && (text_byte(b, at) == 'e' || text_byte(b, at) == 'E')) {
        ++at; digits = 0;
        if (at < end && (text_byte(b, at) == '+' || text_byte(b, at) == '-')) ++at;
        while (at < end && (c = text_byte(b, at)) >= '0' && c <= '9') { ++at; ++digits; }
        if (!digits) return 0;
    }
    return at == end;
}
static unsigned syma_line(TextBuffer *b, unsigned long row, unsigned state, struct Paint *out) {
    unsigned long p = text_start(b, row), end = text_endline(b, row), q;
    unsigned kind; int c;
    while (p < end) {
        c = text_byte(b, p);
        if (state == 1) {
            if (starts(b, p, end, "*/")) { paint(b,out,p++,1); paint(b,out,p++,1); state=0; }
            else paint(b,out,p++,1);
        } else if (state == 2 || state == 3) {
            paint(b,out,p++,3);
            if (c == '"') state=0;
            else if (state == 2 && c == '\\' && p < end) paint(b,out,p++,3);
        } else if (c == ';' || starts(b,p,end,"//")) {
            while (p < end) paint(b,out,p++,1);
        } else if (starts(b,p,end,"/*")) {
            paint(b,out,p++,1);paint(b,out,p++,1);state=1;
        } else if (c == '"' || (c == 'r' && p+1 < end && text_byte(b,p+1) == '"')) {
            state = c == 'r' ? 3 : 2;
            if (c == 'r') paint(b,out,p++,3);
            paint(b,out,p++,3);
        } else if (syma_delimiter(c)) {
            paint(b,out,p++, c == ' ' || c == '\t' ? 0 : 5);
        } else {
            q=p+1;
            while (q<end && !syma_delimiter(text_byte(b,q)) && !starts(b,q,end,"//") && !starts(b,q,end,"/*")) ++q;
            kind=keyword(b,p,q);
            if (c == ':') kind=2;
            else if (!kind && syma_numeric(b,p,q)) kind=4;
            else if (!kind && (text_byte(b,q-1)=='_' ||
                (q-p>=2 && text_byte(b,q-1)=='.' && text_byte(b,q-2)=='.'))) kind=6;
            else if (!kind && c >= 'A' && c <= 'Z') kind=6;
            else if (!kind && (c=='/' || c=='+' || c=='-' || c=='*' || c=='=' || c=='<' || c=='>')) kind=5;
            while (p<q)paint(b,out,p++,kind);
        }
    }
    return state;
}
