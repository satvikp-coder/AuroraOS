/* tests/tools/mktensor.c: host tool that writes one AURQMV1 int8 matvec tensor object (format: tests/programs/matvec_core.h) with deterministic
 * pseudo-random contents, and prints the expected result computed by the INDEPENDENT reference (tests/kernel-host/matvec_ref.h), one line per row,
 * in exactly the form the EL0 program prints it:  "ic: y <row> <acc> <q>". The file is then packed into a model volume with tests/tools/mkmodel.
 *
 *   mktensor [options] out.bin rows cols shift seed
 *     --fill-x V / --fill-w V   every activation / weight is V (-128..127) instead of pseudo-random
 *     --mult M                  every row multiplier is M instead of pseudo-random (1 .. 65536)
 *     --claim-rows N / --claim-cols N   write N into the header instead of the real dimensions (a lying header)
 *     --cut N                   drop the last N bytes of the object (truncated object)
 *     --extra N                 append N zero bytes (trailing garbage)
 *     --zero-mult R             row R's multiplier is 0 (invalid scale)
 * The reference lines describe the REAL data; for hostile objects they are not expected output (the EL0 program must reject the object).
 * Build: cc -std=c11 -Wall -Wextra -O1 -o mktensor tests/tools/mktensor.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel-host/matvec_ref.h"

static uint64_t s;
static uint32_t rnd(void) { s = s * 6364136223846793005ull + 1442695040888963407ull; return (uint32_t)(s >> 33); }
static int usage(void) { fprintf(stderr, "usage: mktensor [--fill-x V] [--fill-w V] [--mult M] [--claim-rows N] [--claim-cols N] [--cut N] [--extra N] [--zero-mult R] out.bin rows cols shift seed\n"); return 2; }

int main(int argc, char **argv)
{
    long fx = 1000, fw = 1000, mult = 0, crows = -1, ccols = -1, cut = 0, extra = 0, zrow = -1; int a = 1;
    for (; a + 1 < argc && strncmp(argv[a], "--", 2) == 0; a += 2) {
        long v = strtol(argv[a + 1], NULL, 0);
        if (!strcmp(argv[a], "--fill-x")) fx = v; else if (!strcmp(argv[a], "--fill-w")) fw = v; else if (!strcmp(argv[a], "--mult")) mult = v;
        else if (!strcmp(argv[a], "--claim-rows")) crows = v; else if (!strcmp(argv[a], "--claim-cols")) ccols = v;
        else if (!strcmp(argv[a], "--cut")) cut = v; else if (!strcmp(argv[a], "--extra")) extra = v; else if (!strcmp(argv[a], "--zero-mult")) zrow = v;
        else return usage();
    }
    if (argc - a != 5) return usage();
    const char *out = argv[a];
    long rows = strtol(argv[a + 1], NULL, 0), cols = strtol(argv[a + 2], NULL, 0), shift = strtol(argv[a + 3], NULL, 0);
    s = strtoull(argv[a + 4], NULL, 0);
    if (rows < 1 || cols < 1 || rows > 100000 || cols > 100000 || shift < 0 || shift > 62 || cut < 0 || extra < 0) return usage();

    int8_t *x = malloc((size_t)cols), *w = malloc((size_t)rows * (size_t)cols); int64_t *m = malloc(sizeof *m * (size_t)rows), *acc = malloc(sizeof *acc * (size_t)rows);
    int *q = malloc(sizeof *q * (size_t)rows);
    if (!x || !w || !m || !acc || !q) { fprintf(stderr, "mktensor: out of memory\n"); return 1; }
    for (long c = 0; c < cols; c++) x[c] = (int8_t)(fx != 1000 ? fx : (long)(rnd() & 0xFF) - 128);
    for (long i = 0; i < rows * cols; i++) w[i] = (int8_t)(fw != 1000 ? fw : (long)(rnd() & 0xFF) - 128);
    for (long r = 0; r < rows; r++) m[r] = r == zrow ? 0 : mult ? mult : 1 + (int64_t)(rnd() % 65536);

    size_t len;
    uint8_t *b = ref_serialize(crows >= 0 ? (uint32_t)crows : (uint32_t)rows, ccols >= 0 ? (uint32_t)ccols : (uint32_t)cols, (uint32_t)shift, 1, 0, 0, x, m, w,
                               (unsigned)rows, (unsigned)cols, &len);
    if (!b || (size_t)cut >= len) { fprintf(stderr, "mktensor: bad --cut or out of memory\n"); return 1; }
    FILE *f = fopen(out, "wb");
    if (!f || fwrite(b, 1, len - (size_t)cut, f) != len - (size_t)cut) { perror(out); return 1; }
    for (long i = 0; i < extra; i++) fputc(0, f);
    if (fclose(f) != 0) { perror(out); return 1; }

    ref_matvec(w, x, m, (unsigned)rows, (unsigned)cols, (unsigned)shift, acc, q);
    for (long r = 0; r < rows; r++) printf("ic: y %ld %lld %d\n", r, (long long)acc[r], q[r]);
    fprintf(stderr, "mktensor: %s: %zu bytes (%ld x %ld, shift %ld)\n", out, len - (size_t)cut + (size_t)extra, rows, cols, shift);
    return 0;
}
