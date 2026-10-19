/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Included in the Nano-X client after its image helpers. File bytes are octets,
 * even on C16, and are decoded explicitly instead of casting a byte buffer. */
#include <stdio.h>
static int image_file_word(FILE *file, unsigned short *word) {
    int lo = fgetc(file), hi = fgetc(file);
    if (lo < 0 || hi < 0 || lo > 255 || hi > 255) return -1;
    *word = (unsigned)lo | (unsigned)hi << 8;return 0;
}
FDE_IMAGE fde_image_load_file(const char *path) {
    FILE *file;
    unsigned short width, height, flags, reserved, words[256];
    unsigned count, i, status = NXR_IMAGE_INVALID;
    unsigned long total, offset = 0;
    FDE_IMAGE id = 0;
    if (!path) {image_status = NXR_IMAGE_INVALID;return 0;}
    file = fopen(path, "rb");if (!file) {image_status = 5;return 0;}
    if (fgetc(file) != 'F' || fgetc(file) != 'D' || fgetc(file) != 'I' || fgetc(file) != '1' ||
        image_file_word(file, &width) < 0 || image_file_word(file, &height) < 0 ||
        image_file_word(file, &flags) < 0 || image_file_word(file, &reserved) < 0 || reserved)
        goto done;
    id = fde_image_create(width, height, flags);
    if (!id) {status = image_status;goto done;}
    total = flags == FDE_IMAGE_MASK_ONLY ? 0 : (unsigned long)width * height;
    if (flags) total += (unsigned long)((width + 15) / 16) * height;
    while (offset < total) {
        count = total - offset > 256 ? 256 : (unsigned)(total - offset);
        for (i = 0; i < count; ++i) if (image_file_word(file, words + i) < 0) goto failed;
        if (fde_image_upload(id, offset, words, count) < 0) {status = image_status;goto failed;}
        offset += count;
    }
    if (fgetc(file) != EOF) goto failed;
    if (ferror(file)) {status = 5;goto failed;}
    status = 0;goto done;
failed:
    fde_image_destroy(id);id = 0;
done:
    if (fclose(file) && !status) {
        if (id) fde_image_destroy(id);
        id = 0;status = 5;
    }
    image_status = status;return id;
}
