/* Host test of the shared virtqueue bookkeeping against a simulated (and hostile) device. */
#include <stdio.h>
#include <string.h>
#include <virtqueue.h>

static int fails, checks;
#define CHECK(c, m) do { checks++; if (!(c)) { fails++; printf("  FAIL: %s (line %d)\n", m, __LINE__); } } while (0)

#define QN 8
static _Alignas(8) uint16_t availmem[2 + QN + 1];
static _Alignas(8) uint16_t usedmem[2 + 4 * QN + 1];
static int barriers;
static void bar(void) { barriers++; }

static struct vq q;
static void fresh(void) { memset(availmem, 0, sizeof availmem); memset(usedmem, 0, sizeof usedmem); barriers = 0; CHECK(vq_init(&q, QN, availmem, usedmem, bar) == VQ_OK, "init"); }
/* what an honest device does: complete one entry */
static void dev_put(uint32_t id, uint32_t len)
{
    uint16_t idx = usedmem[1];
    uint32_t *e = (uint32_t *)(void *)(usedmem + 2) + 2u * (idx & (QN - 1));
    e[0] = id; e[1] = len; usedmem[1] = (uint16_t)(idx + 1);
}

int main(void)
{
    /* init refuses bad arguments */
    CHECK(vq_init(0, QN, availmem, usedmem, 0) == VQ_EARG, "null queue");
    CHECK(vq_init(&q, 0, availmem, usedmem, 0) == VQ_EARG, "qn 0");
    CHECK(vq_init(&q, 6, availmem, usedmem, 0) == VQ_EARG, "qn not a power of two");
    CHECK(vq_init(&q, 512, availmem, usedmem, 0) == VQ_EARG, "qn over the bound");
    CHECK(vq_init(&q, QN, 0, usedmem, 0) == VQ_EARG, "null avail");

    /* honest round trip, ids and lengths come back, avail ring holds the heads in order */
    fresh();
    CHECK(vq_submit(&q, 3, 100) == VQ_OK && vq_submit(&q, 5, 0) == VQ_OK, "submit two");
    CHECK(availmem[1] == 2 && availmem[2] == 3 && availmem[3] == 5, "avail ring and index");
    CHECK(barriers >= 2, "barriers used when publishing");
    uint16_t id; uint32_t len;
    CHECK(vq_complete(&q, &id, &len) == VQ_EEMPTY, "nothing completed yet");
    dev_put(5, 0); dev_put(3, 100);
    CHECK(vq_complete(&q, &id, &len) == VQ_OK && id == 5 && len == 0, "out-of-order completion 5");
    CHECK(vq_complete(&q, &id, &len) == VQ_OK && id == 3 && len == 100, "completion 3 at exactly its cap");
    CHECK(vq_complete(&q, &id, &len) == VQ_EEMPTY, "drained");

    /* argument checks and flow control */
    fresh();
    CHECK(vq_submit(&q, QN, 0) == VQ_EARG, "head out of range");
    CHECK(vq_submit(&q, 1, 0) == VQ_OK && vq_submit(&q, 1, 0) == VQ_EFULL, "the same head cannot be in flight twice");
    CHECK(vq_complete(&q, 0, &len) == VQ_EARG && vq_complete(&q, &id, 0) == VQ_EARG, "null out params");

    /* the ring can be filled exactly, never overfilled */
    fresh();
    for (int i = 0; i < QN; i++) CHECK(vq_submit(&q, (uint16_t)i, 0) == VQ_OK, "fill to qn");
    CHECK(vq_submit(&q, 0, 0) == VQ_EFULL, "full");

    /* indices wrap: 70000 submit/complete cycles through a queue of 8 */
    fresh();
    int wrap_ok = 1;
    for (uint32_t i = 0; i < 70000; i++) {
        uint16_t h = (uint16_t)(i % QN);
        if (vq_submit(&q, h, 4) != VQ_OK) { wrap_ok = 0; break; }
        dev_put(h, 4);
        if (vq_complete(&q, &id, &len) != VQ_OK || id != h || len != 4) { wrap_ok = 0; break; }
    }
    CHECK(wrap_ok && q.outstanding == 0, "70000 cycles across the 16-bit index wrap");

    /* hostile device: id out of range */
    fresh(); vq_submit(&q, 1, 8); dev_put(QN, 0);
    CHECK(vq_complete(&q, &id, &len) == VQ_EPROTO, "id == qn refused");
    CHECK(vq_complete(&q, &id, &len) == VQ_EPROTO && vq_submit(&q, 2, 0) == VQ_EPROTO, "a broken queue stays broken and takes no work");
    fresh(); vq_submit(&q, 1, 8); dev_put(0xFFFFFFFFu, 0);
    CHECK(vq_complete(&q, &id, &len) == VQ_EPROTO, "id 0xFFFFFFFF refused (no 16-bit truncation to a valid id)");
    fresh(); vq_submit(&q, 1, 8); dev_put(0x10001u, 0);
    CHECK(vq_complete(&q, &id, &len) == VQ_EPROTO, "id 0x10001 refused (truncates to 1, which is in flight)");

    /* hostile device: id never submitted, repeated id, length over the cap */
    fresh(); vq_submit(&q, 1, 8); dev_put(2, 0);
    CHECK(vq_complete(&q, &id, &len) == VQ_EPROTO, "id not in flight refused");
    fresh(); vq_submit(&q, 1, 8); vq_submit(&q, 2, 8); dev_put(1, 0); dev_put(1, 0);
    CHECK(vq_complete(&q, &id, &len) == VQ_OK, "first completion of 1 fine");
    CHECK(vq_complete(&q, &id, &len) == VQ_EPROTO, "a repeated id refused");
    fresh(); vq_submit(&q, 1, 8); dev_put(1, 9);
    CHECK(vq_complete(&q, &id, &len) == VQ_EPROTO, "length over the cap by one refused");
    fresh(); vq_submit(&q, 1, 0); dev_put(1, 1);
    CHECK(vq_complete(&q, &id, &len) == VQ_EPROTO, "device-read-only chain reported as written refused");
    fresh(); vq_submit(&q, 1, 8); dev_put(1, 0xFFFFFFFFu);
    CHECK(vq_complete(&q, &id, &len) == VQ_EPROTO, "length 2^32-1 refused");

    /* hostile device: used index jumps ahead of what was submitted */
    fresh(); vq_submit(&q, 1, 8); usedmem[1] = 5;
    CHECK(vq_complete(&q, &id, &len) == VQ_EPROTO, "used index far ahead refused");
    fresh(); vq_submit(&q, 1, 8); dev_put(1, 0); usedmem[1] = 5;
    CHECK(vq_complete(&q, &id, &len) == VQ_EPROTO, "valid first entry but the used index claims more completions than were submitted");
    fresh(); usedmem[1] = 1;
    CHECK(vq_complete(&q, &id, &len) == VQ_EPROTO, "completion with nothing submitted refused");
    fresh(); vq_submit(&q, 1, 8); usedmem[1] = (uint16_t)0xFFFF;
    CHECK(vq_complete(&q, &id, &len) == VQ_EPROTO, "used index that went backwards (wraps to 65535 ahead) refused");

    /* a failed completion consumed nothing and leaked no state: a fresh queue works again */
    fresh();
    CHECK(vq_submit(&q, 0, 4) == VQ_OK, "re-init recovers");

    printf("virtqueue_test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
