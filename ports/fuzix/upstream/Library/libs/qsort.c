/*
 * This file lifted in toto from 'Dlibs' on the atari ST  (RdeBath)
 *
 * 
 *    Dale Schumacher                         399 Beacon Ave.
 *    (alias: Dalnefre')                      St. Paul, MN  55104
 *    dal@syntel.UUCP                         United States of America
 *  "It's not reality that's important, but how you perceive things."
 */

/*
 * Sun Feb  8 21:02:15 EST 1998 claudio@pos.inf.ufpr.br (Claudio Matsuoka)
 * Changed sort direction
 */

/* SPDX-License-Identifier: LGPL-2.1-only */
/* C16 heapsort: size_t indices and constant auxiliary storage.
 * No shared pivot buffer: comparators may call qsort recursively. */
#include <stdlib.h>

static void exchange(char *a, char *b, size_t width) {
  while (width--) {
    char cell = *a;
    *a++ = *b;
    *b++ = cell;
  }
}

static void sift(char *base, size_t root, size_t count, size_t width,
                 int (*compare)(const void *, const void *)) {
  while (root < count / 2) {
    size_t child = root * 2 + 1;
    if (child + 1 < count &&
        compare(base + child * width, base + (child + 1) * width) < 0)
      child++;
    if (compare(base + root * width, base + child * width) >= 0)
      return;
    exchange(base + root * width, base + child * width, width);
    root = child;
  }
}

void qsort(void *data, size_t count, size_t width,
           int (*compare)(const void *, const void *)) {
  char *base = data;
  size_t i;
  if (count < 2)
    return;
  if (!width || count > (size_t)-1 / width)
    return;
  for (i = count / 2; i; i--)
    sift(base, i - 1, count, width, compare);
  for (i = count - 1; i; i--) {
    exchange(base, base + i * width, width);
    sift(base, 0, i, width, compare);
  }
}
