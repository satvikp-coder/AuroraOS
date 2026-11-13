/* kernel/proc/proc.c — process table, creation, destruction, explicit state machine, safe user-memory access. */
#include <console.h>
#include <elf.h>
#include <mm.h>
#include <proc.h>

static struct proc table[PROC_MAX];
static uint64_t next_gen = 1;                    /* generation 0 means "no process"; 64 bits never wrap in practice */

const char *proc_state_name(enum proc_state s)
{
    switch (s) {
    case PROC_UNUSED: return "UNUSED"; case PROC_NEW: return "NEW"; case PROC_READY: return "READY";
    case PROC_RUNNING: return "RUNNING"; case PROC_BLOCKED: return "BLOCKED";
    case PROC_TERMINATED: return "TERMINATED"; case PROC_FAULTED: return "FAULTED";
    }
    return "?";
}

/* The only legal transitions. A TERMINATED or FAULTED process never runs again. */
int proc_transition_ok(enum proc_state from, enum proc_state to)
{
    switch (from) {
    case PROC_UNUSED:     return to == PROC_NEW;
    case PROC_NEW:        return to == PROC_READY || to == PROC_UNUSED;
    case PROC_READY:      return to == PROC_RUNNING || to == PROC_UNUSED;
    case PROC_RUNNING:    return to == PROC_READY || to == PROC_BLOCKED || to == PROC_TERMINATED || to == PROC_FAULTED;
    case PROC_BLOCKED:    return to == PROC_READY || to == PROC_FAULTED || to == PROC_UNUSED;
    case PROC_TERMINATED: return to == PROC_UNUSED;
    case PROC_FAULTED:    return to == PROC_UNUSED;
    }
    return 0;
}

struct proc *proc_get(int pid) { return (pid >= 1 && pid <= PROC_MAX && table[pid - 1].state != PROC_UNUSED) ? &table[pid - 1] : 0; }

static void kzero(void *p, size_t n) { volatile uint8_t *b = (volatile uint8_t *)p; for (size_t i = 0; i < n; i++) b[i] = 0; }

int proc_create(int role, uint64_t arg1, const char *name)
{
    return proc_create_image(role, arg1, name, user_blob_start, (size_t)(user_blob_end - user_blob_start));
}

/* Create a process whose single code page is `image` (len <= one page, kernel-supplied bytes). The process is READY only after EVERY mapping
 * succeeded; on any failure everything allocated so far is released and no slot stays in use. */
int proc_create_image(int role, uint64_t arg1, const char *name, const void *image, size_t len)
{
    int slot = -1;
    for (int i = 0; i < PROC_MAX; i++) if (table[i].state == PROC_UNUSED) { slot = i; break; }
    if (slot < 0) return -1;
    struct proc *p = &table[slot];
    kzero(p, sizeof *p);
    p->pid = slot + 1;                                        /* the ASID is the pid */
    p->principal = PRINCIPAL_NONE;                            /* a new process has NO file identity until kernel code grants one */
    p->gen = next_gen++;                                      /* identifies THIS instance; grants name it, not the reusable pid */
    p->state = PROC_NEW;
    for (int i = 0; i < 15 && name && name[i]; i++) p->name[i] = name[i];

    int rc = aspace_create(&p->as, (uint32_t)p->pid);
    if (rc == 0) rc = aspace_map(&p->as, USER_CODE_VA, UPERM_RX, image, len, &p->code_pa);
    if (rc == 0) rc = aspace_map(&p->as, USER_DATA_VA, UPERM_RW, 0, 0, &p->data_pa);
    if (rc == 0) rc = aspace_map(&p->as, USER_STACK_VA, UPERM_RW, 0, 0, &p->stack_pa);
    if (rc == 0) rc = aspace_map(&p->as, USER_STACK_VA + PAGE_SIZE, UPERM_RW, 0, 0, 0);
    if (rc) { aspace_destroy(&p->as); p->state = PROC_UNUSED; return -2; }

    p->entry = USER_CODE_VA;
    p->ctx.elr = USER_CODE_VA;
    p->ctx.spsr = 0;                                          /* EL0t, AArch64, DAIF clear: IRQs are taken in user mode */
    p->ctx.sp_el0 = USER_STACK_TOP;
    p->ctx.x[0] = (uint64_t)role;
    p->ctx.x[1] = arg1;
    p->state = PROC_READY;
    return p->pid;
}

/*
 * Create a process from a validated executable image (the program loader's back end).
 *
 * `file` is UNTRUSTED: it is validated here, by elf_validate, every time, so a caller cannot hand in a description that does not match the bytes.
 * `principal` is chosen by the KERNEL caller (the launcher's registry), never taken from the file. Order of work, and why it is safe:
 *   1. validate everything (no memory touched); 2. take a slot in state NEW (the scheduler only ever runs READY processes); 3. build the address
 *   space page by page: a fresh zeroed kernel page buffer is filled with the file-backed bytes that fall in that page, the rest is zero (this is
 *   the bss and the tail of the last file page), and the page is mapped with permissions derived from the validated flags (R+X -> RX, R+W -> RW
 *   and never executable, R -> RO); 4. map the stack; 5. only now set the registers, the identity and READY.
 * Any failure destroys the address space (which frees every page it owns) and releases the slot: nothing half-built is ever runnable and nothing
 * leaks. Returns the pid, or PROC_ELOAD_* (< 0); *elf_err receives the elf_validate code for PROC_ELOAD_INVALID.
 */
static uint8_t load_page[PAGE_SIZE];

int proc_create_elf(const uint8_t *file, size_t len, const char *name, uint32_t principal, uint64_t arg, int *elf_err)
{
    if (elf_err) *elf_err = 0;
    if (principal == 0 || principal == PRINCIPAL_NONE) return PROC_ELOAD_ARG;
    struct elf_image img;
    int ve = elf_validate(file, len, &img);
    if (ve) { if (elf_err) *elf_err = ve; return PROC_ELOAD_INVALID; }

    int slot = -1;
    for (int i = 0; i < PROC_MAX; i++) if (table[i].state == PROC_UNUSED) { slot = i; break; }
    if (slot < 0) return PROC_ELOAD_NOSLOT;
    struct proc *p = &table[slot];
    kzero(p, sizeof *p);
    p->pid = slot + 1;
    p->principal = PRINCIPAL_NONE;
    p->gen = next_gen++;
    p->state = PROC_NEW;
    for (int i = 0; i < 15 && name && name[i]; i++) p->name[i] = name[i];

    int rc = aspace_create(&p->as, (uint32_t)p->pid);
    for (int s = 0; rc == 0 && s < img.nseg; s++) {
        const struct elf_seg *g = &img.seg[s];
        int perm = (g->flags & ELF_PF_X) ? UPERM_RX : (g->flags & ELF_PF_W) ? UPERM_RW : UPERM_RO;
        uint64_t file_lo = g->vaddr, file_hi = g->vaddr + g->filesz, end = g->vaddr + g->memsz;
        for (uint64_t pg = g->vaddr & ~(uint64_t)(PAGE_SIZE - 1); rc == 0 && pg < end; pg += PAGE_SIZE) {
            for (size_t i = 0; i < PAGE_SIZE; i++) load_page[i] = 0;
            uint64_t lo = pg > file_lo ? pg : file_lo, hi = pg + PAGE_SIZE < file_hi ? pg + PAGE_SIZE : file_hi;
            for (uint64_t a = lo; a < hi; a++) load_page[a - pg] = file[g->offset + (a - g->vaddr)];     /* hi <= file_hi: inside the validated file range */
            rc = aspace_map(&p->as, pg, perm, load_page, PAGE_SIZE, 0);
        }
    }
    if (rc == 0) rc = aspace_map(&p->as, USER_STACK_VA, UPERM_RW, 0, 0, &p->stack_pa);
    if (rc == 0) rc = aspace_map(&p->as, USER_STACK_VA + PAGE_SIZE, UPERM_RW, 0, 0, 0);
    if (rc) { aspace_destroy(&p->as); kzero(p, sizeof *p); return PROC_ELOAD_NOMEM; }

    p->entry = img.entry;
    p->ctx.elr = img.entry;
    p->ctx.spsr = 0;
    p->ctx.sp_el0 = USER_STACK_TOP;
    p->ctx.x[0] = arg;                                       /* a number chosen by the kernel (the launch registry); the program receives it as its first argument */
    p->principal = principal;
    p->state = PROC_READY;                                   /* the LAST step: until here the scheduler cannot see it */
    return p->pid;
}

int proc_destroy(int pid)
{
    struct proc *p = proc_get(pid);
    if (!p) return -1;
    if (!proc_transition_ok(p->state, PROC_UNUSED)) return -2;     /* e.g. RUNNING: refuse */
    ipc_cleanup(p);
    aspace_destroy(&p->as);
    kzero(p, sizeof *p);                                      /* state = UNUSED */
    return 0;
}

/* ---------------- safe access to user memory ---------------- */

int user_range_ok(uint64_t uva, size_t n)
{
    if (n == 0) return 1;
    if (uva + n < uva) return 0;                              /* overflow */
    return uva >= USER_VA_BASE && uva + n <= USER_VA_LIMIT;   /* only the user slot, never kernel addresses */
}

/*
 * Copies n bytes from user memory. For every page the hardware is asked (AT S1E0R, with the calling process's tables
 * live) whether EL0 could read it, and the bytes are then read through the kernel's identity map of the resulting
 * physical page. So no kernel instruction ever touches a user virtual address, a bad pointer cannot fault the
 * kernel, and kernel-only pages are refused because the EL0 translation fails.
 */
int copy_from_user(void *dst, uint64_t uva, size_t n)
{
    if (n == 0) return AE_OK;
    if (!dst || !user_range_ok(uva, n)) return AE_EFAULT;
    uint8_t *d = (uint8_t *)dst;
    while (n) {
        uint64_t pa;
        if (vmm_hw_translate_el0(uva, 0, &pa) != 0) return AE_EFAULT;
        size_t chunk = PAGE_SIZE - (size_t)(uva & (PAGE_SIZE - 1));
        if (chunk > n) chunk = n;
        const volatile uint8_t *s = (const volatile uint8_t *)(uintptr_t)pa;
        for (size_t i = 0; i < chunk; i++) d[i] = s[i];
        d += chunk; uva += chunk; n -= chunk;
    }
    return AE_OK;
}

int user_writable(uint64_t uva, size_t n)
{
    if (!user_range_ok(uva, n)) return 0;
    uint64_t pa;
    for (uint64_t a = uva & ~(PAGE_SIZE - 1); n && a < uva + n; a += PAGE_SIZE)
        if (vmm_hw_translate_el0(a < uva ? uva : a, 1, &pa) != 0) return 0;
    return 1;
}

/* Mirror of copy_from_user for the other direction: AT S1E0W asks the hardware whether EL0 may WRITE each page. */
int copy_to_user(uint64_t uva, const void *src, size_t n)
{
    if (n == 0) return AE_OK;
    if (!src || !user_range_ok(uva, n)) return AE_EFAULT;
    if (!user_writable(uva, n)) return AE_EFAULT;                  /* check everything first: no partial copy on failure */
    const uint8_t *s = (const uint8_t *)src;
    while (n) {
        uint64_t pa;
        if (vmm_hw_translate_el0(uva, 1, &pa) != 0) return AE_EFAULT;
        size_t chunk = PAGE_SIZE - (size_t)(uva & (PAGE_SIZE - 1));
        if (chunk > n) chunk = n;
        volatile uint8_t *d = (volatile uint8_t *)(uintptr_t)pa;
        for (size_t i = 0; i < chunk; i++) d[i] = s[i];
        s += chunk; uva += chunk; n -= chunk;
    }
    return AE_OK;
}

int proc_set_principal(int pid, uint32_t principal)
{
    struct proc *p = proc_get(pid);
    if (!p || principal == 0 || principal == PRINCIPAL_NONE) return -1;
    p->principal = principal;
    return 0;
}
