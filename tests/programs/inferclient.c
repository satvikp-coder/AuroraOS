/*
 * tests/programs/inferclient.c - EL0 int8 matrix-vector program for tests/qemu/infer-tests.sh, launched by the AURORA_INFER_TEST kernel hook
 * (kernel/infertest.c), which chooses the identity (PRINCIPAL_INFER, or an ordinary application identity) and passes the mode in x0:
 *   mode k (1..16)  read model-volume object k-1 through SYS_MODEL (11), validate it as an AURQMV1 tensor, compute the quantized matvec
 *                   (tests/programs/matvec_core.h, the code the host test exercises) streaming the payload in reads of at most 4096 bytes,
 *                   and print every row as "ic: y <row> <acc> <q>" ONLY after the whole object validated.
 * Mode 1 also runs a synthetic dense/ReLU/dense graph on the first three output values.
 * Exit codes: 0 computed and printed; 3 the object was rejected ("ic: reject <reason>"); 4 the kernel refused SYS_MODEL ("ic: refused <code>":
 * -7 EPERM without PRINCIPAL_INFER, -4 ENOSYS without a valid volume); 5 no such object. Every path exits; nothing here faults on hostile data.
 * Memory: static buffers only (one 4096-byte read buffer + the run state, about 13 KiB); no SYS_MEM heap, no floating point.
 * The result is untrusted data: it is printed for comparison and nothing acts on it. Output goes through SYS_WRITE ("[user pN] " prefix by the kernel).
 */
#include "common.h"
#include "matvec_core.h"
#include <quant_ops.h>
#define SYS_MODEL 11
#define INFO 1
#define READ 2
#define COUNT 0xFFFFFFFFL
typedef unsigned char u8;

_Static_assert(MV_CHUNK <= 4096, "a SYS_MODEL read is at most 4096 bytes");

struct obj { u64 size; u32 kind, reserved; char name[32]; u8 digest[32]; };
static i64 mc(i64 op, i64 a, i64 b, i64 c, i64 d) { return sc(SYS_MODEL, op, a, b, c, d); }

static struct mv_run R;
static u8 buf[MV_CHUNK];
static struct obj o;

/* one line, one SYS_WRITE */
static char line[160]; static u64 ln;
static void put(const char *s) { while (*s && ln < sizeof line - 1) line[ln++] = *s++; }
static void putd(i64 v)
{
    char d[24]; int k = 0; u64 u;
    if (v < 0) { put("-"); u = (u64)0 - (u64)v; } else u = (u64)v;
    if (!u) d[k++] = '0';
    while (u) { d[k++] = (char)('0' + u % 10); u /= 10; }
    while (k && ln < sizeof line - 1) line[ln++] = d[--k];
}
static void flush(void) { line[ln] = 0; say(line); ln = 0; }

static int64_t rd(void *ctx, uint64_t off, uint8_t *b, uint64_t len) { return mc(READ, (i64)*(u64 *)ctx, (i64)off, (i64)b, (i64)len); }

/* Execute two fixed synthetic dense layers on three verified tensor outputs. */
static int tiny_graph(void)
{
    static const int8_t first[6] = { 1, -2, 3, -3, 2, -1 };
    static const int8_t second[4] = { 2, -3, -1, 2 };
    int8_t input[3], hidden[2], active[2], result[2];

    if (R.h.rows < 3) return -1;
    for (unsigned i = 0; i < 3; i++) input[i] = (int8_t)R.out_q[i];
    if (quant_matvec(first, 2, 3, input, 1, 1, 0, hidden) ||
        quant_relu(hidden, 2, active) ||
        quant_matvec(second, 2, 2, active, 1, 2, 0, result)) return -1;

    for (unsigned i = 0; i < 2; i++) {
        put("ic: graph "); putd(i); put(" "); putd(result[i]); flush();
    }
    return 0;
}

void __attribute__((section(".text._start"))) _start(u64 mode)
{
    sayn("ic: mode ", mode);
    i64 cnt = mc(INFO, COUNT, 0, 0, 0);
    if (cnt < 0) { put("ic: refused "); putd(cnt); flush(); quit(4); }
    u64 k = mode - 1;
    if (mode == 0 || k >= (u64)cnt) { say("ic: no such object"); quit(5); }
    i64 r = mc(INFO, (i64)k, (i64)&o, 0, 0);
    if (r < 0) { put("ic: refused "); putd(r); flush(); quit(4); }
    o.name[31] = 0;
    put("ic: object "); putd((i64)k); put(" "); put(o.name); put(" size "); putd((i64)o.size); put(" kind "); putd((i64)o.kind); flush();

    int e = mv_check_object(o.kind, o.size);
    if (e == MV_OK) e = mv_run_object(&R, o.size, rd, &k, buf);
    if (e != MV_OK) { put("ic: reject "); put(mv_strerror(e)); flush(); quit(3); }

    for (u32 i = 0; i < R.h.rows; i++) { put("ic: y "); putd(i); put(" "); putd(R.out_acc[i]); put(" "); putd(R.out_q[i]); flush(); }
    put("ic: done "); putd(R.h.rows); put(" rows "); putd(R.h.cols); put(" cols"); flush();
    if (mode == 1 && tiny_graph()) { say("ic: graph rejected"); quit(3); }
    quit(0);
}
