/* Exercise the real filesystem with full 16-bit cells, short/unaligned writes,
 * indirect blocks, EOF and persisted content after a fresh kernel process. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
static unsigned char data[4096];
static void check(int yes) {
    if (!yes) { puts("DISK CHECK FAILED:"); exit(1); }
}
static unsigned value(unsigned long position) {
    return (unsigned)((position * 109UL + 0x9234UL) & 65535UL);
}
int main(int argc, char **argv) {
    int verify = argc == 2 && !strcmp(argv[1], "--verify");
    int fd = open("/root/wide", verify ? O_RDONLY : O_CREAT | O_TRUNC | O_RDWR, 0600);
    check(fd >= 0);
    if (!verify) {
        for (unsigned round = 0; round < 5; ++round) {
            for (unsigned i = 0; i < sizeof(data); ++i) data[i] = value((unsigned long)round * sizeof(data) + i);
            check(write(fd, data, sizeof(data)) == sizeof(data));
        }
        /* Patch across a filesystem block boundary with a non-block-sized write. */
        check(lseek(fd, 1023L, SEEK_SET) == 1023L);
        for (unsigned i = 0; i < 513; ++i) data[i] = 0xffffU - i;
        check(write(fd, data, 513) == 513);
        check(lseek(fd, 0L, SEEK_SET) == 0L);
    }
    unsigned long position = 0;
    int got;
    while ((got = read(fd, data, 997)) > 0) {
        for (int i = 0; i < got; ++i, ++position) {
            unsigned expected = position >= 1023 && position < 1536 ? 0xffffU - (unsigned)(position - 1023) : value(position);
            check(data[i] == expected);
        }
    }
    check(got == 0 && position == 20480UL && close(fd) == 0);
    puts("disk checks passed (wide cells, partial writes, blocks and EOF)");
    return 0;
}
