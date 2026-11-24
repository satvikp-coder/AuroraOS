/*
 * kernel/include/principal.h - the ONE policy for which file/service identities (principals) a process may ever hold.
 *
 * A principal is a 32-bit identity assigned to a process by KERNEL code only (a launch registration or proc_set_principal); EL0 can neither choose nor
 * change it. The range 0x7F000000..0x7FFFFFFF is RESERVED for trusted kernel services and file owners. No process may hold a reserved identity, ever:
 *   0x7F000001 installer (owns installed executables)       0x7F000002 settings (owns settings.v1)
 *   0x7F000003 the trusted shell (requester pid 0, not a process)
 *   0x7F000004 Passport (owns the permission ledger files)  0x7F000005 Trail (owns the audit ledger files)
 * and never 0 (the kernel) or PRINCIPAL_NONE (no identity). Every place that assigns an identity to a process or grants a capability to a principal
 * must call principal_assignable() so a future reserved number is protected automatically, not by a hand-kept list.
 *
 * Pure and header-only so the host tests exercise the real code.
 */
#ifndef AURORA_PRINCIPAL_H
#define AURORA_PRINCIPAL_H
#include <stdint.h>

#define PRINCIPAL_RESERVED_BASE 0x7F000000u
#define PRINCIPAL_RESERVED_MASK 0xFF000000u
#define PRINCIPAL_SHELL 0x7F000003u
#define PRINCIPAL_PASSPORT 0x7F000004u
#define PRINCIPAL_TRAIL 0x7F000005u

static inline int principal_is_reserved(uint32_t p) { return (p & PRINCIPAL_RESERVED_MASK) == PRINCIPAL_RESERVED_BASE; }
/* 1 only for an identity a process (or an application capability) may hold: not 0, not "none" (0xFFFFFFFF), not in the reserved range. */
static inline int principal_assignable(uint32_t p) { return p != 0 && p != 0xFFFFFFFFu && !principal_is_reserved(p); }
#endif
