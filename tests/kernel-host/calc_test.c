/* tests/kernel-host/calc_test.c - the Calculator's arithmetic (tests/programs/calc_core.h, the same header the EL0 app compiles): exact results, bounds, errors, left-to-right
 * evaluation, and a randomized run against an independent __int128 oracle. Host, ASan/UBSan. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../programs/calc_core.h"

static int passed, failed;
#define CHECK(name, cond) do { if (cond) passed++; else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)

static struct calc C;
static void num(const char *s) { for (; *s; s++) calc_digit(&C, *s - '0'); }
static const char *disp(void) { static char b[40]; calc_display(&C, b, sizeof b); return b; }
static int is(const char *s) { return strcmp(disp(), s) == 0; }

int main(void)
{
    printf("calculator tests\n");
    calc_clear(&C);
    CHECK("starts at 0", is("0"));
    num("12"); CHECK("typing shows the number", is("12"));
    calc_op(&C, '+'); CHECK("after an operator the running value shows", is("12"));
    { char t[32]; calc_pending(&C, t, sizeof t); CHECK("the pending line is '12 +'", strcmp(t, "12 +") == 0); }
    num("8"); CHECK("the second number shows while typed", is("8"));
    calc_equals(&C); CHECK("12 + 8 = 20", is("20"));

    calc_clear(&C); num("7"); calc_op(&C, '-'); num("10"); calc_equals(&C); CHECK("7 - 10 = -3", is("-3"));
    calc_clear(&C); num("6"); calc_op(&C, '*'); num("7"); calc_equals(&C); CHECK("6 * 7 = 42", is("42"));
    calc_clear(&C); num("7"); calc_op(&C, '/'); num("2"); calc_equals(&C); CHECK("7 / 2 = 3 (truncation toward zero)", is("3"));
    calc_clear(&C); num("7"); calc_op(&C, '-'); num("10"); calc_op(&C, '/'); num("2"); calc_equals(&C); CHECK("(7 - 10) / 2 = -1 (left to right, truncation toward zero)", is("-1"));
    calc_clear(&C); num("2"); calc_op(&C, '+'); num("3"); calc_op(&C, '*'); num("4"); calc_equals(&C); CHECK("2 + 3 * 4 = 20: strictly left to right, and it is documented", is("20"));
    calc_clear(&C); num("5"); calc_op(&C, '+'); num("5"); calc_op(&C, '+'); CHECK("chaining applies the pending operation: 5 + 5 + shows 10", is("10"));
    calc_clear(&C); num("9"); calc_op(&C, '/'); num("0"); calc_equals(&C); CHECK("division by zero is an error, never a crash", is("Divide by zero"));
    num("5"); calc_op(&C, '+'); calc_equals(&C); CHECK("the error state ignores everything but Clear", is("Divide by zero"));
    calc_clear(&C); CHECK("Clear recovers", is("0"));
    calc_clear(&C); num("999999999"); calc_op(&C, '*'); num("999999999"); calc_equals(&C); CHECK("a result beyond 12 digits is the Overflow error", is("Overflow"));
    calc_clear(&C); num("999999999"); calc_op(&C, '+'); num("999999999"); calc_op(&C, '*'); num("999"); calc_equals(&C); CHECK("a chained overflow is caught too", is("Overflow"));
    calc_clear(&C); num("1234567890"); CHECK("the tenth digit is ignored (bounded entry)", is("123456789"));
    calc_clear(&C); num("000"); CHECK("leading zeros collapse", is("0"));
    calc_clear(&C); num("123"); calc_backspace(&C); CHECK("backspace removes a digit", is("12"));
    calc_backspace(&C); calc_backspace(&C); calc_backspace(&C); CHECK("backspace past the start stays 0", is("0"));
    calc_clear(&C); num("2"); calc_op(&C, '+'); num("3"); calc_equals(&C); num("4"); CHECK("typing after = starts a new calculation", is("4"));
    calc_clear(&C); num("2"); calc_op(&C, '+'); num("3"); calc_equals(&C); calc_op(&C, '*'); num("5"); calc_equals(&C); CHECK("an operator after = continues from the result: (2+3)*5 = 25", is("25"));
    calc_clear(&C); calc_op(&C, '+'); num("5"); calc_equals(&C); CHECK("an operator with nothing typed starts from 0: + 5 = 5", is("5"));
    calc_clear(&C); calc_equals(&C); CHECK("= with nothing typed changes nothing", is("0"));
    calc_clear(&C); num("5"); calc_op(&C, '+'); calc_op(&C, '-'); num("2"); calc_equals(&C); CHECK("changing the operator before the second number: 5 - 2 = 3", is("3"));
    calc_clear(&C); calc_digit(&C, 12); calc_digit(&C, -1); calc_op(&C, 'x'); CHECK("invalid digits and operators are ignored", is("0") && C.op == 0);

    { /* randomized: every sequence of keys must match an independent oracle with the same documented semantics */
        unsigned s = 1; int bad = 0;
        for (int it = 0; it < 200000 && !bad; it++) {
            struct calc c; calc_clear(&c);
            __int128 acc = 0, ent = 0; int have = 0, aft = 0, err = 0, dig = 0; char op = 0;
            for (int k = 0; k < 12; k++) {
                s = s * 1664525u + 1013904223u; unsigned r = (s >> 8) % 20;
                if (r < 10) {                                                           /* a digit */
                    calc_digit(&c, (int)r);
                    if (!err) { if (aft) { acc = 0; op = 0; aft = 0; ent = 0; dig = 0; } if (dig < 9) { if (dig == 0 && r == 0) have = 1; else { ent = ent * 10 + (int)r; dig++; have = 1; } } }
                } else if (r < 14) {                                                    /* an operator */
                    char o = "+-*/"[r - 10]; calc_op(&c, o);
                    if (!err) {
                        if (op && have && !aft) { __int128 v; int e = 0;
                            if (op == '+') v = acc + ent; else if (op == '-') v = acc - ent; else if (op == '*') v = acc * ent; else if (ent == 0) { e = 1; v = 0; } else v = acc / ent;
                            if (!e && (v > CALC_LIMIT || v < -CALC_LIMIT)) e = 1;
                            if (e) { err = 1; } else acc = (cc_i64)v;
                        } else if (have || aft) { if (!aft) acc = ent; }
                        if (!err) { aft = 0; op = o; ent = 0; dig = 0; have = 0; }
                    }
                } else if (r < 17) {                                                    /* equals */
                    calc_equals(&c);
                    if (!err) {
                        if (op && have) { __int128 v; int e = 0;
                            if (op == '+') v = acc + ent; else if (op == '-') v = acc - ent; else if (op == '*') v = acc * ent; else if (ent == 0) { e = 1; v = 0; } else v = acc / ent;
                            if (!e && (v > CALC_LIMIT || v < -CALC_LIMIT)) e = 1;
                            if (e) err = 1; else { acc = (cc_i64)v; op = 0; ent = 0; dig = 0; have = 0; aft = 1; }
                        } else if (!op && have && !aft) { acc = ent; ent = 0; dig = 0; have = 0; aft = 1; }
                    }
                } else if (r < 19) { calc_backspace(&c); if (!err && !aft && have) { ent /= 10; if (dig > 0) dig--; } }
                else { calc_clear(&c); acc = ent = 0; have = aft = err = dig = 0; op = 0; }
                if ((c.err != 0) != (err != 0)) bad = 1;
                if (!err && (c.acc != (cc_i64)acc || c.entry != (cc_i64)ent || c.op != op || c.after_equals != aft || c.have_entry != have)) bad = 2;
                if (!err && (c.acc > CALC_LIMIT || c.acc < -CALC_LIMIT)) bad = 3;
            }
        }
        if (bad) printf("    random run diverged: %d\n", bad);
        CHECK("200,000 random key sequences match the independent oracle and never leave the bounds", bad == 0);
    }
    printf("calculator: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
