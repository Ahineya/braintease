/* No stdio.h: this internal name does not declare the libc putchar function.
 * A definition inherits the linkage of its preceding static declaration. */
extern int puts(char *);
static int putchar(int value);
int putchar(int value) { return value + 1; }
int main(void) {
    /* libc puts must still use libc putchar for its terminating newline. */
    if (putchar(41) == 42) puts("Y"); else puts("N");
    return 0;
}
