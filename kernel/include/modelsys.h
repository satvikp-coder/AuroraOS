/*
 * kernel/include/modelsys.h - SYS_MODEL (11): bounded, read-only EL0 access to the VERIFIED objects of the model volume (kernel/proc/modelsys.c).
 *
 *   x8 = SYS_MODEL, x0 = op:
 *   MODELOP_INFO  x1 = index, x2 = user pointer to struct user_model_obj        -> 0, and the record is written
 *                 x1 = MODEL_INFO_COUNT (exactly 0xFFFFFFFF)                    -> the number of objects (x2 ignored)
 *   MODELOP_READ  x1 = index, x2 = byte offset, x3 = user buffer, x4 = length    -> bytes copied: min(len, size - offset); 0 at offset == size
 * Errors: AE_EPERM unless the caller's kernel-chosen principal is PRINCIPAL_INFER (checked FIRST, for every op, so an unauthorised process learns
 * nothing, not even whether a volume exists); AE_ENOSYS unless the boot check found the volume valid (modelvol_state() == MODELVOL_VALID);
 * AE_EINVAL for an unknown op, an index that is not an object (64-bit compare: no truncation), length 0 or offset > size; AE_E2BIG for
 * length > MODEL_READ_MAX; AE_EFAULT for a user pointer EL0 may not write; AE_EIO if the device fails a (kernel-bounded) read.
 * EL0 never names a sector, a device or an extent: the extent is the verified table entry.
 *
 * model_read_clamp() is the pure range rule, header-only so tests/kernel-host/modelsys_test.c exercises the real code.
 */
#ifndef AURORA_MODELSYS_H
#define AURORA_MODELSYS_H
#include <stdint.h>

#define SYS_MODEL 11
enum { MODELOP_INFO = 1, MODELOP_READ = 2 };
#define MODEL_INFO_COUNT 0xFFFFFFFFull
#define MODEL_READ_MAX 4096u

struct user_model_obj { uint64_t size; uint32_t kind; uint32_t reserved; char name[32]; uint8_t digest[32]; };   /* 80 bytes, written to user memory */

#define MODEL_CLAMP_EINVAL (-1)          /* == AE_EINVAL */
#define MODEL_CLAMP_E2BIG  (-3)          /* == AE_E2BIG */
/* Bytes a read of `len` at `off` from an object of `size` bytes may return, or a negative error. No addition is ever performed, so nothing can wrap:
 * len 0 -> EINVAL; len > MODEL_READ_MAX -> E2BIG; off > size -> EINVAL; else min(len, size - off) (0 exactly at off == size). */
static inline int64_t model_read_clamp(uint64_t size, uint64_t off, uint64_t len)
{
    if (len == 0) return MODEL_CLAMP_EINVAL;
    if (len > MODEL_READ_MAX) return MODEL_CLAMP_E2BIG;
    if (off > size) return MODEL_CLAMP_EINVAL;
    uint64_t left = size - off;
    return (int64_t)(len < left ? len : left);
}

struct proc;
int64_t model_syscall(struct proc *p, uint64_t op, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4);
void modeltest_run(void);        /* AURORA_MODEL_TEST kernels only (kernel/modeltest.c) */
int  modeltest_summary(void);
#endif
