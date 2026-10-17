#include <stdio.h>
#include <string.h>
int main(void) {
    const char *date = __DATE__, *time = __TIME__;
    if (strlen(date) == 11 && date[3] == ' ' && date[6] == ' ') putchar('Y'); else putchar('N');
    if (strlen(time) == 8 && time[2] == ':' && time[5] == ':') putchar('Y'); else putchar('N');
    putchar('\n'); return 0;
}
