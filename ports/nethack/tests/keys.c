#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include "../config/keys.c"

/* RVM's host terminal parser normalizes cursor keys before guest delivery.
 * Feed malformed/modified sequences through a guest pipe to exercise their
 * exact bytes on both targets, independently of that host parser. */
static int literal_sequences(void)
{
    FILE *saved = stdin, *stream;
    int descriptors[2], ok = 1;
    if (pipe(descriptors) < 0) return 0;
    stream = fdopen(descriptors[0], "r");
    if (!stream) { close(descriptors[0]); close(descriptors[1]); return 0; }
    stdin = stream;
    write(descriptors[1], "\033z", 2);
    ok &= nh_terminal_getchar("hykulnjb") == 27;
    ok &= nh_terminal_getchar("hykulnjb") == 'z';
    write(descriptors[1], "\033[", 2);
    ok &= nh_terminal_getchar("hykulnjb") == 27;
    ok &= nh_terminal_getchar("hykulnjb") == '[';
    write(descriptors[1], "\033[1;5A.", 7);
    ok &= nh_terminal_getchar("hykulnjb") == 27;
    ok &= nh_terminal_getchar("hykulnjb") == '.';
    close(descriptors[1]); fclose(stream); stdin = saved;
    return ok;
}

int main(int argc, char **argv)
{
    static const char *alphabets[] = { "hykulnjb", "47896321", "41236987", NULL };
    struct termios saved, raw;
    int mode = argc > 1 ? atoi(argv[1]) : 0, c, index = 0;
    if (!literal_sequences()) { puts("Literal sequence FAIL"); return 1; }
    if (mode < 0 || mode > 3 || tcgetattr(0, &saved) < 0) return 1;
    raw = saved;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(0, TCSANOW, &raw) < 0) return 1;
    puts("KEYS READY"); fflush(stdout);
    while ((c = nh_terminal_getchar(alphabets[mode])) != '!') {
        printf("KEY %d %d\n", index++, c); fflush(stdout);
    }
    tcsetattr(0, TCSANOW, &saved);
    puts("Terminal keys PASS");
    return 0;
}
