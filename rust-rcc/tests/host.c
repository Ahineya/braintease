/* Small native memory check before booting either guest. */
#include "../runtime/memory.c"
static unsigned short expected[] = {2, 0x34, 0x5634};
static unsigned count, failed;
extern unsigned short rustcell_entry(void);
void rustcell_report(unsigned short value) {
    if (count >= 3 || value != expected[count]) failed = 1;
    ++count;
}
void rustcell_panic(void) { failed = 1; }
int main(void) {
    return rustcell_entry() || failed || count != 3 ? 1 : 42;
}
