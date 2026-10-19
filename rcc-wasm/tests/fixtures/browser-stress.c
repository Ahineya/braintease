extern void *malloc(unsigned long);
extern void free(void *);
extern int putchar(int);
static void *owner;
static unsigned phase;
static unsigned pool(const char *s, unsigned n) {
    return ripple_dom_string((const unsigned short *)s, n);
}
int main(void) {
    unsigned i, text;
    owner = malloc(128);
    ripple_browser_mount(); ripple_dom_begin();
    ripple_dom_command(1, 1, pool("div",3), 0, 0);
    ripple_dom_command(1, 2, pool("button",6), 0, 0);
    ripple_dom_command(2, 3, pool("remove",6), 0, 0);
    ripple_dom_command(1, 4, pool("input",5), 0, 0);
    ripple_dom_command(6, 0, 1, 0, 0);
    ripple_dom_command(6, 1, 2, 0, 0);
    ripple_dom_command(6, 2, 3, 0, 0);
    ripple_dom_command(6, 1, 4, 0, 0);
    ripple_dom_command(8, 2, 1, 1, 3);
    ripple_dom_command(8, 4, 2, 2, 0);
    ripple_dom_command(8, 2, 4, 3, 0);
    ripple_dom_command(4, 2, pool("title",5), pool("ready",5), 0);
    ripple_dom_command(5, 4, 1, pool("unchanged",9), 0);
    text = pool("*",1);
    for (i = 5; i <= 504; ++i) {
        ripple_dom_command(2, i, text, 0, 0);
        ripple_dom_command(6, 1, i, 0, 0);
    }
    ripple_dom_commit();
    for (i = 0; i < 8191; ++i) putchar('a');
    putchar(240); putchar(159); putchar(140); putchar(141); putchar(10);
    return 0;
}
void ripple_browser_on_events(const unsigned short *data, unsigned long cells) {
    unsigned long p = 4;
    while (p < cells) {
        if (data[p+1] == 1 && !phase) {
            ripple_dom_begin();
            ripple_dom_command(9, 4, 2, 0, 0);
            ripple_dom_command(7, 4, 0, 0, 0);
            ripple_dom_command(8, 2, 1, 4, 0);
            ripple_dom_command(3, 3, pool("changed",7), 0, 0);
            ripple_dom_command(6, 1, 504, 5, 0);
            ripple_dom_command(6, 1, 5, 504, 0);
            ripple_dom_command(10, 2, pool("title",5), 0, 0);
            ripple_dom_commit(); phase = 1; putchar('!');
        } else if (data[p+1] == 4 && phase == 1) {
            ripple_dom_begin(); ripple_dom_command(7, 1, 0, 0, 0);
            ripple_dom_commit(); phase = 2; putchar('?');
        }
        p += data[p];
    }
}
void ripple_browser_on_dispose(void) { free(owner); owner = 0; putchar('D'); }
