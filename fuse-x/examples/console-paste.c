/* Regression: a full raw console queue must backpressure a pasted burst. */
#include <stdio.h>
#include <termios.h>
#include <unistd.h>
int main(void) {
    struct termios saved, raw;
    unsigned i;
    char c;
    int good = 1;
    if (tcgetattr(0, &saved)) return 1;
    raw = saved;
    raw.c_lflag &= ~(ICANON | ECHO | ISIG);
    raw.c_iflag = 0;
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(0, TCSANOW, &raw)) return 1;
    puts("PASTE_READY"); fflush(stdout);
    for (i = 0; i < 1500; ++i) {
        if (read(0, &c, 1) != 1 || c != 'a' + i % 26) { good = 0; break; }
        /* Screen applications make several syscalls between input reads. */
        usleep(1000);
        if (write(1, ".", 1) != 1) { good = 0; break; }
    }
    tcsetattr(0, TCSANOW, &saved);
    printf("\nPASTE_%s %u\n", good ? "OK" : "FAILED", i);
    return !good;
}
