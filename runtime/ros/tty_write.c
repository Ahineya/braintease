/* rOS uses INT21 output; standalone DEVICE_* DMA is not available here. */
extern void putchar(int);
void tty_write_bytes(const char *data, unsigned short count) {
    while (count--) putchar(*data++);
}
