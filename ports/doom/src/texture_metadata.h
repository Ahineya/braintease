/* SPDX-License-Identifier: GPL-2.0-or-later
 * Runtime texture layout shared with the cooked metadata validation fixture. */
#ifndef DOOM_PORT_TEXTURE_METADATA_H
#define DOOM_PORT_TEXTURE_METADATA_H

typedef struct {
    short originx;
    short originy;
    int patch;
} texpatch_t;

typedef struct texture_s texture_t;
struct texture_s {
    char name[8];
    short width;
    short height;
    int index;
    texture_t *next;
    short patchcount;
    texpatch_t patches[1];
};

#endif
