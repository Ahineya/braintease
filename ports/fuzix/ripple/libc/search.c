/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <errno.h>
#include <stdlib.h>

void *bsearch(void *key, void *base, size_t count, size_t width, cmp_func_t compare) {
    size_t low = 0, high = count;
    if (!count) return NULL;
    if (!width || count > ~0UL / width) { errno = EINVAL; return NULL; }
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        char *element = (char *)base + middle * width;
        int order = compare(key, element);
        if (!order) return element;
        if (order < 0) high = middle;
        else low = middle + 1;
    }
    return NULL;
}
