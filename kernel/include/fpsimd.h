/*
 * kernel/include/fpsimd.h - per-process FP/SIMD state (AArch64). EL0 may use FP/SIMD (CPACR_EL1.FPEN=11); the kernel does not (-mgeneral-regs-only).
 * The scheduler saves the outgoing process's registers and loads the incoming process's at every switch, so no register value, FPSR flag or FPCR
 * mode ever crosses a process boundary. A new process starts with all-zero state (FPCR=0, FPSR=0). See docs/kernel/userspace.md.
 */
#ifndef AURORA_FPSIMD_H
#define AURORA_FPSIMD_H
#include <stdint.h>

struct fpstate {
    uint64_t v[64];                 /* V0..V31, low half first (128 bits each) */
    uint32_t fpsr, fpcr;
    uint32_t pad[2];                /* keeps the size a multiple of 16 */
} __attribute__((aligned(16)));

void fp_save(struct fpstate *s);    /* fpsimd.S */
void fp_restore(const struct fpstate *s);
#endif
