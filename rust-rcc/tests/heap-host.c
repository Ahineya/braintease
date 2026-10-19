#include "../runtime/memory.c"
extern unsigned short rustcell_entry(void);
void rustcell_panic(void) { exit(101); }
int main(void) { return (int)rustcell_entry(); }
