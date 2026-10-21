/* Reuse the real kernel exec path with a small, prelinked command set. */
extern void riplix_cat_entry(unsigned long);
extern void riplix_ls_entry(unsigned long);
extern void riplix_pipecheck_entry(unsigned long);
#define FUSE_NATIVE_EXTRA_PROGRAMS \
    {"FXN1:cat:1\n", riplix_cat_entry}, \
    {"FXN1:ls:1\n", riplix_ls_entry}, \
    {"FXN1:pipecheck:1\n", riplix_pipecheck_entry}
#include "../../../fuse-x/exec.c"
