#include <stdio.h>
#include <stdint.h>
#include <mmio.h>
#include "../../examples/rOS/disk.c"
#include "../../examples/rOS/fat16.c"

/* Build a tiny unpartitioned FAT16 image with immediate byte stores only.
 * Passing string literals into helpers has miscompiled in large TUs. */
static void make_mini_fat(void) {
    disk_write8(0, 0xEB);
    disk_write8(1, 0x3C);
    disk_write8(2, 0x90);
    disk_write8(3, 'm');
    disk_write8(4, 'k');
    disk_write8(5, 'f');
    disk_write8(6, 's');
    disk_write8(7, '.');
    disk_write8(8, 'f');
    disk_write8(9, 'a');
    disk_write8(10, 't');
    disk_write16(11, 512);
    disk_write8(13, 1);
    disk_write16(14, 1);
    disk_write8(16, 1);
    disk_write16(17, 16);
    disk_write16(19, 32);
    disk_write8(21, 0xF8);
    disk_write16(22, 1);
    disk_write16(24, 32);
    disk_write16(26, 2);
    disk_write32(32, 32);
    disk_write8(36, 0x80);
    disk_write8(38, 0x29);
    disk_write32(39, 0x1234ABCDUL);
    disk_write8(43, 'R');
    disk_write8(44, 'I');
    disk_write8(45, 'P');
    disk_write8(46, 'P');
    disk_write8(47, 'L');
    disk_write8(48, 'E');
    disk_write8(49, 'T');
    disk_write8(50, 'S');
    disk_write8(51, 'T');
    disk_write8(52, ' ');
    disk_write8(53, ' ');
    disk_write8(54, 'F');
    disk_write8(55, 'A');
    disk_write8(56, 'T');
    disk_write8(57, '1');
    disk_write8(58, '6');
    disk_write8(59, ' ');
    disk_write8(60, ' ');
    disk_write8(61, ' ');
    disk_write16(510, 0xAA55);

    disk_write16(512, 0xFFF8);
    disk_write16(514, 0xFFFF);
    disk_write16(516, 0xFFFF);
    disk_write16(518, 0xFFFF);
    disk_write16(520, 0xFFFF);

    /* Root: HELLO.TXT cluster 2 size 3 */
    disk_write8(1024, 'H');
    disk_write8(1025, 'E');
    disk_write8(1026, 'L');
    disk_write8(1027, 'L');
    disk_write8(1028, 'O');
    disk_write8(1029, ' ');
    disk_write8(1030, ' ');
    disk_write8(1031, ' ');
    disk_write8(1032, 'T');
    disk_write8(1033, 'X');
    disk_write8(1034, 'T');
    disk_write8(1035, 0x20);
    disk_write16(1050, 2);
    disk_write32(1052, 3);

    /* Root: SUBDIR cluster 3 */
    disk_write8(1056, 'S');
    disk_write8(1057, 'U');
    disk_write8(1058, 'B');
    disk_write8(1059, 'D');
    disk_write8(1060, 'I');
    disk_write8(1061, 'R');
    disk_write8(1062, ' ');
    disk_write8(1063, ' ');
    disk_write8(1064, ' ');
    disk_write8(1065, ' ');
    disk_write8(1066, ' ');
    disk_write8(1067, 0x10);
    disk_write16(1082, 3);
    disk_write32(1084, 0);

    /* Cluster 2: HELLO.TXT */
    disk_write8(1536, 'H');
    disk_write8(1537, 'i');
    disk_write8(1538, '\n');

    /* Cluster 3: . .. FILE.TXT */
    disk_write8(2048, '.');
    disk_write8(2049, ' ');
    disk_write8(2050, ' ');
    disk_write8(2051, ' ');
    disk_write8(2052, ' ');
    disk_write8(2053, ' ');
    disk_write8(2054, ' ');
    disk_write8(2055, ' ');
    disk_write8(2056, ' ');
    disk_write8(2057, ' ');
    disk_write8(2058, ' ');
    disk_write8(2059, 0x10);
    disk_write16(2074, 3);

    disk_write8(2080, '.');
    disk_write8(2081, '.');
    disk_write8(2082, ' ');
    disk_write8(2083, ' ');
    disk_write8(2084, ' ');
    disk_write8(2085, ' ');
    disk_write8(2086, ' ');
    disk_write8(2087, ' ');
    disk_write8(2088, ' ');
    disk_write8(2089, ' ');
    disk_write8(2090, ' ');
    disk_write8(2091, 0x10);
    disk_write16(2106, 0);

    disk_write8(2112, 'F');
    disk_write8(2113, 'I');
    disk_write8(2114, 'L');
    disk_write8(2115, 'E');
    disk_write8(2116, ' ');
    disk_write8(2117, ' ');
    disk_write8(2118, ' ');
    disk_write8(2119, ' ');
    disk_write8(2120, 'T');
    disk_write8(2121, 'X');
    disk_write8(2122, 'T');
    disk_write8(2123, 0x20);
    disk_write16(2138, 4);
    disk_write32(2140, 3);

    /* Cluster 4: FILE.TXT */
    disk_write8(2560, 'O');
    disk_write8(2561, 'K');
    disk_write8(2562, '\n');
}

int main() {
    Fat16Fs fs;
    Fat16DirEnt ent;
    Fat16Dir dir;
    uint8_t buf[8];
    int r;
    int n;
    int saw_hello;
    int saw_sub;

    make_mini_fat();

    r = fat16_mount(&fs);
    if (r == 0 && fs.bytes_per_sec == 512 && fs.sec_per_clus == 1) {
        putchar('Y');
    } else {
        putchar('N');
    }
    if (fs.root_offset == 1024 && fs.data_offset == 1536) {
        putchar('Y');
    } else {
        putchar('N');
    }

    /* Confirm the root dirent bytes actually landed on the disk. */
    if (disk_read8(1024) == 'H' && disk_read8(1035) == 0x20 && disk_read16(1050) == 2) {
        putchar('Y');
    } else {
        putchar('N');
    }

    r = fat16_lookup(&fs, 0, "hello.txt", &ent);
    if (r == 0 && ent.cluster == 2 && ent.size == 3) {
        putchar('Y');
    } else {
        putchar('N');
    }

    n = fat16_read_at(&fs, ent.cluster, ent.size, 0, buf, 8);
    if (n == 3 && buf[0] == 'H' && buf[1] == 'i' && buf[2] == '\n') {
        putchar('Y');
    } else {
        putchar('N');
    }

    r = fat16_resolve(&fs, 0, "subdir\\file.txt", &ent);
    if (r == 0 && (ent.attr & FAT16_ATTR_DIR) == 0 && ent.size == 3) {
        putchar('Y');
    } else {
        putchar('N');
    }
    n = fat16_read_at(&fs, ent.cluster, ent.size, 0, buf, 8);
    if (n == 3 && buf[0] == 'O' && buf[1] == 'K') {
        putchar('Y');
    } else {
        putchar('N');
    }

    r = fat16_resolve(&fs, 0, "\\subdir", &ent);
    if (r == 0 && (ent.attr & FAT16_ATTR_DIR)) {
        putchar('Y');
    } else {
        putchar('N');
    }

    saw_hello = 0;
    saw_sub = 0;
    fat16_dir_open(&fs, 0, &dir);
    while (fat16_dir_next(&fs, &dir, &ent)) {
        if (ent.name[0] == 'H' && ent.name[1] == 'E') {
            saw_hello = 1;
        }
        if (ent.name[0] == 'S' && ent.name[1] == 'U') {
            saw_sub = 1;
        }
    }
    if (saw_hello && saw_sub) {
        putchar('Y');
    } else {
        putchar('N');
    }

    putchar('\n');
    return 0;
}
