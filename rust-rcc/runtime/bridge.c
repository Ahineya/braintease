/* Explicit Rust/C16 boundary: only carrier values and octet buffers cross it.
 * Rust u16 OBJECTS occupy two cells; do not expose a C unsigned short* as one.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include "memory.c"
#include "os.c"

extern unsigned short rustcell_entry(void);

void rustcell_report(unsigned short value) {
    printf("RUSTCELL_VALUE %u\n", value);
}

long rustcell_write(const unsigned char *bytes, unsigned long length) {
    return (long)write(1, bytes, length);
}

long rustcell_read(unsigned char *bytes, unsigned long length) {
    long count = (long)read(0, bytes, length);
    long i;
    /* C16 read stores octets in cells; ensure the Rust u8 invariant explicitly. */
    for (i = 0; i < count; ++i) bytes[i] &= 255;
    return count;
}

short rustcell_fork(void) { return (short)fork(); }

short rustcell_wait(short pid) {
    int status;
    if (waitpid((int)pid, &status, 0) != pid) return -1;
    return WIFEXITED(status) ? (short)WEXITSTATUS(status) : -1;
}

unsigned short rustcell_sleep(unsigned short seconds) { return sleep(seconds); }

void rustcell_panic(void) {
    static const char message[] = "RUSTCELL_PANIC\n";
    write(2, message, sizeof(message)-1);
    _exit(101);
}

int main(int argc, char **argv) {
    rustcell_arg_count = argc;
    rustcell_arg_values = argv;
    return (int)rustcell_entry();
}
