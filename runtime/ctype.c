#include <ctype.h>

static int in_range(int c) {
    return c >= 0 && c <= 255;
}

int isdigit(int c) {
    return c >= '0' && c <= '9';
}

int islower(int c) {
    return c >= 'a' && c <= 'z';
}

int isupper(int c) {
    return c >= 'A' && c <= 'Z';
}

int isalpha(int c) {
    return islower(c) || isupper(c);
}

int isalnum(int c) {
    return isalpha(c) || isdigit(c);
}

int isblank(int c) {
    return c == ' ' || c == '\t';
}

int isspace(int c) {
    return c == ' ' || c == '\f' || c == '\n' || c == '\r' || c == '\t' || c == '\v';
}

int iscntrl(int c) {
    if (!in_range(c)) {
        return 0;
    }
    return c < 32 || c == 127;
}

int isprint(int c) {
    return c >= 32 && c <= 126;
}

int isgraph(int c) {
    return c >= 33 && c <= 126;
}

int ispunct(int c) {
    return isgraph(c) && !isalnum(c);
}

int isxdigit(int c) {
    return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int tolower(int c) {
    if (isupper(c)) {
        return c - 'A' + 'a';
    }
    return c;
}

int toupper(int c) {
    if (islower(c)) {
        return c - 'a' + 'A';
    }
    return c;
}
