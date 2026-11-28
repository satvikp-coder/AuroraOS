#include <qwen3_merges.h>
#include <stdio.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "failed line %d\n", __LINE__); return 1; } } while (0)

static int64_t read_file(void *ctx, uint64_t off, uint8_t *buf, uint64_t n)
{
    FILE *f = ctx;
    if (fseek(f, (long)off, SEEK_SET)) return -1;
    /* Exercise partial reads at every level. */
    return (int64_t)fread(buf, 1, n > 7 ? 7 : (size_t)n, f);
}

int main(int argc, char **argv)
{
    uint32_t result, rank;
    if (argc == 2) {
        /* Optional offline verification using the locally exported public table. */
        static const uint32_t probes[][4] = {
            {220,220,256,0}, {72,77,258,2}, {439,269,1256,1000},
            {6684,94,100256,100000}, {124002,245,151642,151386}
        };
        FILE *real = fopen(argv[1], "rb");
        CHECK(real && fseek(real, 0, SEEK_END) == 0);
        long size = ftell(real);
        CHECK(size == 16 + 151387 * 16);
        for (unsigned i = 0; i < sizeof(probes) / sizeof(probes[0]); ++i) {
            CHECK(q3m_find_merge(read_file, real, size, probes[i][0], probes[i][1],
                                 &result, &rank) == Q3M_OK);
            CHECK(result == probes[i][2] && rank == probes[i][3]);
        }
        CHECK(fclose(real) == 0);
        puts("qwen3_merges: five published pairs and ranks matched");
        return 0;
    }

    uint8_t table[] = {'A','U','R','Q','T','M','1',0,4,0,0,0,2,0,0,0,
                       0,0,0,0,1,0,0,0,2,0,0,0,1,0,0,0,
                       1,0,0,0,0,0,0,0,3,0,0,0,0,0,0,0};
    FILE *f = tmpfile();
    CHECK(f && fwrite(table, 1, sizeof(table), f) == sizeof(table));
    CHECK(q3m_find_merge(read_file, f, sizeof(table), 1, 0, &result, &rank) == Q3M_OK);
    CHECK(result == 3 && rank == 0);
    CHECK(q3m_find_merge(read_file, f, sizeof(table), 0, 1, &result, &rank) == Q3M_OK);
    CHECK(result == 2 && rank == 1);
    CHECK(q3m_find_merge(read_file, f, sizeof(table), 0, 0, &result, &rank) == Q3M_E_NOENT);
    CHECK(q3m_find_merge(read_file, f, sizeof(table), 4, 0, &result, &rank) == Q3M_E_NOENT);
    CHECK(q3m_find_merge(read_file, f, sizeof(table)-1, 0, 1, &result, &rank) == Q3M_E_BOUNDS);
    CHECK(fseek(f, 44, SEEK_SET) == 0 && fputc(2, f) == 2);
    CHECK(q3m_find_merge(read_file, f, sizeof(table), 1, 0, &result, &rank) == Q3M_E_FORMAT);
    CHECK(fclose(f) == 0);
    puts("qwen3_merges: bounded lookup and error cases passed");
    return 0;
}
