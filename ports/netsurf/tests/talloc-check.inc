#include "utils/talloc.h"
static void check_talloc(void)
{
    void *root = talloc_new(NULL), *other = talloc_new(NULL);
    char *data;
    check(root != NULL && other != NULL, "talloc roots and 32-bit magic");
    if (root == NULL || other == NULL) return;
    data = talloc_size(root, 70000UL);
    check(data != NULL && talloc_get_size(data) == 70000UL, "talloc bank-spanning allocation");
    if (data == NULL) { talloc_free(root); talloc_free(other); return; }
    data[0] = 'a'; data[63999UL] = 'b'; data[69999UL] = 'c';
    check(talloc_parent(data) == root && talloc_total_size(root) == 70000UL,
          "talloc parent and subtree size");
    data = talloc_realloc_size(root, data, 72000UL);
    check(data && data[0] == 'a' && data[63999UL] == 'b' && data[69999UL] == 'c',
          "talloc resize preserves bank-spanning data");
    if (data == NULL) { talloc_free(root); talloc_free(other); return; }
    check(talloc_steal(other, data) == data && talloc_parent(data) == other && talloc_total_size(root) == 0,
          "talloc ownership transfer");
    check(talloc_reference(root, data) == data && talloc_reference_count(data) == 1,
          "talloc secondary reference");
    check(talloc_free(root) == 0 && talloc_reference_count(data) == 0 && data[69999UL] == 'c',
          "talloc reference cleanup");
    check(talloc_free(other) == 0, "talloc recursive destruction");
}
