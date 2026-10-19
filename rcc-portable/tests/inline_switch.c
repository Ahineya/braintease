/* A default arm precedes its dispatch blocks in cproc's block layout.
 * Inlining must keep fixed stack homes available in that layout too. */
static int digit(int c) {
    if ('0' <= c && c <= '7') return c - '0';
    return -1;
}
int parse(const char *s) {
    int value;
    switch (*s) {
    case 'x': return 42;
    case 'a': return 10;
    default:
        value = digit(*s);
        if (value < 0) return -1;
        return value + 1;
    }
}
int main(void) {
    return parse("x")!=42 || parse("a")!=10 || parse("7")!=8 || parse("9")!=-1;
}
