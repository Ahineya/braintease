#include <stdio.h>
#include <string.h>

int main(void) {
    char source[4] = { 'A', (char)50000U, 'B', 0 };
    char target[8];
    char high[2] = { (char)50000U, 0 };
    char low[2] = { 1, 0 };
    long words[2] = { 305419896L, 99L };
    long copied[2] = { 0L, 77L };
    unsigned char overlap[6] = { 50000U, 60000U, 1, 2, 3, 4 };

    memset(target, 1234, sizeof(target));
    if (strcpy(target, source) == target && strlen(target) == 3 &&
        (unsigned char)target[1] == 50000U && target[3] == 0 && target[4] == 1234) putchar('Y'); else putchar('N');
    if (strcmp(high, low) > 0 && strcmp(low, high) < 0 && strcmp(source, target) == 0) putchar('Y'); else putchar('N');
    if (strncmp("AB", "AC", 1) == 0 && strncmp("AB", "AC", 2) < 0 &&
        strncmp("a", "b", 65536UL) < 0) putchar('Y'); else putchar('N');
    strncpy(target, "AB", 4);
    if (target[0] == 'A' && target[1] == 'B' && target[2] == 0 &&
        target[3] == 0 && target[4] == 1234) putchar('Y'); else putchar('N');
    target[2] = 99;
    strncpy(target, "XYZ", 2);
    if (target[0] == 'X' && target[1] == 'Y' && target[2] == 99) putchar('Y'); else putchar('N');
    strcpy(target, "ab");
    strcat(target, "cd");
    strncat(target, "EF", 1);
    if (strcmp(target, "abcdE") == 0 && strchr(target, 'c') == target + 2 &&
        strchr(target, 0) == target + 5) putchar('Y'); else putchar('N');
    strcpy(target, "abaca");
    if (strrchr(target, 'a') == target + 4 && strchr(target, 'x') == 0 &&
        strrchr(target, 0) == target + 5) putchar('Y'); else putchar('N');
    memcpy(copied, words, sizeof(long));
    if (copied[0] == 305419896L && copied[1] == 77L) putchar('Y'); else putchar('N');
    memset(copied, 65535U, sizeof(long));
    if (copied[0] == -1L && copied[1] == 77L) putchar('Y'); else putchar('N');
    memmove(overlap + 1, overlap, 5);
    if (overlap[1] == 50000U && overlap[2] == 60000U && overlap[5] == 3) putchar('Y'); else putchar('N');
    memmove(overlap, overlap + 1, 5);
    if (overlap[0] == 50000U && overlap[1] == 60000U && overlap[4] == 3 &&
        memcmp(high, low, 1) > 0) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
