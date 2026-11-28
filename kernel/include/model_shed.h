/*
 * kernel/include/model_shed.h - Low-memory load shedding, model unloading, and
 * deterministic Aura fallback routing (Issue #12).
 *
 * Pure freestanding C, no libc, no heap allocation.
 * When free physical memory drops below the configured low-watermark (or is
 * insufficient for model_bytes + headroom_bytes at load time), the runtime:
 *   1. Refuses or unloads resident model weights via the caller's unload hook.
 *   2. Transitions state to MS_SHED / MS_UNLOADED with resident_bytes = 0.
 *   3. Routes user commands through the deterministic zero-model Aura
 *      parser/validator (aura_parse + aura_validate), guaranteeing that typed
 *      and speech-matched Quick Commands continue to work under low memory.
 */
#ifndef AURORA_MODEL_SHED_H
#define AURORA_MODEL_SHED_H

#include <aura_cmd.h>
#include <stddef.h>
#include <stdint.h>

enum ms_state {
    MS_UNLOADED = 0,
    MS_RESIDENT = 1,
    MS_SHED     = 2
};

enum ms_rc {
    MS_OK       = 0,
    MS_EARG     = -1,
    MS_ELOWMEM  = -2,
    MS_ESTATE   = -3,
    MS_EBADCMD  = -4
};

struct ms_tracker {
    uint8_t state;
    uint8_t fallback_active;
    uint16_t reserved;
    uint32_t shed_count;
    uint32_t load_count;
    uint32_t fallback_count;
    uint64_t resident_bytes;
    uint64_t low_watermark_bytes;
    uint64_t headroom_bytes;
};

typedef void (*ms_unload_fn)(void *ctx);

static inline void ms_init(struct ms_tracker *t,
                           uint64_t low_watermark_bytes,
                           uint64_t headroom_bytes)
{
    if (!t) return;
    t->state = MS_UNLOADED;
    t->fallback_active = 1;
    t->reserved = 0;
    t->shed_count = 0;
    t->load_count = 0;
    t->fallback_count = 0;
    t->resident_bytes = 0;
    t->low_watermark_bytes = low_watermark_bytes;
    t->headroom_bytes = headroom_bytes;
}

static inline int ms_try_load(struct ms_tracker *t,
                              uint64_t free_bytes,
                              uint64_t model_bytes)
{
    if (!t || model_bytes == 0) return MS_EARG;
    if (model_bytes > UINT64_MAX - t->headroom_bytes) return MS_ELOWMEM;
    uint64_t needed = model_bytes + t->headroom_bytes;
    if (free_bytes < needed || free_bytes - model_bytes < t->low_watermark_bytes) {
        if (t->state == MS_RESIDENT) {
            t->state = MS_SHED;
            t->shed_count++;
        } else {
            t->state = MS_UNLOADED;
        }
        t->resident_bytes = 0;
        t->fallback_active = 1;
        return MS_ELOWMEM;
    }
    t->state = MS_RESIDENT;
    t->resident_bytes = model_bytes;
    t->fallback_active = 0;
    t->load_count++;
    return MS_OK;
}

static inline int ms_unload(struct ms_tracker *t,
                            ms_unload_fn unload_cb,
                            void *ctx,
                            int due_to_pressure)
{
    if (!t) return MS_EARG;
    if (t->state != MS_RESIDENT) {
        t->fallback_active = 1;
        return MS_OK;
    }
    if (unload_cb) unload_cb(ctx);
    t->resident_bytes = 0;
    t->fallback_active = 1;
    if (due_to_pressure) {
        t->state = MS_SHED;
        t->shed_count++;
    } else {
        t->state = MS_UNLOADED;
    }
    return MS_OK;
}

static inline int ms_check_memory_pressure(struct ms_tracker *t,
                                          uint64_t free_bytes,
                                          ms_unload_fn unload_cb,
                                          void *ctx)
{
    if (!t) return MS_EARG;
    if (t->state == MS_RESIDENT && free_bytes < t->low_watermark_bytes) {
        return ms_unload(t, unload_cb, ctx, 1);
    }
    return MS_OK;
}

static inline int ms_route_fallback(struct ms_tracker *t,
                                    const char *text,
                                    size_t len,
                                    const struct aura_caps *caps,
                                    struct aura_intent *intent_out)
{
    if (!t || !text || !intent_out) return MS_EARG;
    if (t->state == MS_RESIDENT) return MS_ESTATE;
    t->fallback_active = 1;
    t->fallback_count++;
    (void)aura_parse(text, len, caps, intent_out);
    if (!aura_validate(intent_out, caps)) return MS_EBADCMD;
    return MS_OK;
}

#endif
