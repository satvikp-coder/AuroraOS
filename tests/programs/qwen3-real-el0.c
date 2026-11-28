#include "common.h"
#include <uvlayout.h>
#include <qwen3_full.h>

#define SYS_MEM 10
#define SYS_MODEL 11
#define MEM_GROW 1
#define MEM_SHRINK 2
#define MODEL_INFO 1
#define MODEL_READ 2

struct model_info { uint64_t size; uint32_t kind, reserved; char name[32]; uint8_t digest[32]; };

static int64_t model_read(void *ctx, uint64_t off, uint8_t *buf, uint64_t len)
{
    return sc(SYS_MODEL, MODEL_READ, (i64)*(uint64_t *)ctx, (i64)off, (i64)buf, (i64)len);
}

_Static_assert(sizeof(struct q3f_arena) <= USER_HEAP_MAX_PAGES * USER_PAGE_BYTES,
               "single-position arena fits the ordinary EL0 heap");

void __attribute__((section(".text._start"))) _start(u64 mode)
{
    if (mode != 1) quit(3);
    uint64_t object = 0;
    struct model_info info;
    i64 rc = sc(SYS_MODEL, MODEL_INFO, 0, (i64)&info, 0, 0);
    if (rc != 0) quit(4);
    const uint64_t pages = (sizeof(struct q3f_arena) + USER_PAGE_BYTES - 1u) / USER_PAGE_BYTES;
    if (sc(SYS_MEM, MEM_GROW, (i64)pages, 0, 0, 0) != (i64)pages) {
        say("qwen-real: no memory for model arena");
        quit(3);
    }
    struct q3f_arena *arena = (void *)USER_HEAP_VA;
    sayn("qwen-real: arena bytes ", sizeof(*arena));
    uint32_t next = 0;
    int result = q3f_load_index(arena, model_read, &object, info.size);
    if (result == Q3M_OK && (arena->hdr.num_layers != 28 || arena->hdr.hidden_dim != 1024 ||
                            arena->hdr.vocab_size != 151936 || arena->hdr.num_heads != 16 ||
                            arena->hdr.num_kv_heads != 8 || arena->hdr.head_dim != 128))
        result = Q3M_E_DIMS;
    if (result == Q3M_OK) {
        say("qwen-real: verified 28-layer model index");
        result = q3f_forward_token(arena, model_read, &object, 151643, 0, &next);
    }
    union { float value; uint32_t bits; } logit = { .value = arena->last_logit };
    if (sc(SYS_MEM, MEM_SHRINK, (i64)pages, 0, 0, 0) != 0) quit(3);
    if (result != Q3M_OK) {
        sayn("qwen-real: forward rejected code ", (u64)(-result));
        quit(3);
    }
    sayn("qwen-real: next token ", next);
    sayn("qwen-real: logit bits ", logit.bits);
    quit(0);
}
