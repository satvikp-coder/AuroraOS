/*
 * kernel/include/procsnap.h - read-only process snapshot for the native Pages screen (issue #9).
 *
 * procsnap_take() copies a bounded, sanitised view of the process table into caller storage. It never writes to a process, never allocates, and
 * never exposes anything a viewer must not hold: no kernel or physical addresses, no saved registers, no principal, no capability, no IPC
 * grant. A process is identified by (pid, gen): the pid is a reusable slot, gen is unique per process instance, so a viewer can tell a new process
 * from the old one that had the same pid. The table is read through a getter so the module is host-testable with a fake table.
 */
#ifndef AURORA_PROCSNAP_H
#define AURORA_PROCSNAP_H
#include <proc.h>

#define PROCSNAP_NAME 16

/* View state. Terminated and faulted processes stay visible until the owner reaps them; "unused" is never listed. */
enum { PSS_NEW = 1, PSS_READY, PSS_RUNNING, PSS_BLOCKED, PSS_EXITED, PSS_FAULTED };

struct procsnap_entry {
    uint8_t  pid, state;
    uint64_t gen;
    char     name[PROCSNAP_NAME];            /* printable ASCII only, always NUL-terminated */
    uint64_t ticks, switches, yields, preemptions;
    uint32_t pages;                          /* physical pages the address space owns (tables and user pages) */
    int32_t  exit_code;                      /* meaningful only for PSS_EXITED */
};

struct procsnap {
    uint32_t n;                              /* entries valid, ascending pid */
    struct procsnap_entry e[PROC_MAX];
};

typedef const struct proc *(*procsnap_get_fn)(int pid);

/* Fill *out from the table; returns the number of entries. A NULL out or getter returns 0 and writes nothing. */
uint32_t procsnap_take(struct procsnap *out, procsnap_get_fn get);
/* The kernel's table. */
uint32_t procsnap_take_kernel(struct procsnap *out);
const char *procsnap_state_name(uint8_t state);
/* A 64-bit digest of everything that is displayed (used to repaint only when something changed). */
uint64_t procsnap_digest(const struct procsnap *s);

#endif
