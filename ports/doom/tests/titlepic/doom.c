// Doom WAD titlepic
#include <stdio.h>
#include <graphics.h>
#include <mmio.h>
#include <string.h>
#include <stdlib.h>

// NOTE: Int is 16 bits.

void unpack(int p, char* unpacked) {
    unpacked[0] = (p & 0xFF);
    unpacked[1] = (p >> 8) & 0xFF;
}

void print_digit(int n) {
    if (n >= 0 && n <= 9) {
        putchar('0' + n);
    } else {
        putchar('?');
    }
}

void print_uint(int n) {
    if (n >= 10000) {
        print_digit(n / 10000);
        print_digit((n / 1000) % 10);
        print_digit((n / 100) % 10);
        print_digit((n / 10) % 10);
        print_digit(n % 10);
    } else
    if (n >= 1000) {
        print_digit(n / 1000);
        print_digit((n / 100) % 10);
        print_digit((n / 10) % 10);
        print_digit(n % 10);
    } else if (n >= 100) {
        print_digit(n / 100);
        print_digit((n / 10) % 10);
        print_digit(n % 10);
    } else if (n >= 10) {
        print_digit(n / 10);
        print_digit(n % 10);
    } else {
        print_digit(n);
    }
}

void print_hex(unsigned int n) {
    char buf[16];
    int i = 0;
    char *digits = "0123456789abcdef";

    if (n == 0) {
        putchar('0');
        return;
    }

    while (n > 0) {
        buf[i++] = digits[n % 16];
        n /= 16;
    }
    while (i--) {
        putchar(buf[i]);
    }
}

void pad_hex(unsigned int n) {
    if (n < 0x10) {
        putchar('0');
    }
    print_hex(n);
}

void br() {
    putchar('\n');
}

void add_words(unsigned int *bank, unsigned int *addr, unsigned int words) {
    unsigned int a = *addr + words;     // words, 16-bit address space
    *bank += (a >> 16);                  // carry if crossed 0xFFFF
    *addr = (unsigned int)(a & 0xFFFF);
}

// unpack one 16-bit word into 2 chars, little-endian
void unpack_word(unsigned int w, char *out2) {
    out2[0] = (char)(w & 0xFF);
    out2[1] = (char)((w >> 8) & 0xFF);
}

void to_bank_addr_plus_bytes(int hi, int lo, int bytes, unsigned int *bank, unsigned int *addr) {
    unsigned int a = (lo + bytes) / 2; // Convert to word offset
    *bank = hi / 2;                    // Bank is high part divided by 2
    if (hi & 1) {
        a = a | 0x8000;                    // Set MSB if hi was odd
    }
    *addr = a & 0xFFFF;                 // Address is low part masked to 16 bits
}

int main() {

    int* image_start = (int*)malloc(32000);
    int* image_end   = (int*)malloc(32000);
//
//    if (image_start == NULL) {
//        puts("Failed to allocate memory for image.");
//        return 1;
//    }

    int ptr = 0;
        unsigned int packed_header[2];
        storage_read_buffer(0, ptr, packed_header, 2);

        char header[5];
        unpack_word(packed_header[0], &header[0]);
        unpack_word(packed_header[1], &header[2]);
        header[4] = '\0';
        puts(header); // "IWAD"

        // lump count (fits into 16 bits for DOOM1.WAD)
        int lump_count = storage_read_at(0, 2);
        puts("Lump count:");
        print_uint(lump_count);
        br();

        // directory offset (4 bytes = 2 words), little-endian
        unsigned int lo = storage_read_at(0, 4); // 0xB7B4
        unsigned int hi = storage_read_at(0, 5); // 0x003F

        puts("Directory offset:");
        print_hex(hi);        // 3f
        print_hex(lo & 0xff); // b4
        print_hex(lo >> 8);   // b7
        br();

        int pic_size_hi = 0;
        int pic_size_lo = 0;

        int pic_addr_hi = 0;
        int pic_addr_lo = 0;

        // At some point in time I actually could extract this as a function, but current compiler can't pointer.
        for (int i = 0; i < lump_count; i++) {
            unsigned int bank, addr;
            // +8 = skip filepos+size, +i*16 = go to ith entry
            to_bank_addr_plus_bytes(hi, lo, i*16 + 8, &bank, &addr);

            // Read full 8-byte name (4 words)
            unsigned int name_words[4];
            storage_read_buffer(bank, addr, name_words, 4);

            // Unpack to 8 chars and NUL-terminate
            char lump_name[9];
            unpack_word(name_words[0], &lump_name[0]);
            unpack_word(name_words[1], &lump_name[2]);
            unpack_word(name_words[2], &lump_name[4]);
            unpack_word(name_words[3], &lump_name[6]);
            lump_name[8] = '\0';

            if (strcmp(lump_name, "TITLEPIC") == 0) {
                puts("Found TITLEPIC at:");
                pad_hex(bank);
                pad_hex(addr >> 8);
                pad_hex(addr & 0xFF);
                br();

                pic_size_hi = storage_read_at(bank, addr - 1);
                pic_size_lo = storage_read_at(bank, addr - 2);

                pic_addr_hi = storage_read_at(bank, addr - 3);
                pic_addr_lo = storage_read_at(bank, addr - 4);

                break;
            }
        }

        puts("TITLEPIC struct size:");
        pad_hex(pic_size_hi);
        pad_hex(pic_size_lo >> 8);
        pad_hex(pic_size_lo & 0xFF);
        br();

        puts("TITLEPIC address:");
        pad_hex(pic_addr_hi);
        pad_hex(pic_addr_lo >> 8);
        pad_hex(pic_addr_lo & 0xFF);
        br();

        // Now we have the address and size of TITLEPIC. Hooray!
        unsigned int pic_bank, pic_addr;
        to_bank_addr_plus_bytes(pic_addr_hi, pic_addr_lo, 0, &pic_bank, &pic_addr);

        puts("TITLEPIC words address:");
        pad_hex(pic_bank);
        pad_hex(pic_addr >> 8);
        pad_hex(pic_addr & 0xFF);
        br();

        int pic_width = storage_read_at(pic_bank, pic_addr);
        int pic_height = storage_read_at(pic_bank, pic_addr + 1);

        puts("TITLEPIC dimensions:");
        print_uint(pic_width);
        putchar('x');
        print_uint(pic_height);
        br();

        // Patch header address in words.
        int patch_header_bank = pic_bank;
        int patch_header_addr = pic_addr;
        // Patch header address in bytes.
        int patch_header_bank_bytes = pic_addr_hi;
        int patch_header_addr_bytes = pic_addr_lo;

        unsigned int POST_HEADER_SIZE = 8; // 4 words, 8 bytes

        unsigned int cols_arr_start_bank_words, cols_arr_start_addr_words;
        to_bank_addr_plus_bytes(patch_header_bank_bytes, patch_header_addr_bytes, POST_HEADER_SIZE, &cols_arr_start_bank_words, &cols_arr_start_addr_words);
        int first_col_offset = storage_read_at(cols_arr_start_bank_words, cols_arr_start_addr_words);
        puts("First column offset:");
        print_uint(first_col_offset);
        br();

        unsigned int first_col_start_bank_words, first_col_start_addr_words;
        int BYTE_OFFSET = 0; // 2 bytes per word
        to_bank_addr_plus_bytes(patch_header_bank_bytes, patch_header_addr_bytes, first_col_offset + BYTE_OFFSET, &first_col_start_bank_words, &first_col_start_addr_words);
        puts("First column start address (words):");
        pad_hex(first_col_start_bank_words);
        pad_hex(first_col_start_addr_words >> 8);
        pad_hex(first_col_start_addr_words & 0xFF);
        br();
        int first_col_start_lo = storage_read_at(first_col_start_bank_words, first_col_start_addr_words);
        // just dump it as hex for now
        puts("First column start address (bytes):");
//        pad_hex(first_col_start_bank_words);
        pad_hex(first_col_start_lo >> 8);
//        pad_hex(first_col_start_lo & 0xFF);
        br();

        // So now "just" need to copy it to the image buffer in "image_start" and "image_end" and then display it.

        // Need to read titlepic with storage_read_at
}
