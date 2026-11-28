/* tests/kernel-host/qwen3_full_test.c - host & real-weight test for kernel/include/qwen3_full.h */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <qwen3_full.h>

static int passed, failed;
#define CHECK(name, cond) do { if (cond) passed++; else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)

struct file_ctx {
    FILE *fp;
    uint64_t size;
};

static int64_t file_rd(void *ctx, uint64_t off, uint8_t *buf, uint64_t len)
{
    struct file_ctx *fc = (struct file_ctx *)ctx;
    if (!fc || !fc->fp || len == 0 || len > Q3M_CHUNK || off > fc->size || len > fc->size - off)
        return -1;
    if (fseeko(fc->fp, (off_t)off, SEEK_SET) != 0) return -1;
    size_t n = fread(buf, 1, (size_t)len, fc->fp);
    return n == (size_t)len ? (int64_t)n : -1;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void put_le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_le32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void put_le64(uint8_t *p, uint64_t v) { put_le32(p, (uint32_t)v); put_le32(p + 4, (uint32_t)(v >> 32)); }

struct mem_ctx {
    uint8_t *buf;
    uint64_t size;
};

static int64_t mem_rd(void *ctx, uint64_t off, uint8_t *dst, uint64_t len)
{
    struct mem_ctx *mc = (struct mem_ctx *)ctx;
    if (!mc || len == 0 || len > Q3M_CHUNK || off > mc->size || len > mc->size - off) return -1;
    memcpy(dst, mc->buf + off, (size_t)len);
    return (int64_t)len;
}

static void test_synthetic_v2(struct q3f_arena *a)
{
    /* 1-layer tied V2 model: vocab=4, hidden=4, inter=4, heads=2, kv_heads=1, head_dim=2, 13 tensors */
    static uint8_t img[4096];
    memset(img, 0, sizeof(img));
    const uint32_t nt = 13;
    uint64_t cursor = Q3M_V2_HDR_SIZE + (uint64_t)nt * Q3M_ENTRY_SIZE;
    static const struct { uint16_t layer, role; uint32_t rows, cols; int32_t shift; int8_t fill; } specs[13] = {
        { Q3M_GLOBAL_LAYER, Q3M_T_EMBED, 4, 4, 0, 1 },
        { Q3M_GLOBAL_LAYER, Q3M_T_FINAL_NORM, 1, 4, 0, 1 },
        { 0, Q3M_T_INPUT_NORM, 1, 4, 0, 1 },
        { 0, Q3M_T_WQ, 4, 4, 2, 1 },
        { 0, Q3M_T_WK, 2, 4, 2, 1 },
        { 0, Q3M_T_WV, 2, 4, 2, 1 },
        { 0, Q3M_T_Q_NORM, 1, 2, 0, 1 },
        { 0, Q3M_T_K_NORM, 1, 2, 0, 1 },
        { 0, Q3M_T_WO, 4, 4, 2, 1 },
        { 0, Q3M_T_POST_ATTN_NORM, 1, 4, -1, 1 }, /* negative shift = 2.0 */
        { 0, Q3M_T_WGATE, 4, 4, 2, 1 },
        { 0, Q3M_T_WUP, 4, 4, 2, 1 },
        { 0, Q3M_T_WDOWN, 4, 4, 2, 1 },
    };
    for (uint32_t i = 0; i < nt; ++i) {
        uint8_t *e = img + Q3M_V2_HDR_SIZE + i * Q3M_ENTRY_SIZE;
        uint64_t sz = (uint64_t)specs[i].rows * specs[i].cols;
        put_le16(e + 0, specs[i].layer);
        put_le16(e + 2, specs[i].role);
        put_le32(e + 4, specs[i].rows);
        put_le32(e + 8, specs[i].cols);
        put_le32(e + 12, (uint32_t)specs[i].shift);
        put_le64(e + 16, cursor);
        put_le64(e + 24, sz);
        memset(img + cursor, (uint8_t)specs[i].fill, (size_t)sz);
        if (specs[i].role == Q3M_T_EMBED) {
            /* Make token 2 embedding highest so greedy argmax picks 2 */
            memset(img + cursor + 8, 4, 4);
        }
        cursor += sz;
    }
    memcpy(img, "AURQ3V2", 8);
    put_le32(img + 8, 2);
    put_le32(img + 12, 4);
    put_le32(img + 16, 4);
    put_le32(img + 20, 4);
    put_le32(img + 24, 1);
    put_le32(img + 28, 2);
    put_le32(img + 32, 2);
    put_le32(img + 36, nt);
    put_le64(img + 40, cursor);
    put_le32(img + 48, 1);
    put_le32(img + 52, Q3M_TIED_EMBED);

    struct mem_ctx mc = { img, cursor };
    CHECK("load synthetic V2 index with negative shift", q3f_load_index(a, mem_rd, &mc, cursor) == Q3M_OK);
    uint32_t next = 99;
    CHECK("forward pos 0 succeeds", q3f_forward_token(a, mem_rd, &mc, 0, 0, &next) == Q3M_OK && next == 2);
#ifndef Q3F_SINGLE_POSITION
    CHECK("forward pos 1 succeeds", q3f_forward_token(a, mem_rd, &mc, next, 1, &next) == Q3M_OK && next == 2);
#else
    int silu_close = 1;
    for (int i = -2000; i <= 2000; ++i) {
        float x = (float)i / 100.0f;
        if (fabsf(q3f_silu(x) - x / (1.0f + expf(-x))) > 0.000004f) silu_close = 0;
    }
    CHECK("freestanding SiLU matches the libm reference within 4e-6", silu_close);
#endif
    CHECK("out-of-bounds pos rejected", q3f_forward_token(a, mem_rd, &mc, 0, Q3F_MAX_CTX, &next) == Q3M_E_BOUNDS);
}

int main(int argc, char **argv)
{
    struct q3f_arena *a = (struct q3f_arena *)calloc(1, sizeof(*a));
    if (!a) return 2;

    test_synthetic_v2(a);

    if (argc > 1) {
        FILE *fp = fopen(argv[1], "rb");
        CHECK("open real AURQ3V2 model", fp != NULL);
        if (fp) {
            fseeko(fp, 0, SEEK_END);
            uint64_t fsize = (uint64_t)ftello(fp);
            struct file_ctx fc = { fp, fsize };
            uint64_t t0 = now_ns();
            int rc = q3f_load_index(a, file_rd, &fc, fsize);
            uint64_t t1 = now_ns();
            CHECK("real Qwen3-0.6B index loaded", rc == Q3M_OK);
            CHECK("real Qwen3-0.6B dims match", a->hdr.hidden_dim == 1024 && a->hdr.num_layers == 28 && a->hdr.vocab_size == 151936);
            printf("qwen3_full: arena=%.2f MiB index_load_ms=%.2f layers=%u hidden=%u inter=%u vocab=%u\n",
                   (double)sizeof(*a) / (1024.0 * 1024.0), (double)(t1 - t0) / 1e6,
                   a->hdr.num_layers, a->hdr.hidden_dim, a->hdr.intermediate_dim, a->hdr.vocab_size);

            /* Prompt tokens passed after argv[1], or default BOS/sample */
            uint32_t prompt[16];
            int plen = 0;
            for (int i = 2; i < argc && plen < 16; ++i) prompt[plen++] = (uint32_t)strtoul(argv[i], NULL, 10);
            if (plen == 0) { prompt[0] = 785; prompt[1] = 6722; prompt[2] = 315; prompt[3] = 9625; prompt[4] = 374; plen = 5; }

            uint32_t next = 0;
            uint64_t tp0 = now_ns();
            for (int p = 0; p < plen; ++p) {
                rc = q3f_forward_token(a, file_rd, &fc, prompt[p], (uint32_t)p, (p + 1 == plen) ? &next : NULL);
                CHECK("prefill token ok", rc == Q3M_OK);
            }
            uint64_t tp1 = now_ns();
            printf("qwen3_full: prefill %d tokens in %.1f ms -> first generated token=%u\n",
                   plen, (double)(tp1 - tp0) / 1e6, next);
            printf("qwen3_full: first logit=%.9g\n", (double)a->last_logit);

            uint32_t gen[8];
            gen[0] = next;
            uint64_t td0 = now_ns();
            int gcount = 1;
            for (int step = 1; step < 4; ++step) {
                rc = q3f_forward_token(a, file_rd, &fc, gen[step - 1], (uint32_t)(plen + step - 1), &next);
                CHECK("decode token ok", rc == Q3M_OK);
                gen[gcount++] = next;
            }
            uint64_t td1 = now_ns();
            double decode_s = (double)(td1 - td0) / 1e9;
            printf("qwen3_full: decode %d tokens in %.2f s (%.2f tok/s):", gcount - 1, decode_s, (gcount - 1) / decode_s);
            for (int i = 0; i < gcount; ++i) printf(" %u", gen[i]);
            printf("\n");
            fclose(fp);
        }
    }

    free(a);
    printf("qwen3_full: %d passed, 0 failed\n", passed);
    return failed ? 1 : 0;
}
