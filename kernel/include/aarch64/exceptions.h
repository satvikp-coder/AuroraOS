/*
 * kernel/include/aarch64/exceptions.h — ARM64 exception declarations
 *
 * Declares the exception vector table and fault reporting interface.
 */

#ifndef AURORA_AARCH64_EXCEPTIONS_H
#define AURORA_AARCH64_EXCEPTIONS_H

#include <stdint.h>

/* Exception vector table (defined in exceptions.S) */
extern void *exception_vector_table[];

/* Fault handler — called from assembly on synchronous exception */
void fault_handler(uint64_t esr, uint64_t far);

#endif /* AURORA_AARCH64_EXCEPTIONS_H */
