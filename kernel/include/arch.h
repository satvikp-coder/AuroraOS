#ifndef AURORA_ARCH_H
#define AURORA_ARCH_H
#include <stdint.h>

/* Register frame pushed by the exception vectors (must match exceptions.S, 288 bytes). */
struct exc_frame {
    uint64_t x[31];
    uint64_t elr, spsr, esr, far;
    uint64_t kind;
};

extern char exception_vectors[];
void exception_handler(struct exc_frame *f);
void exceptions_init(void);          /* installs VBAR_EL1 */

static inline uint64_t read_sctlr_el1(void) { uint64_t v; __asm__ volatile ("mrs %0, sctlr_el1" : "=r"(v)); return v; }
static inline uint64_t read_current_el(void) { uint64_t v; __asm__ volatile ("mrs %0, CurrentEL" : "=r"(v)); return v >> 2; }

#endif
