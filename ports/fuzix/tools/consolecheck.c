/* Console transport acceptance and throughput fixture, shared by both kernels. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
static char buffer[1024];
int main(int argc, char **argv) {
    struct termios saved, raw;
    unsigned i;
    unsigned long k;
    char *allocation, *crossing;
    if (tcgetattr(1, &saved)) return 1;
    raw = saved; raw.c_oflag = 0;
    if (tcsetattr(1, TCSANOW, &raw)) return 2;
    puts("CONSOLE_BEGIN"); fflush(stdout);
    if (argc > 1 && !strcmp(argv[1], "--bench")) {
        memset(buffer, 'x', sizeof(buffer));
        for (k = 0; k < 128; ++k)
            if (write(1, buffer, sizeof(buffer)) != sizeof(buffer)) return 3;
    } else {
        allocation = malloc(65000UL);
        if (!allocation) return 4;
        crossing = allocation + 63900UL;
        for (i = 0; i < sizeof(buffer); ++i) crossing[i] = i & 255;
        /* A bank-spanning user source, embedded NULs and all octet values. */
        if (write(1, crossing, sizeof(buffer)) != sizeof(buffer)) return 5;
        free(allocation);
        /* Scalar stdio, a buffered write and stderr must retain their order. */
        fputs("stdio", stdout); fflush(stdout);
        if (write(1, "/write", 6) != 6) return 6;
        fputs("/stderr", stderr);
        if (tcsetattr(1, TCSANOW, &saved)) return 7;
        if (write(1, "\nPOST\n", 6) != 6) return 8;
        if (tcsetattr(1, TCSANOW, &raw)) return 9;
    }
    puts("\nCONSOLE_END"); fflush(stdout);
    if (tcsetattr(1, TCSANOW, &saved)) return 10;
    return 0;
}
