/* SPDX-License-Identifier: GPL-2.0-or-later
 * Configured, bounded IWAD storage for Doom's existing file-class interface.
 * The same reader serves native validation and Ripple MMIO storage.
 */
#include <stdlib.h>
#include <string.h>
#include "doom_build_config.h"
#include "wad_io.h"
#include "w_file.h"
#include "i_system.h"

typedef struct {
    wad_file_t base;
    WadFile input;
} port_wad_file_t;

extern wad_file_class_t stdc_wad_file;
static port_wad_file_t *active_wad;

static wad_file_t *port_open(char *path) {
    port_wad_file_t *file;
    if (active_wad != NULL)
        I_Error("This port supports one configured IWAD; additional WADs are not supported");
    if (path == NULL || (strcmp(path, DOOM_WAD_PATH) != 0 && strcmp(path, "doom1.wad") != 0))
        I_Error("WAD path is not the configured fixture: %s", path ? path : "(null)");
    file = malloc(sizeof(port_wad_file_t));
    if (!file) I_Error("No memory for WAD file handle");
    /* W_AddFile validates and builds Doom's directory. Do not build a second,
     * unused WadLump index here before the engine reads the same records. */
    if (!wad_open_stream(&file->input, DOOM_WAD_PATH, DOOM_WAD_LENGTH)) {
        const char *error = file->input.error;
        free(file);
        I_Error("Cannot open configured WAD: %s", error);
        return NULL;
    }
    file->base.file_class = &stdc_wad_file;
    file->base.mapped = NULL;
    file->base.length = file->input.length;
    active_wad = file;
    return &file->base;
}

static void port_close(wad_file_t *base) {
    port_wad_file_t *file = (port_wad_file_t *)base;
    wad_close(&file->input);
    if (active_wad == file) active_wad = NULL;
    free(file);
}

static size_t port_read(wad_file_t *base, uint32_t offset, void *buffer, size_t count) {
    port_wad_file_t *file = (port_wad_file_t *)base;
    if (count > UINT32_MAX || !wad_read(&file->input, offset, (WadOctet *)buffer, (uint32_t)count))
        return 0;
    return count;
}

wad_file_class_t stdc_wad_file = {
    port_open,
    port_close,
    port_read
};
