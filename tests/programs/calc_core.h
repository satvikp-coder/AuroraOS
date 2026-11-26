/* tests/programs/calc_core.h - the Calculator's arithmetic as a pure, deterministic, bounded state machine (no libc, no allocation, no floating point).
 * Used by the EL0 application (tests/programs/calc.c) and, byte for byte, by the host test (tests/kernel-host/calc_test.c).
 *  - Integers only: at most CALC_DIGITS (9) digits typed per number; results are limited to +-999,999,999,999 (12 digits); anything beyond is the "Overflow" error. Division truncates toward
 *    zero and dividing by zero is the "Divide by zero" error. Nothing wraps, nothing is undefined.
 *  - An error state ignores everything except Clear.
 *  - Operators apply left to right (a calculator, not an expression parser): 2 + 3 * 4 = 20. */
#ifndef AURORA_CALC_CORE_H
#define AURORA_CALC_CORE_H
typedef long long cc_i64;
#define CALC_DIGITS 9
#define CALC_LIMIT 999999999999LL
enum { CC_OK = 0, CC_DIV0 = 1, CC_OVERFLOW = 2 };

struct calc { cc_i64 acc, entry; int digits, have_entry, after_equals, err; char op; };

static inline void calc_clear(struct calc *c) { c->acc = 0; c->entry = 0; c->digits = 0; c->have_entry = 0; c->after_equals = 0; c->err = CC_OK; c->op = 0; }

static inline int calc_apply(cc_i64 a, char op, cc_i64 b, cc_i64 *r)
{
    cc_i64 v;
    switch (op) {
    case '+': v = a + b; break;                                     /* operands are within +-1e12: no 64-bit overflow is possible */
    case '-': v = a - b; break;
    case '*': if (__builtin_mul_overflow(a, b, &v)) return CC_OVERFLOW; break;   /* the 64-bit product itself may overflow; the limit check below covers the rest */
    case '/': if (b == 0) return CC_DIV0; v = a / b; break;
    default: return CC_OVERFLOW;
    }
    if (v > CALC_LIMIT || v < -CALC_LIMIT) return CC_OVERFLOW;
    *r = v; return CC_OK;
}

static inline void calc_digit(struct calc *c, int d)
{
    if (c->err || d < 0 || d > 9) return;
    if (c->after_equals) { c->acc = 0; c->op = 0; c->after_equals = 0; c->entry = 0; c->digits = 0; }
    if (c->digits >= CALC_DIGITS) return;                          /* bounded: the tenth digit is ignored */
    if (c->digits == 0 && d == 0) { c->have_entry = 1; return; }   /* no leading zeros */
    c->entry = c->entry * 10 + d; c->digits++; c->have_entry = 1;
}

static inline void calc_backspace(struct calc *c)
{
    if (c->err || c->after_equals || !c->have_entry) return;
    c->entry /= 10; if (c->digits > 0) c->digits--;
}

static inline void calc_op(struct calc *c, char op)
{
    if (c->err || (op != '+' && op != '-' && op != '*' && op != '/')) return;
    if (c->op && c->have_entry && !c->after_equals) {
        cc_i64 r; int e = calc_apply(c->acc, c->op, c->entry, &r);
        if (e) { c->err = e; return; }
        c->acc = r;
    } else if (c->have_entry || c->after_equals) {
        if (!c->after_equals) c->acc = c->entry;
    }
    c->after_equals = 0; c->op = op; c->entry = 0; c->digits = 0; c->have_entry = 0;
}

static inline void calc_equals(struct calc *c)
{
    if (c->err) return;
    if (c->op && c->have_entry) {
        cc_i64 r; int e = calc_apply(c->acc, c->op, c->entry, &r);
        if (e) { c->err = e; return; }
        c->acc = r; c->op = 0; c->entry = 0; c->digits = 0; c->have_entry = 0; c->after_equals = 1;
    } else if (!c->op && c->have_entry && !c->after_equals) {
        c->acc = c->entry; c->entry = 0; c->digits = 0; c->have_entry = 0; c->after_equals = 1;
    }
}

/* The text of the big display line: the number being typed, or the running value; errors are words. cap >= 24. */
static inline void calc_display(const struct calc *c, char *out, int cap)
{
    const char *msg = c->err == CC_DIV0 ? "Divide by zero" : c->err == CC_OVERFLOW ? "Overflow" : 0;
    int n = 0;
    if (msg) { while (msg[n] && n + 1 < cap) { out[n] = msg[n]; n++; } out[n] = 0; return; }
    cc_i64 v = (c->have_entry && !c->after_equals) ? c->entry : (c->op && !c->have_entry) ? c->acc : (c->have_entry ? c->entry : c->acc);
    int neg = v < 0; unsigned long long u = (unsigned long long)(neg ? -v : v);
    char d[24]; int k = 0; if (!u) d[k++] = '0'; while (u && k < 23) { d[k++] = (char)('0' + u % 10); u /= 10; }
    if (neg && n + 1 < cap) out[n++] = '-';
    while (k && n + 1 < cap) out[n++] = d[--k];
    out[n] = 0;
}

/* The small line above it: the pending operation, e.g. "12 +". */
static inline void calc_pending(const struct calc *c, char *out, int cap)
{
    int n = 0; out[0] = 0;
    if (c->err || !c->op) return;
    cc_i64 v = c->acc; int neg = v < 0; unsigned long long u = (unsigned long long)(neg ? -v : v);
    char d[24]; int k = 0; if (!u) d[k++] = '0'; while (u && k < 23) { d[k++] = (char)('0' + u % 10); u /= 10; }
    if (neg && n + 1 < cap) out[n++] = '-';
    while (k && n + 1 < cap) out[n++] = d[--k];
    if (n + 3 < cap) { out[n++] = ' '; out[n++] = c->op; out[n] = 0; }
}
/* Save only a completed result. CALC v1 stores a sign and five LE magnitude bytes with their complements. */
#define CALC_SAVE_SIZE 16
static inline int calc_encode_result(const struct calc *c, unsigned char *out)
{
    if (!c->after_equals || c->err || c->acc < -CALC_LIMIT || c->acc > CALC_LIMIT) return 0;
    out[0] = 'C'; out[1] = 'A'; out[2] = 'L'; out[3] = 'C'; out[4] = 1;
    out[5] = c->acc < 0 ? 0xff : 0;
    unsigned long long magnitude = (unsigned long long)(out[5] ? -c->acc : c->acc);
    for (int i = 0; i < 5; i++) {
        out[6 + i] = (unsigned char)(magnitude >> (8 * i));
        out[11 + i] = (unsigned char)~out[6 + i];
    }
    return 1;
}

static inline int calc_decode_result(struct calc *c, const unsigned char *in, int len)
{
    if (len != CALC_SAVE_SIZE || in[0] != 'C' || in[1] != 'A' || in[2] != 'L' ||
        in[3] != 'C' || in[4] != 1 || (in[5] != 0 && in[5] != 0xff)) return 0;
    unsigned long long magnitude = 0;
    for (int i = 0; i < 5; i++) {
        if ((unsigned)in[6 + i] + in[11 + i] != 255) return 0;
        magnitude |= (unsigned long long)in[6 + i] << (8 * i);
    }
    if (magnitude > CALC_LIMIT || (!magnitude && in[5])) return 0;
    calc_clear(c);
    c->acc = in[5] ? -(cc_i64)magnitude : (cc_i64)magnitude;
    c->after_equals = 1;
    return 1;
}
#endif
