void putchar(int c);

#define STR(x) #x

int main() {
    char *s;
    s = STR(hello);
    if (s[0] == 'h' && s[1] == 'e' && s[2] == 'l' && s[3] == 'l' && s[4] == 'o' && s[5] == 0) {
        putchar('Y');
    } else {
        putchar('N');
    }
    putchar('\n');
    return 0;
}
