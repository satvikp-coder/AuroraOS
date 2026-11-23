/*
 * tests/programs/modelclient.c - EL0 client of SYS_MODEL (11) for tests/qemu/model-tests.sh, launched by the AURORA_MODEL_TEST kernel hook
 * (kernel/modeltest.c), which chooses the principal and passes the mode in x0:
 *   1 FULL     (PRINCIPAL_INFER, valid volume) count, INFO of every object, every object read whole in 4096-byte chunks and compared with the
 *              synthetic content, then hostile calls with their exact AE_* codes
 *   2 DENIED   (an ordinary application principal) every op, valid or hostile, is AE_EPERM
 *   3 NOVOL    (PRINCIPAL_INFER, no volume or one that failed its boot check) every op is AE_ENOSYS
 *   4 CRASH    (PRINCIPAL_INFER) reads one chunk of object 0, then faults
 *   5 QUIT     (PRINCIPAL_INFER) reads one chunk of object 0, then exits 7 in the middle of the object
 * The image is built by tests/tools/mkmodel from three files whose bytes are byte(k, i) = (i*131 + k*17 + (i >> 9)) & 0xFF, k = object index.
 * Exit 0 means every check in the mode held. Output goes through SYS_WRITE ("[user pN] " prefix added by the kernel).
 */
#include "common.h"
#define SYS_MODEL 11
#define INFO 1
#define READ 2
#define COUNT 0xFFFFFFFFL
typedef unsigned char u8;
#define EINVAL_ (-1)
#define EFAULT_ (-2)
#define E2BIG_ (-3)
#define ENOSYS_ (-4)
#define EPERM_ (-7)

struct obj { u64 size; u32 kind, reserved; char name[32]; u8 digest[32]; };
static const u64 SIZES[3] = { 10000, 700, 512 };
static const char *const NAMES[3] = { "weights.bin", "tokenizer.json", "config" };

static i64 mc(i64 op, i64 a, i64 b, i64 c, i64 d) { return sc(SYS_MODEL, op, a, b, c, d); }
static u8 want(u64 k, u64 i) { return (u8)((i * 131u + k * 17u + (i >> 9)) & 0xFF); }

static u8 buf[4096 + 64];
static struct obj o;
static int fails;
static void ok_(const char *what, int cond)             /* ONE SYS_WRITE per line: the kernel tags each write as a line */
{
    char b[200]; u64 n = 0; const char *p = cond ? "mc: PASS " : "mc: FAIL ";
    while (*p) b[n++] = *p++;
    while (*what && n < 190) b[n++] = *what++;
    b[n] = 0; say(b);
    if (!cond) fails++;
}
static int streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static int same(u64 k, u64 off, const u8 *p, u64 n) { for (u64 i = 0; i < n; i++) if (p[i] != want(k, off + i)) return 0; return 1; }
static void say_digest(u64 k, const u8 *d)
{
    char b[80]; u64 n = 0; const char *h = "0123456789abcdef", *p = "mc: digest ";
    while (*p) b[n++] = *p++;
    b[n++] = (char)('0' + k); b[n++] = ' ';
    for (int i = 0; i < 32; i++) { b[n++] = h[d[i] >> 4]; b[n++] = h[d[i] & 15]; }
    b[n] = 0; say(b);
}

#define UNMAPPED   0x4000100000L                 /* inside the user window, never mapped */
#define KERNEL_VA  ((i64)0xFFFF000040080000ULL)
#define KERNEL_RAM 0x40080000L

/* Every op the client knows, valid and hostile; each must return `code` (AE_EPERM or AE_ENOSYS). */
static void all_refused(i64 code, const char *tag)
{
    i64 r[16]; int n = 0;
    r[n++] = mc(INFO, COUNT, 0, 0, 0);
    r[n++] = mc(INFO, 0, (i64)&o, 0, 0);
    r[n++] = mc(INFO, 2, (i64)&o, 0, 0);
    r[n++] = mc(READ, 0, 0, (i64)buf, 4096);
    r[n++] = mc(READ, 1, 100, (i64)buf, 1);
    r[n++] = mc(READ, 0, -1, (i64)buf, 4096);
    r[n++] = mc(READ, 0, 0, (i64)buf, 0);
    r[n++] = mc(READ, 0, 0, (i64)buf, 4097);
    r[n++] = mc(READ, 99, 0, KERNEL_VA, 4096);
    r[n++] = mc(0, 0, 0, 0, 0);
    r[n++] = mc(3, 0, 0, 0, 0);
    r[n++] = mc(-1, -1, -1, -1, -1);
    int good = 1; for (int i = 0; i < n; i++) if (r[i] != code) good = 0;
    for (u64 i = 0; i < sizeof buf; i++) if (buf[i]) good = 0;          /* nothing was ever written into the buffer */
    for (u64 i = 0; i < sizeof o; i++) if (((u8 *)&o)[i]) good = 0;
    ok_(tag, good);
}

static void full(void)
{
    i64 cnt = mc(INFO, COUNT, 0, 0, 0);
    ok_("count: INFO(0xFFFFFFFF) returns 3", cnt == 3);
    ok_("count: the pointer is ignored for the count (kernel pointer given)", mc(INFO, COUNT, KERNEL_VA, 0, 0) == 3);
    u8 dg[3][32];
    for (u64 k = 0; k < 3; k++) {
        for (u64 i = 0; i < sizeof o; i++) ((u8 *)&o)[i] = 0xEE;
        i64 r = mc(INFO, (i64)k, (i64)&o, 0, 0);
        int nz = 0; for (int i = 0; i < 32; i++) { dg[k][i] = o.digest[i]; nz |= o.digest[i]; }
        ok_(k == 0 ? "info 0: weights.bin, 10000 bytes, kind 0, reserved 0, non-zero digest" : k == 1 ? "info 1: tokenizer.json, 700 bytes, kind 0, reserved 0, non-zero digest"
                   : "info 2: config, 512 bytes, kind 0, reserved 0, non-zero digest",
            r == 0 && o.size == SIZES[k] && o.kind == 0 && o.reserved == 0 && streq(o.name, NAMES[k]) && nz);
        say_digest(k, o.digest);
    }
    { int d = 0; for (int i = 0; i < 32; i++) d |= (dg[0][i] != dg[1][i]) | ((dg[1][i] != dg[2][i]) << 1); ok_("info: the three digests differ", d == 3); }

    for (u64 k = 0; k < 3; k++) {                    /* every object, whole, in 4096-byte chunks */
        u64 off = 0; int good = 1, chunks = 0;
        for (;;) {
            for (u64 i = 0; i < 4096; i++) buf[i] = 0xA5;
            i64 r = mc(READ, (i64)k, (i64)off, (i64)buf, 4096);
            if (r == 0) break;
            u64 exp = SIZES[k] - off < 4096 ? SIZES[k] - off : 4096;
            if (r != (i64)exp || !same(k, off, buf, exp)) { good = 0; break; }
            for (u64 i = exp; i < 4096; i++) if (buf[i] != 0xA5) good = 0;      /* never a byte past the returned count */
            off += exp; chunks++;
            if (chunks > 10) { good = 0; break; }
        }
        ok_(k == 0 ? "read: weights.bin whole in 4096-byte chunks (4096+4096+1808, then 0 at EOF), every byte right"
            : k == 1 ? "read: tokenizer.json whole (700, then 0 at EOF), every byte right" : "read: config whole (exactly one sector, then 0), every byte right",
            good && off == SIZES[k] && chunks == (k == 0 ? 3 : 1));
    }
    { i64 r = mc(READ, 0, 500, (i64)buf, 1000); ok_("read: offset 500 length 1000 (crosses two sector boundaries) -> 1000 right bytes", r == 1000 && same(0, 500, buf, 1000)); }
    { i64 r = mc(READ, 0, 9990, (i64)buf, 4096); ok_("read: 10 bytes before the end with length 4096 -> clamped to 10", r == 10 && same(0, 9990, buf, 10)); }
    { i64 r = mc(READ, 1, 699, (i64)buf + 4095, 1); ok_("read: the last byte, into the last byte of the buffer -> 1", r == 1 && buf[4095] == want(1, 699)); }
    ok_("read: offset == size -> 0 (EOF)", mc(READ, 2, 512, (i64)buf, 4096) == 0);
    ok_("read: offset == size with a kernel buffer -> 0 (nothing to copy, nothing touched)", mc(READ, 2, 512, KERNEL_VA, 4096) == 0);
    ok_("hostile: offset size+1 -> EINVAL", mc(READ, 2, 513, (i64)buf, 1) == EINVAL_);
    ok_("hostile: offset 2^64-1 -> EINVAL", mc(READ, 0, -1, (i64)buf, 4096) == EINVAL_);
    ok_("hostile: offset 2^63 -> EINVAL", mc(READ, 0, (i64)0x8000000000000000ULL, (i64)buf, 1) == EINVAL_);
    ok_("hostile: length 0 -> EINVAL", mc(READ, 0, 0, (i64)buf, 0) == EINVAL_);
    ok_("hostile: length 4097 -> E2BIG", mc(READ, 0, 0, (i64)buf, 4097) == E2BIG_);
    ok_("hostile: length 2^64-1 -> E2BIG", mc(READ, 0, 0, (i64)buf, -1) == E2BIG_);
    ok_("hostile: offset 2^64-1 and length 2^64-1 -> E2BIG (length checked first)", mc(READ, 0, -1, (i64)buf, -1) == E2BIG_);
    ok_("hostile: read index 3 (one past the last) -> EINVAL", mc(READ, 3, 0, (i64)buf, 1) == EINVAL_);
    ok_("hostile: read index 2^32 (would be 0 if truncated) -> EINVAL", mc(READ, 0x100000000L, 0, (i64)buf, 1) == EINVAL_);
    ok_("hostile: read index 0xFFFFFFFF -> EINVAL (the count sentinel is INFO only)", mc(READ, COUNT, 0, (i64)buf, 1) == EINVAL_);
    ok_("hostile: read index 2^64-1 -> EINVAL", mc(READ, -1, 0, (i64)buf, 1) == EINVAL_);
    ok_("hostile: info index 3 -> EINVAL", mc(INFO, 3, (i64)&o, 0, 0) == EINVAL_);
    ok_("hostile: info index 2^32 -> EINVAL", mc(INFO, 0x100000000L, (i64)&o, 0, 0) == EINVAL_);
    ok_("hostile: info index 0xFFFFFFFE -> EINVAL", mc(INFO, 0xFFFFFFFEL, (i64)&o, 0, 0) == EINVAL_);
    ok_("hostile: info index 2^64-1 -> EINVAL (only exactly 0xFFFFFFFF is the count)", mc(INFO, -1, (i64)&o, 0, 0) == EINVAL_);
    ok_("hostile: info into NULL -> EFAULT", mc(INFO, 0, 0, 0, 0) == EFAULT_);
    ok_("hostile: info into an unmapped user page -> EFAULT", mc(INFO, 0, UNMAPPED, 0, 0) == EFAULT_);
    ok_("hostile: info into a kernel address -> EFAULT", mc(INFO, 0, KERNEL_VA, 0, 0) == EFAULT_);
    ok_("hostile: info into the kernel's identity-mapped RAM -> EFAULT", mc(INFO, 0, KERNEL_RAM, 0, 0) == EFAULT_);
    ok_("hostile: info into this program's own read-only code page -> EFAULT", mc(INFO, 0, (i64)(u64)&same, 0, 0) == EFAULT_);
    ok_("hostile: info into this program's read-only data -> EFAULT", mc(INFO, 0, (i64)(u64)SIZES, 0, 0) == EFAULT_);
    ok_("hostile: read into NULL -> EFAULT", mc(READ, 0, 0, 0, 16) == EFAULT_);
    ok_("hostile: read into an unmapped user page -> EFAULT", mc(READ, 0, 0, UNMAPPED, 4096) == EFAULT_);
    ok_("hostile: read into a kernel address -> EFAULT", mc(READ, 0, 0, KERNEL_VA, 4096) == EFAULT_);
    ok_("hostile: read into the kernel's identity-mapped RAM -> EFAULT", mc(READ, 0, 0, KERNEL_RAM, 512) == EFAULT_);
    ok_("hostile: read into this program's own read-only code page -> EFAULT", mc(READ, 0, 0, (i64)(u64)&same, 64) == EFAULT_);
    ok_("hostile: read into this program's read-only data -> EFAULT", mc(READ, 0, 0, (i64)(u64)SIZES, 8) == EFAULT_);
    ok_("hostile: a buffer that crosses the end of user memory -> EFAULT", mc(READ, 0, 0, (i64)0x00007FFFFFFFFFF8ULL, 16) == EFAULT_);
    ok_("hostile: unknown ops 0, 3, 2^63 and 2^64-1 -> EINVAL", mc(0, 0, 0, 0, 0) == EINVAL_ && mc(3, 0, 0, 0, 0) == EINVAL_
        && mc((i64)0x8000000000000000ULL, 0, 0, 0, 0) == EINVAL_ && mc(-1, 0, 0, 0, 0) == EINVAL_);
    ok_("identity: PRINCIPAL_INFER has no file authority (SYS_FS lookup -> EPERM)", fs_lookup("note") == EPERM_);
    { i64 r = mc(READ, 0, 0, (i64)buf, 4096); ok_("after every hostile call the first chunk still reads right", r == 4096 && same(0, 0, buf, 4096)); }
}

void __attribute__((section(".text._start"))) _start(u64 mode)
{
    sayn("mc: mode ", mode);
    if (mode == 1) full();
    else if (mode == 2) all_refused(EPERM_, "denied: every op (count, info, read, hostile, unknown) -> EPERM, nothing written");
    else if (mode == 3) all_refused(ENOSYS_, "novol: every op (count, info, read, hostile, unknown) -> ENOSYS, nothing written");
    else if (mode == 4 || mode == 5) {
        i64 r = mc(READ, 0, 0, (i64)buf, 4096);
        ok_("mid-read: the first chunk of weights.bin read", r == 4096 && same(0, 0, buf, 4096));
        if (mode == 4) { say("mc: faulting mid-object"); *(volatile u64 *)0x10 = 1; }
        say("mc: exiting mid-object"); quit(7);
    } else ok_("unknown mode", 0);
    sayn("mc: failures ", (u64)fails);
    quit(fails ? 1 : 0);
}
