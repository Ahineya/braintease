/* Exercise the storage-word API independently of logical WAD reads. The raw
 * prefix checksums come from the original shareware WAD; cooked metadata is
 * checked field by field rather than decoded through the production reader. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "doom_build_config.h"
#include "wad_io.h"

static uint16_t global_words[18];

static void check(int good, const char *message)
{
    if (!good) {
        printf("ASSET WORDS ERROR %s\n", message);
        exit(1);
    }
}

#if DOOM_WAD_MODE != 0
static void check_destination(WadFile *wad, uint16_t *words)
{
    words[0] = 0xa55aU;
    words[17] = 0x5aa5U;
#if DOOM_WAD_MODE == 2
    check(wad_read_words(wad, DOOM_ASSET_HEADER_WORD, words + 1, 16UL),
          "cooked header read");
    check(words[1] == 0x5257U && words[2] == 0x4144U,
          "full 16-bit magic retained");
    check(words[3] == 2U && words[4] == 1U, "version and ABI");
    check(words[5] == (uint16_t)DOOM_WAD_LENGTH &&
          words[6] == (uint16_t)(DOOM_WAD_LENGTH >> 16), "source length");
    check(words[7] == (uint16_t)DOOM_ASSET_LUMP_COUNT &&
          words[8] == (uint16_t)(DOOM_ASSET_LUMP_COUNT >> 16), "lump count");
    check(words[9] == (uint16_t)DOOM_ASSET_INDEX_WORD &&
          words[10] == (uint16_t)(DOOM_ASSET_INDEX_WORD >> 16), "index address");
    check(words[11] == (uint16_t)DOOM_ASSET_TOTAL_WORDS &&
          words[12] == (uint16_t)(DOOM_ASSET_TOTAL_WORDS >> 16), "image length");
    check(words[13] == DOOM_ASSET_FINGERPRINT_LO &&
          words[14] == DOOM_ASSET_FINGERPRINT_HI, "full fingerprint words");
    check(words[15] == (uint16_t)DOOM_ASSET_DIRECTORY_WORD &&
          words[16] == (uint16_t)(DOOM_ASSET_DIRECTORY_WORD >> 16), "decoded directory address");
#else
    check(wad_read_words(wad, 0UL, words + 1, 16UL), "expanded header read");
    check(words[1] == 73 && words[2] == 87 && words[3] == 65 && words[4] == 68,
          "expanded header magic");
    check(words[5] == 240 && words[6] == 4 && words[7] == 0 && words[8] == 0,
          "expanded header count");
    check(words[9] == 180 && words[10] == 183 && words[11] == 63 && words[12] == 0,
          "expanded directory offset");
    check(words[13] == 0 && words[14] == 0 && words[15] == 0 && words[16] == 31,
          "expanded payload prefix");
#endif
    check(words[0] == 0xa55aU && words[17] == 0x5aa5U, "destination guards");
}

static void check_bulk(WadFile *wad)
{
    const uint32_t count = 65540UL;
    uint16_t *words = malloc((size_t)(count + 2UL) * sizeof(*words));
    uint32_t sum = 0;
    uint32_t weighted = 0;
    uint32_t i;
    check(words != NULL, "far allocation");
    words[0] = 0xa55aU;
    words[count + 1UL] = 0x5aa5U;
    /* The source crosses three 32,768-word storage blocks. A destination this
     * long necessarily crosses a 64,000-cell Ripple RAM bank. */
    check(wad_read_words(wad, 1UL, words + 1, count), "far bulk read");
    for (i = 0; i < count; ++i) {
        uint16_t value = words[i + 1UL];
        check(value <= 255U, "expanded octet width");
        sum += value;
        weighted += (uint32_t)value * ((i & 255UL) + 1UL);
    }
    check(sum == 3480150UL && weighted == 453830716UL,
          "independent source checksums");
    check(words[0] == 0xa55aU && words[count + 1UL] == 0x5aa5U,
          "bulk destination guards");
    check_destination(wad, words + 64000UL);
    free(words);
}
#endif

int main(void)
{
    WadFile wad;
    uint16_t stack_words[18];
    uint16_t guard = 0xa55aU;
    check(DOOM_WAD_LENGTH == 4196020UL, "expected shareware fixture");
    check(wad_open_stream(&wad, DOOM_WAD_PATH, DOOM_WAD_LENGTH), "open stream");
#if DOOM_WAD_MODE == 0
    check(!wad_read_words(&wad, 0UL, &guard, 1UL), "packed API rejection");
#else
    check_destination(&wad, stack_words);
    check_destination(&wad, global_words);
    check_bulk(&wad);
    check(wad_read_words(&wad, 0UL, NULL, 0UL), "empty null read");
    check(wad_read_words(&wad, DOOM_ASSET_TOTAL_WORDS, NULL, 0UL), "empty EOF read");
    check(wad_read_words(&wad, DOOM_ASSET_TOTAL_WORDS - 1UL, &guard, 1UL), "last word");
    guard = 0xa55aU;
    check(!wad_read_words(&wad, 0UL, NULL, 1UL), "nonempty null read");
    check(!wad_read_words(&wad, DOOM_ASSET_TOTAL_WORDS, &guard, 1UL), "read beyond EOF");
    check(!wad_read_words(&wad, DOOM_ASSET_TOTAL_WORDS + 1UL, NULL, 0UL), "empty beyond EOF");
    check(!wad_read_words(&wad, UINT32_MAX, &guard, 2UL), "offset wrap");
    check(!wad_read_words(&wad, 1UL, &guard, UINT32_MAX), "length wrap");
#endif
    check(guard == 0xa55aU, "rejected read changed destination");
    wad_close(&wad);
    check(wad.asset_file == NULL && wad.host_file == NULL, "closed streams");
    puts("ASSET WORDS OK storage modes, destinations, bank boundaries, full words, bounds");
    return 0;
}
