/* Exercise the actual W_AddFile parser with synthetic directories whose
 * counts are not multiples of its batch size. The file adapter is a bounded
 * serialized-octet array; lookup and zone allocation remain engine code. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "i_system.h"
#include "w_wad.h"
#include "wad_io.h"
#include "z_zone.h"

static WadOctet image[284];
static wad_file_t file;
static unsigned int read_count;
static uint32_t read_offsets[3];
static size_t read_lengths[3];

void I_Error(char *format, ...)
{
    va_list args;
    va_start(args, format);
    printf("WAD DIRECTORY ERROR ");
    vprintf(format, args);
    va_end(args);
    putchar('\n');
    exit(1);
}

byte *I_ZoneBase(size_t *size)
{
    *size = 32768UL;
    return malloc(*size);
}

void I_BeginRead(void) {}
void I_EndRead(void) {}

char *D_SuggestGameName(GameMission_t mission, GameMode_t mode)
{
    (void)mission;
    (void)mode;
    return "fixture";
}

char *D_GameMissionString(GameMission_t mission)
{
    (void)mission;
    return "fixture";
}

static void check(int condition, const char *message)
{
    if (!condition) I_Error("%s", message);
}

wad_file_t *W_OpenFile(char *path)
{
    check(strcmp(path, "synthetic") == 0, "open path");
    return &file;
}

size_t W_Read(wad_file_t *wad, uint32_t offset, void *buffer, size_t length)
{
    WadOctet *dest = buffer;
    size_t i;
    check(wad == &file, "file identity");
    check(offset <= file.length && length <= file.length - offset,
          "parser read beyond serialized file");
    check(read_count < 3, "unexpected extra directory read");
    read_offsets[read_count] = offset;
    read_lengths[read_count] = length;
    ++read_count;
    for (i = 0; i < length; ++i) dest[i] = image[offset + i];
    return length;
}

static void put_u32(unsigned int offset, uint32_t value)
{
    image[offset] = (WadOctet)(value & 255UL);
    image[offset + 1] = (WadOctet)((value >> 8) & 255UL);
    image[offset + 2] = (WadOctet)((value >> 16) & 255UL);
    image[offset + 3] = (WadOctet)((value >> 24) & 255UL);
}

static void prepare(unsigned int count)
{
    unsigned int i;
    unsigned int j;
    memset(image, 0, sizeof(image));
    image[0] = 'I'; image[1] = 'W'; image[2] = 'A'; image[3] = 'D';
    put_u32(4, (uint32_t)count);
    put_u32(8, 12UL);
    file.file_class = NULL;
    file.mapped = NULL;
    file.length = 12UL + (uint32_t)count * 16UL;
    read_count = 0;
    for (i = 0; i < count; ++i) {
        unsigned int record = 12U + i * 16U;
        const char *name = "ENTRY000";
        uint32_t position = 4UL + i;
        uint32_t length = 1UL;
        if (count == 1) name = "SINGLE08";
        else if (i == 0 || i == 16) name = "DUPENAME";
        else if (i == 15) name = "ABCDEFGH";
        if (i == count - 1) {
            position = file.length;
            length = 0UL;
        }
        put_u32(record, position);
        put_u32(record + 4, length);
        for (j = 0; j < 8; ++j) image[record + 8 + j] = name[j];
        if (i > 0 && i < 15) image[record + 15] = (WadOctet)('A' + i);
    }
}

static void check_lookup(unsigned int count)
{
    if (count == 1) {
        check(W_CheckNumForName("single08") == 0, "single full-length name");
    } else {
        check(W_CheckNumForName("dupename") == 16, "last duplicate wins");
        check(W_CheckNumForName("ABCDEFGH") == 15, "eight-character name");
        check(W_CheckNumForName("ENTRY00B") == 1, "first batch entry");
        check(W_CheckNumForName("ENTRY00O") == 14, "last ordinary entry");
    }
    check(W_CheckNumForName("ABSENT") == -1, "absent name");
}

static void directory_case(unsigned int count)
{
    unsigned int i;
    prepare(count);
    check(W_AddFile("synthetic") == &file && numlumps == count, "directory count");
    check(read_offsets[0] == 0UL && read_lengths[0] == 12UL, "header read");
    check(read_offsets[1] == 12UL, "directory start");
    if (count == 1) {
        check(read_count == 2 && read_lengths[1] == 16UL, "single-entry tail");
    } else {
        check(read_count == 3 && read_lengths[1] == 256UL &&
              read_offsets[2] == 268UL && read_lengths[2] == 16UL,
              "full batch plus one-entry tail");
    }
    for (i = 0; i < count; ++i) {
        check(lumpinfo[i].wad_file == &file && lumpinfo[i].cache == NULL &&
              lumpinfo[i].next == NULL, "initial entry state");
        check(lumpinfo[i].position == (i == count - 1 ? file.length : 4UL + i),
              "entry position");
        check(lumpinfo[i].size == (i == count - 1 ? 0UL : 1UL), "entry size");
    }
    check(W_LumpLength(count - 1) == 0UL, "zero-length EOF marker");
    check_lookup(count);
    W_GenerateHashTable();
    check_lookup(count);
    Z_CheckHeap();
}

int main(void)
{
    Z_Init();
    directory_case(1);
    /* W_AddFile deliberately supports one input. Reset its public directory
     * state between independent fixtures; it releases the old hash itself. */
    free(lumpinfo);
    lumpinfo = NULL;
    numlumps = 0;
    directory_case(17);
    puts("WAD DIRECTORY OK 1/17 entries, batch tail, full names, EOF marker, duplicate lookup");
    return 0;
}
