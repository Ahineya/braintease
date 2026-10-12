/* Exercise the prepared renderer's real lookup/composite routines. Short
 * composite columns need all 128 rows addressed by the wall sampler, even
 * when projection rounding runs one row past their declared texture height. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "texture_cache.inc"

static patch_t *test_patches[2];
static size_t composite_allocation;
static byte *composite_payload;
static void check(int good)
{
    /* Stop before a failed size/offset check could make a later read unsafe. */
    if (!good) {
        puts("TEXTURE FAIL");
        exit(1);
    }
}

void I_Error(char *format, ...)
{
    printf("TEXTURE ERROR %s\n", format);
    exit(1);
}

void *Z_Malloc(size_t size, int tag, void *owner)
{
    byte *result = malloc(size + 2);
    (void)tag;
    if (result == NULL) exit(1);
    memset(result, 211, size + 2);
    if (owner == &texturecomposite[0]) {
        composite_allocation = size;
        composite_payload = result;
    }
    if (owner != NULL) *(void **)owner = result;
    return result;
}

void Z_Free(void *pointer)
{
    free(pointer);
}

void Z_ChangeTag2(void *pointer, int tag, char *file, int line)
{
    (void)pointer; (void)tag; (void)file; (void)line;
}

patch_t *W_CachePatchNum(int lump, int tag)
{
    (void)tag;
    check(lump == 0 || lump == 1);
    return test_patches[lump];
}

static void make_patch(int which, int width, int height, int top)
{
    int x, y;
    size_t header = offsetof(patch_t, columnofs)
                  + (size_t)(width < 8 ? 8 : width) * sizeof(int32_t);
    size_t stride = (size_t)(height - top) + 5;
    patch_t *patch = malloc(header + (size_t)width * stride);
    if (patch == NULL) exit(1);
    test_patches[which] = patch;
    patch->width = width;
    patch->height = height;
    patch->leftoffset = patch->topoffset = 0;
    for (x = 0; x < width; ++x) {
        byte *post = (byte *)patch + header + (size_t)x * stride;
        patch->columnofs[x] = (int32_t)(header + (size_t)x * stride);
        post[0] = top;
        post[1] = height - top;
        post[2] = 0;
        for (y = top; y < height; ++y)
            post[3 + y - top] = (byte)((x * 3 + y + which * 17) & 255);
        post[3 + height - top] = 0;
        post[4 + height - top] = 255;
    }
}

static void run_case(int height, int composite, int top)
{
    int x, y, width = 32;
    int rows = height < 128 ? 128 : height;
    texture_t *texture = malloc(sizeof(texture_t) + sizeof(texpatch_t));
    short lumps[32];
    uint32_t offsets[32];
    size_t sizes[1];
    short *lump_lists[1];
    uint32_t *offset_lists[1];
    byte *images[1];
    texture_t *texture_list[1];
    int masks[1];
    if (texture == NULL) exit(1);
    texture_list[0] = texture;
    textures = texture_list;
    lump_lists[0] = lumps;
    offset_lists[0] = offsets;
    texturecolumnlump = lump_lists;
    texturecolumnofs = offset_lists;
    texturecompositesize = sizes;
    texturecomposite = images;
    masks[0] = width - 1;
    texturewidthmask = masks;
    texture->width = width;
    texture->height = height;
    texture->patchcount = composite ? 2 : 1;
    for (x = 0; x < 2; ++x) {
        texture->patches[x].originx = texture->patches[x].originy = 0;
        texture->patches[x].patch = x;
        make_patch(x, width, height, top);
    }
    R_GenerateLookup(0);
    if (composite) {
        check(sizes[0] == (size_t)width * rows);
        R_GenerateComposite(0);
        check(composite_allocation == (size_t)width * rows);
        for (x = 0; x < width; ++x) {
            byte *column = R_GetColumn(0, x);
            check(lumps[x] == -1 && offsets[x] == (uint32_t)x * rows);
            check(column == images[0] + offsets[x]);
            for (y = 0; y < rows; ++y) {
                int source_y = y % height;
                int expected = source_y < top ? 0 : ((x * 3 + source_y + 17) & 255);
                check(column[y] == expected);
            }
        }
        check(composite_payload[composite_allocation] == 211
              && composite_payload[composite_allocation + 1] == 211);
        free(images[0]);
    } else {
        /* Single patch/masked columns retain their encoded posts and offsets. */
        check(sizes[0] == 0);
        for (x = 0; x < width; ++x) {
            byte *column = R_GetColumn(0, x);
            check(lumps[x] == 0);
            check(column == (byte *)test_patches[0] + test_patches[0]->columnofs[x] + 3);
            check(column[-3] == top && column[-2] == height - top);
            check(column[height - top + 1] == 255);
        }
    }
    free(test_patches[0]);
    free(test_patches[1]);
    free(texture);
}

int main(void)
{
    run_case(3, 1, 0);
    run_case(16, 1, 0);
    run_case(63, 1, 0);
    run_case(127, 1, 0);
    run_case(128, 1, 0);
    run_case(200, 1, 0);
    run_case(16, 1, 3);
    run_case(16, 0, 3);
    run_case(200, 0, 3);
    puts("TEXTURE OK: short/tall composites, last-column guard, holes, masked posts");
    return 0;
}
