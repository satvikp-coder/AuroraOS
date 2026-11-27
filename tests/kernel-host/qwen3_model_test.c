/* tests/kernel-host/qwen3_model_test.c - host tests for kernel/include/qwen3_model.h */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <qwen3_projection.h>

static int passed, failed;
#define CHECK(name, cond) do { if (cond) passed++; else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)

static void put_le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_le32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void put_le64(uint8_t *p, uint64_t v) { put_le32(p, (uint32_t)v); put_le32(p + 4, (uint32_t)(v >> 32)); }

struct mem_dev {
    const uint8_t *b;
    size_t len;
    int64_t fail_at;
    int64_t short_at;
};

static int64_t mem_rd(void *ctx, uint64_t off, uint8_t *buf, uint64_t len)
{
    struct mem_dev *d = (struct mem_dev *)ctx;
    if (len == 0 || len > Q3M_CHUNK) return -1;
    if (d->fail_at >= 0 && off >= (uint64_t)d->fail_at) return -5;
    if (off >= d->len) return 0;
    uint64_t n = d->len - off < len ? d->len - off : len;
    memcpy(buf, d->b + off, (size_t)n);
    if (d->short_at >= 0 && off >= (uint64_t)d->short_at && n > 0) return (int64_t)(n - 1);
    return (int64_t)n;
}

int main(void)
{
    /* Build a valid 2-tensor model image: 48 hdr + 2*32 dir + 12 + 8 = 132 bytes */
    uint8_t img[132];
    memset(img, 0, sizeof(img));
    memcpy(img, "AURQ3V1\0", 8);
    put_le32(img + 8, 1);    /* version */
    put_le32(img + 12, 16);  /* vocab_size */
    put_le32(img + 16, 8);   /* hidden_dim */
    put_le32(img + 20, 16);  /* intermediate_dim */
    put_le32(img + 24, 2);   /* num_layers */
    put_le32(img + 28, 2);   /* num_heads */
    put_le32(img + 32, 4);   /* head_dim */
    put_le32(img + 36, 2);   /* num_tensors */
    put_le64(img + 40, sizeof(img));

    /* Tensor 0: layer 0, WQ, 3x4 = 12 bytes at offset 112 */
    uint8_t *e0 = img + Q3M_HDR_SIZE;
    put_le16(e0 + 0, 0); put_le16(e0 + 2, Q3M_T_WQ);
    put_le32(e0 + 4, 3); put_le32(e0 + 8, 4); put_le32(e0 + 12, 2);
    put_le64(e0 + 16, 112); put_le64(e0 + 24, 12);

    /* Tensor 1: layer 1, WDOWN, 2x4 = 8 bytes at offset 124 */
    uint8_t *e1 = img + Q3M_HDR_SIZE + Q3M_ENTRY_SIZE;
    put_le16(e1 + 0, 1); put_le16(e1 + 2, Q3M_T_WDOWN);
    put_le32(e1 + 4, 2); put_le32(e1 + 8, 4); put_le32(e1 + 12, 3);
    put_le64(e1 + 16, 124); put_le64(e1 + 24, 8);

    for (size_t i = 0; i < 12; i++) img[112 + i] = (uint8_t)(i + 1);
    for (size_t i = 0; i < 8; i++)  img[124 + i] = (uint8_t)(0x80u + i);

    struct q3m_header h;
    CHECK("valid header parses", q3m_parse_header(img, Q3M_HDR_SIZE, sizeof(img), &h) == Q3M_OK && h.num_tensors == 2);
    CHECK("null header rejected", q3m_parse_header(0, Q3M_HDR_SIZE, sizeof(img), &h) == Q3M_E_ARG);
    CHECK("short header rejected", q3m_parse_header(img, Q3M_HDR_SIZE - 1, sizeof(img), &h) == Q3M_E_TRUNC);

    uint8_t bad_hdr[Q3M_HDR_SIZE];
    memcpy(bad_hdr, img, Q3M_HDR_SIZE); bad_hdr[0] = 'X';
    CHECK("bad magic rejected", q3m_parse_header(bad_hdr, Q3M_HDR_SIZE, sizeof(img), &h) == Q3M_E_MAGIC);

    memcpy(bad_hdr, img, Q3M_HDR_SIZE); put_le32(bad_hdr + 8, 2);
    CHECK("bad version rejected", q3m_parse_header(bad_hdr, Q3M_HDR_SIZE, sizeof(img), &h) == Q3M_E_VERSION);

    memcpy(bad_hdr, img, Q3M_HDR_SIZE); put_le32(bad_hdr + 28, 4); /* 4 * 4 = 16 > hidden 8 */
    CHECK("heads * head_dim > hidden rejected", q3m_parse_header(bad_hdr, Q3M_HDR_SIZE, sizeof(img), &h) == Q3M_E_DIMS);

    memcpy(bad_hdr, img, Q3M_HDR_SIZE); put_le64(bad_hdr + 40, sizeof(img) + 1);
    CHECK("total_bytes != obj_size rejected", q3m_parse_header(bad_hdr, Q3M_HDR_SIZE, sizeof(img), &h) == Q3M_E_BOUNDS);

    struct mem_dev dev = { img, sizeof(img), -1, -1 };
    struct q3m_tensor_desc td;
    CHECK("read tensor 0", q3m_read_tensor_desc(&h, 0, mem_rd, &dev, &td) == Q3M_OK && td.role == Q3M_T_WQ && td.bytes == 12);
    CHECK("read out-of-range tensor returns NOENT", q3m_read_tensor_desc(&h, 2, mem_rd, &dev, &td) == Q3M_E_NOENT);

    uint8_t wbuf[16];
    CHECK("stream tensor 0", q3m_stream_tensor(&td, mem_rd, &dev, wbuf, sizeof(wbuf)) == Q3M_OK && wbuf[0] == 1 && wbuf[11] == 12);
    CHECK("stream into too-small buffer rejected", q3m_stream_tensor(&td, mem_rd, &dev, wbuf, 8) == Q3M_E_BOUNDS);

    const int8_t input[4] = { 7, -4, -8, 2 };
    int8_t projection[3], reference[3], scratch[8];
    CHECK("full-tensor reference",
          quant_matvec((const int8_t *)wbuf, 3, 4, input, 1, td.shift, 0, reference) == 0);
    CHECK("projection batches two rows and handles final row",
          q3m_project_i8(&td, mem_rd, &dev, input, 4, projection, 3, scratch, 8) == Q3M_OK &&
          memcmp(projection, reference, sizeof(projection)) == 0);
    CHECK("projection works with one row of scratch",
          q3m_project_i8(&td, mem_rd, &dev, input, 4, projection, 3, scratch, 4) == Q3M_OK &&
          memcmp(projection, reference, sizeof(projection)) == 0);
    CHECK("projection rejects short scratch",
          q3m_project_i8(&td, mem_rd, &dev, input, 4, projection, 3, scratch, 3) == Q3M_E_BOUNDS);
    CHECK("projection rejects mismatched input and short output",
          q3m_project_i8(&td, mem_rd, &dev, input, 3, projection, 3, scratch, 8) == Q3M_E_BOUNDS &&
          q3m_project_i8(&td, mem_rd, &dev, input, 4, projection, 2, scratch, 8) == Q3M_E_BOUNDS);
    struct q3m_tensor_desc overflow = td;
    overflow.offset = UINT64_MAX - td.bytes + 1;
    CHECK("projection rejects wrapped offset",
          q3m_project_i8(&overflow, mem_rd, &dev, input, 4, projection, 3, scratch, 8) == Q3M_E_BOUNDS);
    dev.fail_at = 120;
    CHECK("projection propagates read failure in second batch",
          q3m_project_i8(&td, mem_rd, &dev, input, 4, projection, 3, scratch, 8) == Q3M_E_READ);

    /* Hostile read failures */
    dev.fail_at = 112;
    CHECK("read error during stream -> Q3M_E_READ", q3m_stream_tensor(&td, mem_rd, &dev, wbuf, sizeof(wbuf)) == Q3M_E_READ);
    dev.fail_at = -1; dev.short_at = 112;
    CHECK("short read during stream -> Q3M_E_TRUNC", q3m_stream_tensor(&td, mem_rd, &dev, wbuf, sizeof(wbuf)) == Q3M_E_TRUNC);

    printf("qwen3_model: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
