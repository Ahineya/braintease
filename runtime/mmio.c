// MMIO access functions for Ripple VM
// These provide memory-mapped I/O access without inline assembly

#include <mmio_constants.h>

// Read from MMIO address (bank 0)
unsigned short mmio_read(unsigned short addr) {
    unsigned short result;
    // Use extended inline assembly with output operand
    // %0 is the output (result), %1 is the input (addr)
    __asm__("LOAD %0, R0, %1" : "=r"(result) : "r"(addr));
    return result;
}

// Write to MMIO address (bank 0)
void mmio_write(unsigned short addr, unsigned short value) {
    // Use extended inline assembly with input operands
    // %0 is value, %1 is addr
    __asm__("STORE %0, R0, %1" : : "r"(value), "r"(addr));
}

// TTY output
void tty_putchar(unsigned char c) {
    mmio_write(MMIO_TTY_OUT, c);
}

// Get random number
unsigned short rng_get(void) {
    return mmio_read(MMIO_RNG);
}

// Get current RNG seed (low 16 bits)
unsigned short rng_get_seed(void) {
    return mmio_read(MMIO_RNG_SEED);
}

// Set RNG seed (low 16 bits)
void rng_set_seed(unsigned short seed) {
    mmio_write(MMIO_RNG_SEED, seed);
}

// Display control
void display_set_mode(unsigned short mode) {
    mmio_write(MMIO_DISP_MODE, mode);
}

void display_set_resolution(unsigned char width, unsigned char height) {
    unsigned short resolution = ((unsigned short)width << 8) | height;
    mmio_write(MMIO_DISP_RESOLUTION, resolution);
}

void display_enable(void) {
    mmio_write(MMIO_DISP_CTL, DISP_CTL_ENABLE);
}

void display_clear(void) {
    mmio_write(MMIO_DISP_CTL, DISP_CTL_ENABLE | DISP_CTL_CLEAR);
}

void display_flush(void) {
    mmio_write(MMIO_DISP_FLUSH, 1);
}

// TEXT40 VRAM access
void text40_putchar(int x, int y, unsigned char c) {
    if (x >= 0 && x < TEXT40_WIDTH && y >= 0 && y < TEXT40_HEIGHT) {
        unsigned short addr = TEXT40_BASE + y * TEXT40_WIDTH + x;
        // In our architecture, char is 16-bit, so mask to get only low 8 bits
        // Write character in low byte, attributes (0) in high byte
        unsigned short value = (c & 0xFF);
        mmio_write(addr, value);
    }
}

void text40_puts(int x, int y, const char* s) {
    int pos = x;
    int i = 0;
    char ch;
    
    // Use index-based access instead of pointer arithmetic
    while ((ch = s[i]) != 0 && pos < TEXT40_WIDTH) {
        // Mask the character to 8 bits since char is 16-bit in our architecture
        text40_putchar(pos, y, ch & 0xFF);
        pos += 1;
        i += 1;
    }
}

// TEXT40 colored character functions
void text40_putchar_color(int x, int y, unsigned char c, unsigned char fg, unsigned char bg) {
    if (x >= 0 && x < TEXT40_WIDTH && y >= 0 && y < TEXT40_HEIGHT) {
        unsigned short addr = TEXT40_BASE + y * TEXT40_WIDTH + x;
        // Create attribute byte from foreground and background colors
        unsigned char attr = MAKE_ATTR(fg & 0x0F, bg & 0x0F);
        // Combine character and attribute into a single 16-bit value
        unsigned short value = ((attr & 0xFF) << 8) | (c & 0xFF);
        mmio_write(addr, value);
    }
}

void text40_putchar_attr(int x, int y, unsigned char c, unsigned char attr) {
    if (x >= 0 && x < TEXT40_WIDTH && y >= 0 && y < TEXT40_HEIGHT) {
        unsigned short addr = TEXT40_BASE + y * TEXT40_WIDTH + x;
        // Combine character and attribute into a single 16-bit value
        unsigned short value = ((attr & 0xFF) << 8) | (c & 0xFF);
        mmio_write(addr, value);
    }
}

void text40_puts_color(int x, int y, const char* s, unsigned char fg, unsigned char bg) {
    int pos = x;
    int i = 0;
    char ch;
    
    // Use index-based access instead of pointer arithmetic
    while ((ch = s[i]) != 0 && pos < TEXT40_WIDTH) {
        // Mask the character to 8 bits since char is 16-bit in our architecture
        text40_putchar_color(pos, y, ch & 0xFF, fg, bg);
        pos += 1;
        i += 1;
    }
}

void text40_puts_attr(int x, int y, const char* s, unsigned char attr) {
    int pos = x;
    int i = 0;
    char ch;
    
    // Use index-based access instead of pointer arithmetic
    while ((ch = s[i]) != 0 && pos < TEXT40_WIDTH) {
        // Mask the character to 8 bits since char is 16-bit in our architecture
        text40_putchar_attr(pos, y, ch & 0xFF, attr);
        pos += 1;
        i += 1;
    }
}

// TEXT40 attribute functions
void text40_set_attr(int x, int y, unsigned char attr) {
    if (x >= 0 && x < TEXT40_WIDTH && y >= 0 && y < TEXT40_HEIGHT) {
        unsigned short addr = TEXT40_BASE + y * TEXT40_WIDTH + x;
        // Read current value
        unsigned short current = mmio_read(addr);
        // Keep character, update attribute
        unsigned short value = ((attr & 0xFF) << 8) | (current & 0xFF);
        mmio_write(addr, value);
    }
}

unsigned char text40_get_char(int x, int y) {
    if (x >= 0 && x < TEXT40_WIDTH && y >= 0 && y < TEXT40_HEIGHT) {
        unsigned short addr = TEXT40_BASE + y * TEXT40_WIDTH + x;
        unsigned short value = mmio_read(addr);
        // Return the low byte (character)
        return value & 0xFF;
    }
    return 0;
}

unsigned char text40_get_attr(int x, int y) {
    if (x >= 0 && x < TEXT40_WIDTH && y >= 0 && y < TEXT40_HEIGHT) {
        unsigned short addr = TEXT40_BASE + y * TEXT40_WIDTH + x;
        unsigned short value = mmio_read(addr);
        // Return the high byte (attribute)
        return (value >> 8) & 0xFF;
    }
    return 0;
}

// Keyboard input functions (TEXT40 mode only)
int key_pressed(unsigned short key_addr) {
    return (mmio_read(key_addr) & KEY_PRESSED) != 0;
}

int key_up_pressed(void) {
    return key_pressed(MMIO_KEY_UP);
}

int key_down_pressed(void) {
    return key_pressed(MMIO_KEY_DOWN);
}

int key_left_pressed(void) {
    return key_pressed(MMIO_KEY_LEFT);
}

int key_right_pressed(void) {
    return key_pressed(MMIO_KEY_RIGHT);
}

int key_z_pressed(void) {
    return key_pressed(MMIO_KEY_Z);
}

int key_x_pressed(void) {
    return key_pressed(MMIO_KEY_X);
}

// RGB565 color helpers
unsigned short rgb565_from_rgb(unsigned char r, unsigned char g, unsigned char b) {
    // Convert 8-bit RGB values to RGB565 format
    unsigned short r5 = (r >> 3) & 0x1F;
    unsigned short g6 = (g >> 2) & 0x3F;
    unsigned short b5 = (b >> 3) & 0x1F;
    return (r5 << 11) | (g6 << 5) | b5;
}

void rgb565_set_pixel(unsigned short x, unsigned short y, unsigned short color, unsigned short width) {
    // Calculate pixel address in back buffer
    unsigned short buffer_size = width * width; // Assuming square for simplicity
    unsigned short addr = 32 + buffer_size + (y * width + x);
    mmio_write(addr, color);
}

void rgb565_fill_rect(unsigned short x, unsigned short y, unsigned short w, unsigned short h, 
                       unsigned short color, unsigned short screen_width) {
    for (unsigned short dy = 0; dy < h; dy++) {
        for (unsigned short dx = 0; dx < w; dx++) {
            rgb565_set_pixel(x + dx, y + dy, color, screen_width);
        }
    }
}

// Storage device functions
void storage_set_block(unsigned short block) {
    mmio_write(MMIO_STORE_BLOCK, block);
}

void storage_set_addr(unsigned short addr) {
    mmio_write(MMIO_STORE_ADDR, addr);
}

unsigned short storage_read(void) {
    return mmio_read(MMIO_STORE_DATA);
}

void storage_write(unsigned short value) {
    mmio_write(MMIO_STORE_DATA, value);
}

unsigned short storage_get_status(void) {
    return mmio_read(MMIO_STORE_CTL);
}

void storage_commit(void) {
    mmio_write(MMIO_STORE_CTL, STORE_CTL_COMMIT);
}

void storage_commit_all(void) {
    mmio_write(MMIO_STORE_CTL, STORE_CTL_COMMIT_ALL);
}

int storage_is_busy(void) {
    return (storage_get_status() & STORE_CTL_BUSY) != 0;
}

int storage_is_dirty(void) {
    return (storage_get_status() & STORE_CTL_DIRTY) != 0;
}

// High-level storage operations
void storage_write_at(unsigned short block, unsigned short addr, unsigned short value) {
    storage_set_block(block);
    storage_set_addr(addr);
    storage_write(value);
}

unsigned short storage_read_at(unsigned short block, unsigned short addr) {
    storage_set_block(block);
    storage_set_addr(addr);
    return storage_read();
}

void storage_write_buffer(unsigned short block, unsigned short addr, const unsigned short* data, unsigned short count) {
    storage_set_block(block);
    storage_set_addr(addr);
    for (unsigned short i = 0; i < count; i++) {
        storage_write(data[i]);
    }
}

void storage_read_buffer(unsigned short block, unsigned short addr, unsigned short* data, unsigned short count) {
    storage_set_block(block);
    storage_set_addr(addr);
    for (unsigned short i = 0; i < count; i++) {
        data[i] = storage_read();
    }
}

// Interrupt controller
unsigned short irq_get_status(void) {
    return mmio_read(MMIO_IRQ_STATUS);
}

void irq_raise(unsigned short bits) {
    mmio_write(MMIO_IRQ_STATUS, bits);
}

unsigned short irq_get_enable(void) {
    return mmio_read(MMIO_IRQ_ENABLE);
}

void irq_set_enable(unsigned short mask) {
    mmio_write(MMIO_IRQ_ENABLE, mask);
}

int irq_is_busy(void) {
    return mmio_read(MMIO_IRQ_BUSY) != 0;
}

void irq_ack(void) {
    mmio_write(MMIO_IRQ_BUSY, 1);
}

void irq_set_vector(unsigned short bank, unsigned short off) {
    mmio_write(MMIO_IRQ_VECTOR_BANK, bank);
    mmio_write(MMIO_IRQ_VECTOR_OFF, off);
}

unsigned short irq_get_cause(void) {
    return mmio_read(MMIO_IRQ_CAUSE);
}