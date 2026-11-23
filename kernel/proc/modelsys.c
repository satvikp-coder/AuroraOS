/*
 * kernel/proc/modelsys.c - SYS_MODEL (11): read-only EL0 access to the objects of the model volume that modelvol_check() VERIFIED at boot.
 * ABI and error rules: kernel/include/modelsys.h. Design notes: docs/kernel/model-asset-store.md.
 *
 *  - Authority: only a process whose kernel-chosen principal is PRINCIPAL_INFER (put there by proc_set_service_principal, kernel code only). That check
 *    comes before anything else, for every op. Then the volume must be VALID; otherwise AE_ENOSYS (absent or failed its boot check).
 *  - Bounds: EL0 names an object INDEX and a byte range inside it, never a sector or a device. The extent is the verified table entry
 *    (modelvol_object()); the byte range is model_read_clamp() (pure, host-tested, no addition that could wrap); every sector read is re-checked
 *    against [first_sector, first_sector + sectors) before it reaches blkm_read().
 *  - Copies: copy_to_user only, after user_writable() has accepted the WHOLE destination, so a bad pointer is AE_EFAULT with nothing read or written.
 *  - State: none per process. One static 512-byte bounce sector, used only inside a single system call (system calls run to completion with
 *    interrupts masked on the one CPU, so it is never shared between two callers); a process that exits or faults between calls leaves nothing held.
 *  - What this does NOT give: the object bytes are not re-hashed on read. A host that rewrites the image file while the VM runs is not detected
 *    (the volume is attached read-only to the VM, which stops the guest writing it, not the host). Integrity was checked once at boot; it is not authenticity.
 */
#include <modelsys.h>
#include <modelvol.h>
#include <modelstore.h>
#include <principal.h>
#include <proc.h>
#include <blk.h>

_Static_assert(MODEL_CLAMP_EINVAL == AE_EINVAL && MODEL_CLAMP_E2BIG == AE_E2BIG, "model_read_clamp codes are the AE_* codes");
_Static_assert(sizeof(struct user_model_obj) == 80, "user_model_obj ABI");
_Static_assert(MODEL_READ_MAX <= SYS_FS_IO_MAX, "one read is at most one page-sized chunk");

static uint8_t BOUNCE[MS_SECTOR];

static int64_t info(uint64_t index, uint64_t uptr)
{
    if (index == MODEL_INFO_COUNT) return (int64_t)modelvol_objects();
    if (index >= modelvol_objects()) return AE_EINVAL;                       /* 64-bit compare: 2^32 is not object 0 */
    const struct ms_object *o = modelvol_object((uint32_t)index);
    if (!o) return AE_EINVAL;
    struct user_model_obj u;
    uint8_t *z = (uint8_t *)&u;
    for (unsigned i = 0; i < sizeof u; i++) z[i] = 0;                         /* no kernel stack bytes reach EL0, padding included */
    u.size = o->size; u.kind = o->kind; u.reserved = 0;
    for (unsigned i = 0; i < sizeof u.name && i <= MS_NAME_MAX; i++) u.name[i] = o->name[i];
    for (unsigned i = 0; i < 32; i++) u.digest[i] = o->digest[i];
    int rc = copy_to_user(uptr, &u, sizeof u);
    return rc == AE_OK ? 0 : rc;
}

static int64_t readobj(uint64_t index, uint64_t off, uint64_t ubuf, uint64_t len)
{
    if (index >= modelvol_objects()) return AE_EINVAL;
    const struct ms_object *o = modelvol_object((uint32_t)index);
    if (!o) return AE_EINVAL;
    int64_t n = model_read_clamp(o->size, off, len);
    if (n <= 0) return n;                                                      /* an error, or 0 at end of object */
    if (!user_writable(ubuf, (size_t)n)) return AE_EFAULT;                    /* the whole destination, before any device read */
    /* off < size <= sectors * 512 and first_sector + sectors <= total_sectors <= 2^40 (ms_parse), so none of these sums can wrap. */
    uint64_t end = o->first_sector + o->sectors, done = 0;
    while (done < (uint64_t)n) {
        uint64_t pos = off + done, sec = o->first_sector + pos / MS_SECTOR, in = pos % MS_SECTOR;
        if (sec < o->first_sector || sec >= end) return AE_EIO;               /* unreachable by the clamp; the extent stays the last word */
        if (blkm_read(sec, BOUNCE, 1) != BLK_OK) return AE_EIO;
        uint64_t c = MS_SECTOR - in;
        if (c > (uint64_t)n - done) c = (uint64_t)n - done;
        int rc = copy_to_user(ubuf + done, BOUNCE + in, (size_t)c);
        if (rc != AE_OK) return rc;
        done += c;
    }
    return n;
}

int64_t model_syscall(struct proc *p, uint64_t op, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
    if (p->principal != PRINCIPAL_INFER) return AE_EPERM;
    if (modelvol_state() != MODELVOL_VALID) return AE_ENOSYS;
    switch (op) {
    case MODELOP_INFO: return info(a1, a2);
    case MODELOP_READ: return readobj(a1, a2, a3, a4);
    default: return AE_EINVAL;
    }
}
