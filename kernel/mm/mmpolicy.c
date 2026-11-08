/*
 * kernel/mm/mmpolicy.c — physical-target policy (see mmpolicy.h). Pure logic, no hardware access.
 *
 * Rules, in the order they are evaluated:
 *   1. size 0 or pa+size overflow                       -> deny
 *   2. any overlap with a no-map region                 -> deny, for EVERY attribute
 *   3. Device attribute:  must lie wholly inside ONE registered MMIO window and must not touch RAM
 *   4. Normal attributes: must lie wholly inside RAM (so never on MMIO or on unknown space)
 *        RW  may not overlap kernel text or kernel rodata        (no writable alias of protected pages)
 *        RX  must lie wholly inside kernel text                  (the only executable memory; keeps W^X
 *                                                                 across aliases of data/stack/free RAM)
 *        RO  is always fine inside RAM (a read-only alias cannot weaken anything)
 * A request that mixes protected and unprotected pages is judged as a whole: one protected page denies it.
 */
#include <mmpolicy.h>

static int overlaps(uint64_t a0, uint64_t a1, const struct mm_range *r) { return a0 < r->end && r->start < a1; }
static int within(uint64_t a0, uint64_t a1, const struct mm_range *r) { return a0 >= r->start && a1 <= r->end; }

int mm_policy_check(const struct mm_policy *p, uint64_t pa, uint64_t size, int attr)
{
    if (size == 0 || pa + size < pa) return MMP_DENY_OVERFLOW;
    uint64_t end = pa + size;
    if (attr < MMP_ATTR_NORMAL_RW || attr > MMP_ATTR_DEVICE) return MMP_DENY_ATTR;

    for (int i = 0; i < p->nnomap; i++)
        if (overlaps(pa, end, &p->nomap[i])) return MMP_DENY_NOMAP;

    if (attr == MMP_ATTR_DEVICE) {
        if (overlaps(pa, end, &p->ram)) return MMP_DENY_DEVICE_ON_RAM;
        for (int i = 0; i < p->nmmio; i++)
            if (within(pa, end, &p->mmio[i])) return MMP_OK;
        return MMP_DENY_DEVICE_OUTSIDE_WINDOW;
    }

    if (!within(pa, end, &p->ram)) {
        for (int i = 0; i < p->nmmio; i++)
            if (overlaps(pa, end, &p->mmio[i])) return MMP_DENY_NORMAL_ON_MMIO;
        return MMP_DENY_NORMAL_OUTSIDE_RAM;
    }
    if (attr == MMP_ATTR_NORMAL_RW) {
        if (overlaps(pa, end, &p->text)) return MMP_DENY_TEXT_WRITE;
        if (overlaps(pa, end, &p->rodata)) return MMP_DENY_RODATA_WRITE;
    } else if (attr == MMP_ATTR_NORMAL_RX) {
        if (!within(pa, end, &p->text)) return MMP_DENY_EXEC_OUTSIDE_TEXT;
    }
    return MMP_OK;
}

const char *mm_policy_reason(int rc)
{
    switch (rc) {
    case MMP_OK: return "allowed";
    case MMP_DENY_OVERFLOW: return "empty range or address overflow";
    case MMP_DENY_ATTR: return "unknown attribute";
    case MMP_DENY_NOMAP: return "device-tree no-map region";
    case MMP_DENY_NORMAL_ON_MMIO: return "cacheable mapping of a device window";
    case MMP_DENY_NORMAL_OUTSIDE_RAM: return "cacheable mapping outside RAM";
    case MMP_DENY_DEVICE_ON_RAM: return "device attributes on RAM";
    case MMP_DENY_DEVICE_OUTSIDE_WINDOW: return "device mapping outside registered windows";
    case MMP_DENY_TEXT_WRITE: return "writable alias of kernel text";
    case MMP_DENY_RODATA_WRITE: return "writable alias of kernel rodata";
    case MMP_DENY_EXEC_OUTSIDE_TEXT: return "executable mapping outside kernel text";
    case MMP_DENY_BAD_REGISTRATION: return "bad policy registration";
    default: return "unknown";
    }
}

int mm_policy_add_nomap(struct mm_policy *p, uint64_t addr, uint64_t size)
{
    if (size == 0 || addr + size < addr || p->nnomap >= MMP_MAX_NOMAP) return MMP_DENY_BAD_REGISTRATION;
    p->nomap[p->nnomap].start = addr; p->nomap[p->nnomap].end = addr + size; p->nnomap++;
    return MMP_OK;
}

int mm_policy_add_mmio(struct mm_policy *p, uint64_t addr, uint64_t size)
{
    if (size == 0 || addr + size < addr || p->nmmio >= MMP_MAX_MMIO) return MMP_DENY_BAD_REGISTRATION;
    uint64_t end = addr + size;
    if (overlaps(addr, end, &p->ram)) return MMP_DENY_BAD_REGISTRATION;            /* a device window can never be RAM */
    for (int i = 0; i < p->nnomap; i++) if (overlaps(addr, end, &p->nomap[i])) return MMP_DENY_BAD_REGISTRATION;
    p->mmio[p->nmmio].start = addr; p->mmio[p->nmmio].end = end; p->nmmio++;
    return MMP_OK;
}
