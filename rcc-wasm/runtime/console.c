/* One UTF-8 octet per C16 cell. Host decodes this borrowed buffer once per
 * newline or 8192-cell batch, never once per putchar. */
extern void __wasm_host_write(const unsigned short *data, unsigned long count);
extern void __wasm_trap(int code);
extern void __wasm_exit(int status);
static unsigned short output[8192];
static unsigned count;
void __wasm_flush(void) {
    if (count) { __wasm_host_write(output, count); count = 0; }
}
int putchar(int value) {
    output[count++] = value & 255;
    if (count == 8192 || value == 10) __wasm_flush();
    return value;
}
int puts(const unsigned short *text) {
    while (*text) putchar(*text++);
    putchar(10); return 0;
}
void mmio_write(unsigned address, unsigned value) {
    if (address == 0) { putchar(value); return; }
    __wasm_trap(5);
}
unsigned mmio_read(unsigned address) { __wasm_trap(5); return 0; }
void exit(int status) { __wasm_flush(); __wasm_exit(status); }
void rfl_halt(void) { exit(1); }
