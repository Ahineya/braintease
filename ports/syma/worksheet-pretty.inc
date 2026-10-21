/* Display only. Exact cached normal forms and saved documents are never rewritten. */
static unsigned math_head(Id x, const char *name) {
    char full[80];
    snprintf(full, sizeof(full), "Math/Small/%s", name);
    return syma_headis(x, full);
}
static unsigned precedence(Id x) {
    return math_head(x, "Sum") && syma_argcnt(x) == 2       ? 1
           : math_head(x, "Product") && syma_argcnt(x) == 2 ? 2
           : math_head(x, "Power") && syma_argcnt(x) == 2   ? 3
                                                            : 4;
}
static void pretty_term(Buffer *b, Id x, unsigned parent, unsigned depth) {
    Count i, n = syma_argcnt(x);
    unsigned prec = precedence(x);
    if (depth > 128)
        syma_fail("display nesting limit exceeded");
    if (prec < 4) {
        const char *op = prec == 1 ? " + " : prec == 2 ? " * " : "^";
        if (prec < parent)
            syma_bufput(b, '(');
        pretty_term(b, syma_arg(x, 0), prec == 3 ? 4 : prec, depth + 1);
        syma_buftext(b, op);
        pretty_term(b, syma_arg(x, 1), prec == 3 ? 3 : prec, depth + 1);
        if (prec < parent)
            syma_bufput(b, ')');
    } else if (n == 1 && (math_head(x, "Sin") || math_head(x, "Cos"))) {
        syma_buftext(b, math_head(x, "Sin") ? "sin(" : "cos(");
        pretty_term(b, syma_arg(x, 0), 0, depth + 1);
        syma_bufput(b, ')');
    } else if (syma_headis(x, "Imported")) {
        syma_buftext(b, "Imported ");
        pretty_term(b, syma_arg(x, 0), 0, depth + 1);
    } else if (syma_headis(x, "Output")) {
        syma_bufput(b, '[');
        for (i = 0; i < n; i++) {
            if (i)
                syma_buftext(b, ", ");
            pretty_term(b, syma_arg(x, i), 0, depth + 1);
        }
        syma_bufput(b, ']');
    } else if (syma_node(x)->kind == CALL) {
        syma_bufput(b, '{');
        if (syma_node(x)->h)
            pretty_term(b, syma_node(x)->h, 0, depth + 1);
        for (i = 0; i < n; i++) {
            syma_bufput(b, ' ');
            pretty_term(b, syma_arg(x, i), 4, depth + 1);
        }
        syma_bufput(b, '}');
    } else {
        char *s = syma_show(x, 0);
        unsigned negative = parent >= 3 && syma_node(x)->kind == NUM && syma_node(x)->num < 0;
        if (negative)
            syma_bufput(b, '(');
        syma_buftext(b, s);
        if (negative)
            syma_bufput(b, ')');
        free(s);
    }
}
/* A separate boundary handles invalid old cache/error text without changing a context. */
static Buffer pretty_buffer;
static char *display_result(const char *raw) {
    jmp_buf local;
    jmp_buf *previous = syma_error_target;
    memset(&pretty_buffer, 0, sizeof(pretty_buffer));
    syma_error_target = &local;
    if (setjmp(local)) {
        free(pretty_buffer.p);
        syma_error_target = previous;
        return duplicate(raw);
    }
    pretty_term(&pretty_buffer, syma_parse(raw), 0, 0);
    syma_error_target = previous;
    return pretty_buffer.p ? pretty_buffer.p : duplicate(raw);
}
