#include "../runtime/memory.c"
extern unsigned short rustcell_entry(void);
extern void exit(int);

void rustcell_panic(void) { exit(101); }
static unsigned drops;
void rustcell_report(unsigned short value) { if(value!=901) exit(102); ++drops; }
int main(void) {
    int result = (int)rustcell_entry();
    return result ? result : (drops == 1 ? 0 : 103);
}
