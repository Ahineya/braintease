/* C16 target properties. Sizes and alignments are in guest cells. */
#include <stdbool.h>
#include <string.h>
#include "util.h"
#include "cc.h"

static struct type valist = {
    .kind = TYPEPOINTER, .prop = PROPSCALAR, .size = 2, .align = 1, .base = &typevoid};
static const struct target c16 = {
    .name = "ripple16", .typevalist = &valist, .typewchar = &typeuint, .signedchar = 1};
const struct target *targ;

void targinit(const char *name)
{
    enum typequal qual = QUALNONE;
    if (name && strcmp(name, "ripple16"))
        fatal("unsupported frontend target '%s'", name);
    targ = &c16;
    typechar.u.arith.issigned = true;
    typeadjvalist = typeadjust(targ->typevalist, &qual);
}
