/* Leaf arithmetic helpers. Private ABI: T0..T3 = lhs/result, T4..T7 = rhs.
 * All other caller-saved registers may be destroyed; no helper nests a call.
 * Arithmetic uses unsigned word operations, including signed division via
 * magnitudes, so minimum signed integers never invoke C signed overflow.
 */
#include <stdio.h>
#include <string.h>
#include "ripple.h"
const char *binary_ops[24] = {"add", "sub", "mul", "sdiv", "udiv", "srem", "urem", "and",
                              "or",  "xor", "shl", "lshr", "ashr", "eq",   "ne",   "slt",
                              "sle", "sgt", "sge", "ult",  "ule",  "ugt",  "uge",  "unused"};
static FILE *out;
static const char *prefix;
static unsigned long serial;
#define P(...) fprintf(out, __VA_ARGS__)

static void negate(unsigned base, int n)
{
    int i;
    P("    LI X0, 1\n");
    for (i = 0; i < n; i++)
        P("    XORI R%u, R%u, 65535\n    ADD R%u, R%u, X0\n    SLTU X0, R%u, X0\n", base + i,
          base + i, base + i, base + i, base + i);
}

static void addition(unsigned a, unsigned b, int n, bool subtract)
{
    int i;
    P("    LI X0, 0\n");
    for (i = 0; i < n; i++) {
        if (subtract)
            P("    SLTU X1, R%u, R%u\n    SUB R%u, R%u, R%u\n    SLTU X2, R%u, X0\n    SUB R%u, "
              "R%u, X0\n    OR X0, X1, X2\n",
              a + i, b + i, a + i, a + i, b + i, a + i, a + i, a + i);
        else
            P("    ADD X1, R%u, R%u\n    SLTU X2, X1, R%u\n    ADD R%u, X1, X0\n    SLTU X1, R%u, "
              "X0\n    OR X0, X1, X2\n",
              a + i, b + i, a + i, a + i, a + i);
    }
}

static void left(unsigned base, int n, const char *carry)
{
    int i;
    P("    LI X0, 0\n");
    for (i = 0; i < n; i++)
        P("    SRLI X1, R%u, 15\n    SLLI R%u, R%u, 1\n    OR R%u, R%u, X0\n    ADD X0, X1, R0\n",
          base + i, base + i, base + i, base + i, base + i);
    if (carry)
        P("    ADD %s, X0, R0\n", carry);
}

static void right(unsigned base, int n, bool arithmetic)
{
    int i;
    if (arithmetic)
        P("    ANDI X0, R%u, 32768\n", base + n - 1);
    else
        P("    LI X0, 0\n");
    for (i = n - 1; i >= 0; i--)
        P("    SLLI X1, R%u, 15\n    SRLI R%u, R%u, 1\n    OR R%u, R%u, X0\n    ADD X0, X1, R0\n",
          base + i, base + i, base + i, base + i, base + i);
}

static void compare(int n, bool sign)
{
    int i;
    unsigned long id = ++serial;
    P("    LI A0, 0\n");
    for (i = n - 1; i >= 0; i--)
        P("    BEQ T%d, T%d, %s_c%lu_%d\n    %s A0, T%d, T%d\n    BEQ R0, R0, "
          "%s_c%lu_done\n%s_c%lu_%d:\n",
          i, 4 + i, prefix, id, i, sign && i == n - 1 ? "SLT" : "SLTU", i, 4 + i, prefix, id,
          prefix, id, i);
    P("%s_c%lu_done:\n", prefix, id);
}

static void multiply(int n)
{
    int i;
    unsigned long id = ++serial;
    for (i = 0; i < n; i++)
        P("    LI A%d, 0\n", i);
    P("    LI RV0, %d\n%s_multiply%lu:\n    ANDI RV1, T4, 1\n    BEQ RV1, R0, "
      "%s_multiply%lu_skip\n",
      n * 16, prefix, id, prefix, id);
    addition(7, 15, n, false);
    P("%s_multiply%lu_skip:\n", prefix, id);
    left(15, n, NULL);
    right(19, n, false);
    P("    ADDI RV0, RV0, -1\n    BNE RV0, R0, %s_multiply%lu\n", prefix, id);
    for (i = 0; i < n; i++)
        P("    ADD T%d, A%d, R0\n", i, i);
}

static void division(int n, bool sign, bool remainder)
{
    int i;
    unsigned long id = ++serial;
    /* dividend/quotient in T0.., remainder in A0.., divisor in T4... */
    P("    LI RV0, 0\n");
    for (i = 0; i < n; i++)
        P("    OR RV0, RV0, T%d\n", 4 + i);
    P("    BNE RV0, R0, %s_div%lu_nonzero\n    BRK\n%s_div%lu_nonzero:\n", prefix, id, prefix, id);
    if (sign) {
        P("    SRLI RV1, T%d, 15\n    SRLI SC, T%d, 15\n    XOR SC, SC, RV1\n    BEQ RV1, R0, "
          "%s_div%lu_positive_a\n",
          n - 1, n + 3, prefix, id);
        negate(15, n);
        P("%s_div%lu_positive_a:\n    SRLI X3, T%d, 15\n    BEQ X3, R0, %s_div%lu_positive_b\n",
          prefix, id, n + 3, prefix, id);
        negate(19, n);
        P("%s_div%lu_positive_b:\n", prefix, id);
    }
    for (i = 0; i < n; i++)
        P("    LI A%d, 0\n", i);
    P("    LI RV0, %d\n%s_div%lu_loop:\n", n * 16, prefix, id);
    left(15, n, "X3");
    /* Shift remainder and retain its overflow bit in X3 (n+1 bit trial). */
    for (i = 0; i < n; i++)
        P("    SRLI X1, A%d, 15\n    SLLI A%d, A%d, 1\n    OR A%d, A%d, X3\n    ADD X3, X1, R0\n",
          i, i, i, i, i);
    P("    BNE X3, R0, %s_div%lu_sub\n", prefix, id);
    for (i = n - 1; i >= 0; i--)
        P("    BEQ A%d, T%d, %s_div%lu_cmp%d\n    SLTU X1, A%d, T%d\n    BNE X1, R0, "
          "%s_div%lu_skip\n    BEQ R0, R0, %s_div%lu_sub\n%s_div%lu_cmp%d:\n",
          i, i + 4, prefix, id, i, i, i + 4, prefix, id, prefix, id, prefix, id, i);
    P("%s_div%lu_sub:\n", prefix, id);
    addition(7, 19, n, true);
    P("    ORI T0, T0, 1\n%s_div%lu_skip:\n    ADDI RV0, RV0, -1\n    BNE RV0, R0, "
      "%s_div%lu_loop\n",
      prefix, id, prefix, id);
    if (remainder)
        for (i = 0; i < n; i++)
            P("    ADD T%d, A%d, R0\n", i, i);
    if (sign) {
        P("    BEQ %s, R0, %s_div%lu_done\n", remainder ? "RV1" : "SC", prefix, id);
        negate(15, n);
        P("%s_div%lu_done:\n", prefix, id);
    }
}

void emit_helpers(FILE *file, const char *unit, bool used[24][5])
{
    int op, n, i;
    out = file;
    prefix = unit;
    serial = 0;
    for (op = 0; op < 23; op++)
        for (n = 1; n <= 4; n *= 2)
            if (used[op][n]) {
                P("%s_%s%d:\n", prefix, binary_ops[op], n);
                if (op == 0 || op == 1)
                    addition(15, 19, n, op == 1);
                else if (op == 2)
                    multiply(n);
                else if (op >= 3 && op <= 6)
                    division(n, op == 3 || op == 5, op == 5 || op == 6);
                else if (op >= 7 && op <= 9) {
                    const char *asmop = op == 7 ? "AND" : op == 8 ? "OR" : "XOR";
                    for (i = 0; i < n; i++)
                        P("    %s T%d, T%d, T%d\n", asmop, i, i, i + 4);
                } else if (op >= 10 && op <= 12) {
                    unsigned long id = ++serial;
                    P("    ADD RV0, T4, R0\n    LI X0, %d\n    SLTU X0, RV0, X0\n    BNE X0, R0, "
                      "%s_shift%lu_valid\n    BRK\n%s_shift%lu_valid:\n    BEQ RV0, R0, "
                      "%s_shift%lu_done\n%s_shift%lu_loop:\n",
                      n * 16, prefix, id, prefix, id, prefix, id, prefix, id);
                    if (op == 10)
                        left(15, n, NULL);
                    else
                        right(15, n, op == 12);
                    P("    ADDI RV0, RV0, -1\n    BNE RV0, R0, "
                      "%s_shift%lu_loop\n%s_shift%lu_done:\n",
                      prefix, id, prefix, id);
                } else if (op == 13 || op == 14) {
                    P("    LI X0, 0\n");
                    for (i = 0; i < n; i++)
                        P("    XOR X1, T%d, T%d\n    OR X0, X0, X1\n", i, i + 4);
                    P("    SLTU T0, R0, X0\n");
                    if (op == 13)
                        P("    XORI T0, T0, 1\n");
                } else {
                    bool invert = op == 16 || op == 18 || op == 20 || op == 22;
                    bool swap = op == 16 || op == 17 || op == 20 || op == 21;
                    if (swap)
                        for (i = 0; i < n; i++)
                            P("    ADD X0, T%d, R0\n    ADD T%d, T%d, R0\n    ADD T%d, X0, R0\n", i,
                              i, i + 4, i + 4);
                    compare(n, op >= 15 && op <= 18);
                    P("    ADD T0, A0, R0\n");
                    if (invert)
                        P("    XORI T0, T0, 1\n");
                }
                P("    RET\n");
            }
}
