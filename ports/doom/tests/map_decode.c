/* Native parser regression harness. Include the engine loader so its private
 * REJECT decoder is tested too. Link with dead-section elimination: gameplay
 * bootstrap is deliberately outside this parser test. The real engine retains
 * its normal implementations of these services.
 *
 * From the repository root:
 *   node ports/doom/tools/test.mjs fixtures --program map_decode
 * This builds the native fixture with the platform's dead-section linker flag
 * and runs it against ports/doom/assets/doom1.wad.
 * Expected: E1M1 loaded; 8 malformed inputs rejected. The REJECT padding
 * warning is an intentional malformed-input test, not a test failure. */
#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "p_setup.c"

#define TEST_LUMPS 11
static byte *test_lumps[TEST_LUMPS];
static uint32_t test_lengths[TEST_LUMPS];
static void *allocations[512];
static int allocation_count;
static int spawned_things;
static int expecting_error;
static jmp_buf error_target;
static unsigned rejected_inputs;
GameMode_t gamemode = shareware;

void I_Error(char *format, ...)
{
    va_list args;
    if (expecting_error)
    {
        ++rejected_inputs;
        longjmp(error_target, 1);
    }
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
    exit(1);
}

void *Z_Malloc(size_t size, int tag, void *user)
{
    void *result = calloc(size ? size : 1, 1);
    (void)tag;
    assert(result != NULL && allocation_count < 512);
    allocations[allocation_count++] = result;
    if (user != NULL)
        *(void **)user = result;
    return result;
}

void Z_Free(void *pointer)
{
    int i;
    for (i = 0; i < allocation_count; ++i)
        if (allocations[i] == pointer)
        {
            free(pointer);
            allocations[i] = NULL;
            return;
        }
    assert(!"untracked allocation");
}

uint32_t W_LumpLength(unsigned int lump)
{
    assert(lump < TEST_LUMPS);
    return test_lengths[lump];
}

void *W_CacheLumpNum(int lump, int tag)
{
    (void)tag;
    assert(lump >= 0 && lump < TEST_LUMPS);
    return test_lumps[lump];
}

void W_ReleaseLumpNum(int lump) { (void)lump; }
void W_ReadLump(unsigned int lump, void *dest)
{
    assert(lump < TEST_LUMPS);
    memcpy(dest, test_lumps[lump], test_lengths[lump]);
}

int R_FlatNumForName(char *name) { assert(strlen(name) <= 8); return 0; }
int R_TextureNumForName(char *name) { assert(strlen(name) <= 8); return 0; }
int M_CheckParm(char *name) { (void)name; return 0; }
boolean I_GetMemoryValue(unsigned int offset, void *value, int size)
{
    (void)offset;
    memset(value, 0, (size_t)size);
    return true;
}
void P_SpawnMapThing(mapthing_t *thing)
{
    (void)thing;
    ++spawned_things;
}

static void load_lumps(const char *path)
{
    FILE *file = fopen(path, "rb");
    byte header[12], entry[16];
    uint32_t count, directory, index;
    int found = 0, i;
    assert(file != NULL);
    assert(fread(header, 1, 12, file) == 12);
    count = DoomReadLE32(header + 4);
    directory = DoomReadLE32(header + 8);
    for (index = 0; index < count; ++index)
    {
        assert(fseek(file, directory + index * 16, SEEK_SET) == 0);
        assert(fread(entry, 1, 16, file) == 16);
        if (memcmp(entry + 8, "E1M1\0\0\0\0", 8) == 0)
        {
            found = 1;
            break;
        }
    }
    assert(found && index + TEST_LUMPS <= count);
    for (i = 0; i < TEST_LUMPS; ++i)
    {
        uint32_t offset;
        assert(fseek(file, directory + (index + i) * 16, SEEK_SET) == 0);
        assert(fread(entry, 1, 16, file) == 16);
        offset = DoomReadLE32(entry);
        test_lengths[i] = DoomReadLE32(entry + 4);
        test_lumps[i] = malloc(test_lengths[i] ? test_lengths[i] : 1);
        assert(test_lumps[i] != NULL);
        assert(fseek(file, offset, SEEK_SET) == 0);
        assert(fread(test_lumps[i], 1, test_lengths[i], file) == test_lengths[i]);
    }
    fclose(file);
}

static void expect_error(void (*load)(int), int lump)
{
    expecting_error = 1;
    if (setjmp(error_target) == 0)
    {
        load(lump);
        assert(!"invalid map input was accepted");
    }
    expecting_error = 0;
}

static void corrupt_word(void (*load)(int), int lump, unsigned offset, uint16_t value)
{
    byte low = test_lumps[lump][offset];
    byte high = test_lumps[lump][offset + 1];
    test_lumps[lump][offset] = value & 255;
    test_lumps[lump][offset + 1] = value >> 8;
    expect_error(load, lump);
    test_lumps[lump][offset] = low;
    test_lumps[lump][offset + 1] = high;
}

int main(int argc, char **argv)
{
    int i;
    byte signed_min[] = {0, 128};
    byte signed_max[] = {255, 127};
    byte minus_one[] = {255, 255};
    byte reject[20];
    assert(argc == 2);
    assert(DoomReadFixed16(signed_min) == INT32_MIN);
    assert(DoomReadFixed16(signed_max) == INT32_C(2147418112));
    assert(DoomReadFixed16(minus_one) == -INT32_C(65536));
    load_lumps(argv[1]);
    P_LoadVertexes(ML_VERTEXES);
    P_LoadSectors(ML_SECTORS);
    P_LoadSideDefs(ML_SIDEDEFS);
    P_LoadLineDefs(ML_LINEDEFS);
    P_LoadBlockMap(ML_BLOCKMAP);
    P_LoadSegs(ML_SEGS);
    P_LoadSubsectors(ML_SSECTORS);
    P_LoadNodes(ML_NODES);
    P_LoadReject(ML_REJECT);
    P_LoadThings(ML_THINGS);
    assert(numvertexes == 467 && numsectors == 85 && numsides == 648);
    assert(numlines == 475 && numsegs == 732 && numsubsectors == 237);
    assert(numnodes == 236 && spawned_things == 138);
    assert(vertexes[0].x == DoomReadFixed16(test_lumps[ML_VERTEXES]));
    assert(vertexes[0].y == DoomReadFixed16(test_lumps[ML_VERTEXES] + 2));
    assert(rejectmatrix == test_lumps[ML_REJECT]);
    totallines = 1;
    PadRejectArray(reject, 20);
    assert(reject[0] == 28 && reject[8] == 50);
    assert(reject[12] == 0x11 && reject[13] == 0x4a && reject[14] == 0x1d);
    assert(reject[16] == 0 && reject[19] == 0);

    --test_lengths[ML_VERTEXES];
    expect_error(P_LoadVertexes, ML_VERTEXES);
    ++test_lengths[ML_VERTEXES];
    corrupt_word(P_LoadSideDefs, ML_SIDEDEFS, 28, 0xffff);
    corrupt_word(P_LoadLineDefs, ML_LINEDEFS, 0, 0xffff);
    corrupt_word(P_LoadSegs, ML_SEGS, 8, 2);
    corrupt_word(P_LoadSubsectors, ML_SSECTORS, 0, 0xffff);
    corrupt_word(P_LoadNodes, ML_NODES, 24, 0); /* self-reference at node zero */
    corrupt_word(P_LoadBlockMap, ML_BLOCKMAP, 4, 0);
    --test_lengths[ML_THINGS];
    expect_error(P_LoadThings, ML_THINGS);
    ++test_lengths[ML_THINGS];
    assert(rejected_inputs == 8);
    for (i = 0; i < allocation_count; ++i)
        free(allocations[i]);
    for (i = 0; i < TEST_LUMPS; ++i)
        free(test_lumps[i]);
    puts("map decode: E1M1 loaded; 8 malformed inputs rejected");
    return 0;
}
