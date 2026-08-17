void putchar(int c);

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
int c99 = 1;
#else
int c99 = 0;
#endif

#if defined(__STDC__) && __STDC__ == 1
int stdc = 1;
#else
int stdc = 0;
#endif

int main() {
    if (c99) putchar('Y'); else putchar('N');
    if (stdc) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
