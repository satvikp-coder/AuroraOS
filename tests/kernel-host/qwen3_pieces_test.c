#include <stdio.h>
#include <string.h>
#include <qwen3_pieces.h>

static int64_t short_read(void *ctx, uint64_t off, uint8_t *out, uint64_t n)
{
    /* All calls must stay within the test object. Exercise short reads. */
    if (off >= 41 || n > 41 - off) return -1;
    if (n > 2) n = 2;
    memcpy(out, (uint8_t *)ctx + off, n);
    return (int64_t)n;
}

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "failed line %d\n", __LINE__); return 1; } } while (0)

int main(void)
{
    /* Bytes independently checked against the Python export test. */
    uint8_t table[] = {'A','U','R','Q','T','P','1',0,4,0,0,0,0,0,0,0,1,0,0,0,
                       2,0,0,0,4,0,0,128,9,0,0,0,'A',0,' ','B','<','e','n','d','>'};
    uint8_t piece[5];
    int special = -1;
    CHECK(q3p_read_piece(short_read, table, sizeof(table), 1, piece, 5, &special) == 1);
    CHECK(piece[0] == 0 && !special);
    CHECK(q3p_read_piece(short_read, table, sizeof(table), 2, piece, 5, &special) == 2);
    CHECK(memcmp(piece, " B", 2) == 0 && !special);
    CHECK(q3p_read_piece(short_read, table, sizeof(table), 3, piece, 5, &special) == 5);
    CHECK(memcmp(piece, "<end>", 5) == 0 && special);
    CHECK(q3p_read_piece(short_read, table, sizeof(table), 3, piece, 4, &special) == Q3M_E_BOUNDS);
    CHECK(q3p_read_piece(short_read, table, sizeof(table), 4, piece, 5, &special) == Q3M_E_NOENT);
    table[28] = 8;
    CHECK(q3p_read_piece(short_read, table, sizeof(table), 3, piece, 5, &special) == Q3M_E_BOUNDS);
    table[20] = 1;
    CHECK(q3p_read_piece(short_read, table, sizeof(table), 1, piece, 5, &special) == Q3M_E_BOUNDS);
    puts("qwen3_pieces: lookup, short reads and malformed ranges passed");
    return 0;
}
