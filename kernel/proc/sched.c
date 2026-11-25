/*
 * kernel/proc/sched.c — round-robin scheduler and the EL0 system-call/fault path.
 *
 * Everything here runs inside an exception handler, on the SP_EL1 exception stack, with IRQs masked by hardware.
 * A context switch is: save the interrupted frame into the outgoing process, install the next process's frame
 * over the live frame (*f = next->ctx), switch TTBR0, and RETURN: the assembly epilogue in vectors.S then restores
 * x0-x30, SP_EL0, ELR and SPSR from that frame and ERETs into the incoming process. Nothing ever "calls" a switch
 * function that returns on another stack, so no kernel stack is shared or swapped.
 *
 * The kernel boot context enters with `svc #0x5c4e` (sched_run): its frame is parked in kctx and restored, with the
 * SCHED_* result in x0, when nothing is runnable (all processes done, or the rest are blocked) or the tick limit expires.
 */
#include <console.h>
#include <irq.h>
#include <mm.h>
#include <proc.h>
#include <timer.h>
#include <timersys.h>
#include <modelsys.h>
#include <ui_sys.h>
#include <passport.h>
#include <trail.h>
#include <ledger.h>
#include <aura_exec.h>
#include <review.h>
#include <broker_kernel.h>
#include <aura_cmd.h>

#define SVC_ENTER_SCHED 0x5c4e
#define SVC_ENTER_SLICE 0x5c4f   /* a bounded slice for the interactive shell: keeps the tick count and the round-robin position */

static int active, cur_pid;                    /* cur_pid: 0 while no user process owns the CPU */
static int fp_owner; static uint64_t fp_owner_gen;   /* the process whose FP/SIMD registers are live in the CPU right now (0 = none), and its instance */
static int rr_cursor;                          /* pid that last ran: where the next round-robin search starts when cur_pid is 0 (a new slice) */
static struct exc_frame kctx;
static uint64_t ticks, limit, slice;
static int preempt, need_resched, timed_out;
static uint32_t quantum = 5;                   /* ticks per time slice when preemption is on */
static void (*saved_hook)(void);
static struct sched_stats stats;
static char trace[SCHED_TRACE_MAX];
static int trace_n;

const char *sched_trace(int *len) { *len = trace_n; return trace; }
void sched_trace_reset(void) { trace_n = 0; }
int sched_active(void) { return active; }
const struct sched_stats *sched_get_stats(void) { return &stats; }
void sched_stats_reset(void) { stats = (struct sched_stats){ 0 }; }
uint64_t sched_ticks(void) { return ticks; }
void sched_set_preempt(int enabled, uint32_t q) { preempt = enabled; if (q) quantum = q; }

static void bug(const char *msg)
{
    console_puts("\n[sched] KERNEL BUG: "); console_puts(msg); console_puts("\nKERNEL PANIC: scheduler invariant violated.\n");
    for (;;) __asm__ volatile ("msr daifset, #0xf\n wfi");
}

static void set_state(struct proc *p, enum proc_state to)
{
    if (!proc_transition_ok(p->state, to)) {
        console_puts("[sched] illegal transition "); console_puts(proc_state_name(p->state)); console_puts(" -> "); console_puts(proc_state_name(to)); console_puts("\n");
        bug("illegal process state transition");
    }
    p->state = to;
}

static void set_ttbr0(uint64_t v) { __asm__ volatile ("msr ttbr0_el1, %0\n isb" :: "r"(v) : "memory"); }

static void tick_hook(void)
{
    ticks++;
    if (limit && ticks >= limit) timed_out = 1;
    if (cur_pid) { struct proc *p = proc_get(cur_pid); if (p) p->ticks_run++; }
    if (preempt && ++slice >= quantum) need_resched = 1;
}

static struct proc *current(void) { return cur_pid ? proc_get(cur_pid) : 0; }

static int pick_next(void)
{   /* round robin: the first READY process after the current one; the current one (if READY again) comes last */
    for (int i = 1; i <= PROC_MAX; i++) {
        int pid = ((cur_pid ? cur_pid : (rr_cursor ? rr_cursor : PROC_MAX)) - 1 + i) % PROC_MAX + 1;
        struct proc *p = proc_get(pid);
        if (p && p->state == PROC_READY) return pid;
    }
    return 0;
}

static int any_blocked(void)
{
    for (int pid = 1; pid <= PROC_MAX; pid++) { struct proc *p = proc_get(pid); if (p && p->state == PROC_BLOCKED) return 1; }
    return 0;
}

static void return_to_kernel(struct exc_frame *f, uint64_t reason)
{
    timer_stop();
    timer_set_tick_hook(saved_hook);
    set_ttbr0(vmm_kernel_l1_pa());                           /* back to the master table, ASID 0 */
    *f = kctx;                                               /* the SVC caller resumes after its svc ... */
    f->x[0] = reason;                                        /* ... with the result in x0 */
    active = 0; cur_pid = 0; need_resched = 0;
}

static void switch_to(struct exc_frame *f, int pid)
{
    struct proc *p = proc_get(pid);
    set_state(p, PROC_RUNNING);
    p->switches_in++; stats.switches++;
    /* FP/SIMD: the live registers belong to fp_owner (the kernel never touches them). Save them into THAT process, but only if the slot still holds the
     * same instance: a destroyed and re-created slot must never receive the old process's registers. Then load the incoming process's own state. */
    if (fp_owner) { struct proc *o = proc_get(fp_owner); if (o && o->gen == fp_owner_gen && o->state != PROC_UNUSED) fp_save(&o->fp); }
    fp_restore(&p->fp); fp_owner = pid; fp_owner_gen = p->gen;
    cur_pid = pid; rr_cursor = pid; slice = 0; need_resched = 0;
    set_ttbr0(aspace_ttbr(&p->as));                          /* kernel slots are identical in every table: this code keeps running */
    *f = p->ctx;
    if (p->rx_pending) {                                     /* a woken receiver: finish its receive now, with ITS page tables live */
        uint64_t from = 0;
        int r = ipc_dequeue(p, p->rx_buf, p->rx_max, &from);
        if (r == AE_EAGAIN) bug("receiver woken with an empty mailbox");
        f->x[0] = (uint64_t)(int64_t)r; f->x[1] = from;
        p->rx_pending = 0;
    }
}

/* The current process (if any) has just left RUNNING (and, if READY, had its context saved). Pick what runs next. */
static void reschedule(struct exc_frame *f)
{
    int next = pick_next();
    if (!next) { return_to_kernel(f, any_blocked() ? SCHED_DEADLOCK : SCHED_ALL_DONE); return; }
    switch_to(f, next);
}

uint64_t sched_run(uint64_t limit_ticks)
{
    register uint64_t r __asm__("x0") = limit_ticks;
    __asm__ volatile ("svc #0x5c4e" : "+r"(r) :: "memory");
    return r;
}

uint64_t sched_run_slice(uint64_t slice_ticks)
{
    register uint64_t r __asm__("x0") = slice_ticks;
    __asm__ volatile ("svc #0x5c4f" : "+r"(r) :: "memory");
    return r;
}

int sched_has_ready(void)
{
    for (int pid = 1; pid <= PROC_MAX; pid++) { struct proc *p = proc_get(pid); if (p && p->state == PROC_READY) return 1; }
    return 0;
}

int sched_handle_kernel_svc(struct exc_frame *f, uint32_t imm)
{
    if (imm != SVC_ENTER_SCHED && imm != SVC_ENTER_SLICE) return 1;
    if (active) bug("sched_run called while the scheduler is running");
    kctx = *f;
    if (imm == SVC_ENTER_SLICE) {                            /* a slice of the interactive shell: ticks and the round-robin cursor carry over */
        if (f->x[0] == 0 || f->x[0] > 1000) bug("sched_run_slice: the slice must be 1..1000 ticks");
        limit = ticks + f->x[0];
    } else { limit = f->x[0]; ticks = 0; rr_cursor = 0; }
    slice = 0; timed_out = 0; need_resched = 0; cur_pid = 0;
    active = 1;
    saved_hook = 0;
    timer_set_tick_hook(tick_hook);
    timer_start_periodic(1000);
    reschedule(f);
    return 0;
}

static void user_print(int pid, const char *buf, size_t n)
{   /* Each write becomes exactly one line that starts with a kernel-written tag, so user text can never forge a
     * kernel/test line; control bytes are replaced. */
    console_puts("[user p"); console_dec((uint64_t)pid); console_puts("] ");
    for (size_t i = 0; i < n; i++) { char c = buf[i]; console_putc((c >= 0x20 && c < 0x7f) ? c : '.'); }
    console_puts("\n");
}

/* ---- SYS_PASSPORT syscall handler ---- */
static int64_t passport_syscall(struct exc_frame *f, struct proc *p, uint64_t op, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
    (void)a3; (void)a4;
    const struct passport *pp = ledger_passport();
    if (!pp || !ledger_passport_ok()) return AE_EPERM;
    
    if (p->principal == PRINCIPAL_NONE) return AE_EPERM;
    
    switch ((int)op) {
    case PP_OP_CHECK: {  /* x1 = perm, x2 = gen -> x0 = 1/0, x1 = grant seq on deny */
        uint8_t perm = (uint8_t)a1;
        (void)a2;  /* gen is ignored, we use p->gen */
        if (perm == PP_NONE || perm >= PP_COUNT) return AE_EINVAL;
        int allowed = passport_el0_check((struct passport *)pp, p->principal, p->gen, perm);
        if (!allowed) {
            /* Find the denial record sequence number for the caller */
            uint32_t denial_count = passport_denial_count(pp);
            if (denial_count > 0) {
                const struct pp_audit_rec *rec = passport_denial_at(pp, denial_count - 1);
                if (rec && rec->principal == p->principal) {
                    f->x[1] = rec->seq;
                }
            }
        }
        return allowed ? 1 : 0;
    }
    case PP_OP_LIST: {  /* x1 = user buffer, x2 = max_count -> x0 = count written */
        struct pp_cap_entry *buf = (struct pp_cap_entry *)a1;
        uint32_t max_count = (uint32_t)a2;
        if (!buf || max_count == 0) return AE_EINVAL;
        if (!user_writable(a1, max_count * sizeof(struct pp_cap_entry))) return AE_EFAULT;
        
        struct pp_cap_entry tmp[PP_MAX_GRANTS];
        uint32_t count = passport_el0_list(pp, p->principal, tmp, max_count);
        int rc = copy_to_user(a1, tmp, count * sizeof(struct pp_cap_entry));
        if (rc != AE_OK) return rc;
        return (int64_t)count;
    }
    case PP_OP_AUDIT: {  /* x1 = index, x2 = user buffer -> x0 = 1/0 */
        uint32_t index = (uint32_t)a1;
        struct pp_audit_rec *buf = (struct pp_audit_rec *)a2;
        if (!buf) return AE_EINVAL;
        if (!user_writable(a2, sizeof(struct pp_audit_rec))) return AE_EFAULT;
        
        struct pp_audit_rec rec;
        int found = passport_el0_audit(pp, index, &rec);
        if (!found) return 0;
        return copy_to_user(a2, &rec, sizeof(rec)) == AE_OK ? 1 : AE_EFAULT;
    }
    default:
        return AE_ENOSYS;
    }
}

/* ---- SYS_TRAIL syscall handler ---- */
static int64_t trail_syscall(struct exc_frame *f, struct proc *p, uint64_t op, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
    (void)a4;
    const struct trail *tr = ledger_trail();
    if (!tr || !ledger_trail_ok()) return AE_EPERM;
    
    if (p->principal == PRINCIPAL_NONE) return AE_EPERM;
    
    switch ((int)op) {
    case TR_OP_HEAD: {  /* -> x0 = head_low, x1 = head_high */
        uint64_t head = trail_head(tr);
        f->x[0] = (uint64_t)(uint32_t)head;       /* low 32 bits */
        f->x[1] = (uint64_t)(head >> 32);         /* high 32 bits */
        return AE_OK;
    }
    case TR_OP_READ: {  /* x1 = seq, x2 = user buffer -> x0 = 0/AE_ENOENT */
        uint32_t seq = (uint32_t)a1;
        struct trail_rec *buf = (struct trail_rec *)a2;
        if (!buf) return AE_EINVAL;
        if (!user_writable(a2, sizeof(struct trail_rec))) return AE_EFAULT;
        
        const struct trail_rec *rec = trail_at(tr, seq);
        if (!rec) return AE_ENOENT;
        if (rec->principal != p->principal) return AE_EPERM;  /* Principal isolation */
        return copy_to_user(a2, rec, sizeof(*rec)) == AE_OK ? AE_OK : AE_EFAULT;
    }
    case TR_OP_SCAN: {  /* x1 = start_seq (0 = newest), x2 = max_records, x3 = user buffer -> x0 = count */
        uint32_t start_seq = (uint32_t)a1;
        uint32_t max_records = (uint32_t)a2;
        struct trail_rec *buf = (struct trail_rec *)a3;
        if (!buf || max_records == 0) return AE_EINVAL;
        if (!user_writable(a3, max_records * sizeof(struct trail_rec))) return AE_EFAULT;
        
        struct trail_rec tmp[TR_MAX];
        uint32_t count = 0;
        
        if (start_seq == 0) {
            /* Start from newest */
            for (uint32_t i = 0; i < tr->n && count < max_records; i++) {
                const struct trail_rec *rec = trail_nth_newest(tr, i);
                if (rec && rec->principal == p->principal) {
                    tmp[count++] = *rec;
                }
            }
        } else {
            /* Start from specific sequence */
            const struct trail_rec *rec = trail_at(tr, start_seq);
            if (rec && rec->principal == p->principal) {
                tmp[count++] = *rec;
                for (uint32_t i = 1; i < tr->n && count < max_records; i++) {
                    const struct trail_rec *next = trail_nth_newest(tr, i);
                    if (next && next->principal == p->principal) {
                        tmp[count++] = *next;
                    }
                }
            }
        }
        return copy_to_user(a3, tmp, count * sizeof(struct trail_rec)) == AE_OK ? (int64_t)count : AE_EFAULT;
    }
    case TR_OP_COUNT: {  /* -> x0 = total records in chain */
        return (int64_t)trail_count(tr);
    }
    case TR_OP_VERIFY: {  /* x0 = expected_head_low, x1 = expected_head_high -> x0 = TR_OK/TR_E_CHAIN/TR_E_CORRUPT */
        uint64_t expected_head = a1 | (a2 << 32);
        return (int64_t)trail_verify_head(tr, expected_head);
    }
    default:
        return AE_ENOSYS;
    }
}

/* ---- SYS_ACTION syscall handler ---- */
static int64_t action_syscall(struct exc_frame *f, struct proc *p, uint64_t op, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
    (void)f;
    if (p->principal == PRINCIPAL_NONE) return AE_EPERM;
    
    /* Get the broker environment and check if review path is available */
    const struct bp_env *env = broker_env();
    if (!env || !env->review_path) return ACTION_E_REVIEW_UNAVAILABLE;
    
    switch ((int)op) {
    case ACTION_OP_SET_SETTING: {  /* S1: x1 = setting_id, x2 = value, x3 = arg_kind */
        /* Parse and validate the intent */
        struct aura_intent intent = {0};
        intent.version = AURA_INTENT_VERSION;
        intent.kind = AK_SET_SETTING;
        intent.target = (uint8_t)a1;  /* setting_id */
        intent.arg = (int32_t)a2;     /* value */
        intent.arg_kind = (uint8_t)a3; /* AA_ABS or AA_REL */
        
        /* Validate the intent */
        struct aura_caps caps = { .pages = 1, .timer = 1, .notes = 1, .messaging = 1, .hardware = 1 };
        if (!aura_validate(&intent, &caps)) return ACTION_E_ARG;
        
        /* Check passport capability */
        if (!env->passport_check || !env->passport_check(p->principal, p->gen, PP_SETTINGS_PROPOSE)) {
            return ACTION_E_PERM;
        }
        
        /* Check ledger health */
        if (!ledger_ok()) return ACTION_E_REVIEW_UNAVAILABLE;
        
        /* Create broker request */
        struct bp_request req = {0};
        req.pid = p->pid;
        req.gen = p->gen;
        req.nonce = a4;  /* Use a4 as nonce */
        req.intent = intent;
        
        struct bp_result result = {0};
        struct bp_replay *replay = broker_replay();
        bp_decide(env, replay, &req, &result);
        
        if (result.decision == BP_ALLOW_NOW) {
            /* S0 action - execute immediately (should not happen for SET_SETTING) */
            return ACTION_OK;
        } else if (result.decision == BP_REVIEW) {
            /* S1 action - open review */
            /* The actual review opening happens in the shell loop via aura_s1_begin */
            return ACTION_PENDING;
        } else {
            return ACTION_E_PERM;
        }
    }
    case ACTION_OP_FILE_EDIT: {  /* S2: x1 = file_handle, x2 = offset, x3 = length, x4 = flags, x5 = user buffer */
        /* Check passport capability */
        if (!env->passport_check || !env->passport_check(p->principal, p->gen, PP_FILES_WRITE)) {
            return ACTION_E_PERM;
        }
        return ACTION_E_REVIEW_UNAVAILABLE;  /* Not yet implemented */
    }
    case ACTION_OP_FILE_DELETE: {  /* S3: x1 = file_handle */
        /* Check passport capability */
        if (!env->passport_check || !env->passport_check(p->principal, p->gen, PP_FILES_DELETE)) {
            return ACTION_E_PERM;
        }
        return ACTION_E_REVIEW_UNAVAILABLE;  /* Not yet implemented */
    }
    default:
        return AE_ENOSYS;
    }
}

static void do_syscall(struct exc_frame *f, struct proc *p)
{
    uint64_t nr = f->x[8];
    p->syscalls++;
    switch (nr) {
    case SYS_WRITE: {
        uint64_t ptr = f->x[0], len = f->x[1];
        if (len == 0) { f->x[0] = 0; return; }
        if (len > SYS_WRITE_MAX) { f->x[0] = (uint64_t)(int64_t)AE_E2BIG; return; }
        char tmp[SYS_WRITE_MAX];
        int rc = copy_from_user(tmp, ptr, (size_t)len);
        if (rc != AE_OK) { f->x[0] = (uint64_t)(int64_t)rc; return; }
        if (trace_n < SCHED_TRACE_MAX) trace[trace_n++] = tmp[0];
        user_print(p->pid, tmp, (size_t)len);
        f->x[0] = len;
        return;
    }
    case SYS_YIELD:
        f->x[0] = 0; p->yields++;
        p->ctx = *f; set_state(p, PROC_READY);
        reschedule(f);
        return;
    case SYS_SEND: {
        uint64_t dest = f->x[0];
        int r = ipc_send(p, dest, f->x[1], f->x[2]);
        f->x[0] = (uint64_t)(int64_t)r;
        if (r == AE_OK) {                                    /* wake a receiver that was parked waiting for a message */
            struct proc *to = proc_get((int)dest);
            if (to && to->state == PROC_BLOCKED && to->rx_pending) set_state(to, PROC_READY);
        }
        return;
    }
    case SYS_RECV: {
        uint64_t buf = f->x[0], max = f->x[1], flags = f->x[2], from = 0;
        if (max == 0 || max > PAGE_SIZE || (flags & ~(uint64_t)IPC_NONBLOCK)) { f->x[0] = (uint64_t)(int64_t)AE_EINVAL; return; }   /* unknown flag bits are refused, not ignored */
        int r = ipc_dequeue(p, buf, max, &from);
        if (r != AE_EAGAIN) { f->x[0] = (uint64_t)(int64_t)r; f->x[1] = from; return; }
        if (flags & IPC_NONBLOCK) { f->x[0] = (uint64_t)(int64_t)AE_EAGAIN; return; }
        if (!user_writable(buf, (size_t)max)) { f->x[0] = (uint64_t)(int64_t)AE_EFAULT; return; }   /* refuse before blocking */
        p->rx_pending = 1; p->rx_buf = buf; p->rx_max = max;
        p->ctx = *f; set_state(p, PROC_BLOCKED);             /* parked as a saved frame: no busy waiting, no kernel stack held */
        reschedule(f);
        return;
    }
    case SYS_MEM:
        f->x[0] = (uint64_t)mem_syscall(p, f->x[0], f->x[1]);
        return;
    case SYS_FS:
        f->x[0] = (uint64_t)fs_syscall(p, f->x[0], f->x[1], f->x[2], f->x[3], f->x[4]);
        return;
    case SYS_SETTINGS:
        f->x[0] = (uint64_t)settings_syscall(p, f->x[0], f->x[1], f->x[2]);
        return;
    case SYS_MODEL:
        f->x[0] = (uint64_t)model_syscall(p, f->x[0], f->x[1], f->x[2], f->x[3], f->x[4]);
        return;
    case SYS_TIMER:
        f->x[0] = (uint64_t)timersys_syscall(p, f->x[0], f->x[1], f->x[2]);
        return;
    case SYS_UI:
        f->x[0] = (uint64_t)ui_syscall(p, f->x[0], f->x[1], f->x[2], f->x[3], f->x[4]);
        return;
    case SYS_PASSPORT:
        f->x[0] = (uint64_t)passport_syscall(f, p, f->x[0], f->x[1], f->x[2], f->x[3], f->x[4]);
        return;
    case SYS_TRAIL:
        f->x[0] = (uint64_t)trail_syscall(f, p, f->x[0], f->x[1], f->x[2], f->x[3], f->x[4]);
        return;
    case SYS_ACTION:
        f->x[0] = (uint64_t)action_syscall(f, p, f->x[0], f->x[1], f->x[2], f->x[3], f->x[4]);
        return;
    case SYS_GETPID: f->x[0] = (uint64_t)p->pid; return;
    case SYS_TICKS:  f->x[0] = ticks; return;
    case SYS_EXIT:
        p->exit_code = (int)(int64_t)f->x[0]; ipc_cleanup(p);
        set_state(p, PROC_TERMINATED);
        reschedule(f);
        return;
    default:
        f->x[0] = (uint64_t)(int64_t)AE_ENOSYS;              /* unknown syscall: fail safely, the process continues */
        return;
    }
}

static const char *fsc_class(uint64_t fsc)
{
    switch (fsc & 0x3C) { case 0x04: return "translation fault"; case 0x08: return "access flag fault"; case 0x0C: return "permission fault"; default: return "other"; }
}

static void do_fault(struct exc_frame *f, struct proc *p, uint64_t ec)
{
    p->fault_esr = f->esr; p->fault_far = f->far; p->fault_elr = f->elr; p->fault_spsr = f->spsr;
    console_puts("[proc "); console_dec((uint64_t)p->pid); console_puts("] FAULT at EL0: EC="); console_hex(ec);
    if (ec == 0x24 || ec == 0x20) {
        console_puts(" FSC="); console_hex(f->esr & 0x3F); console_puts(" ("); console_puts(fsc_class(f->esr & 0x3F)); console_puts(")");
        if (ec == 0x24) console_puts((f->esr & (1u << 6)) ? " write" : " read");
        console_puts(" FAR="); console_hex(f->far);
    }
    console_puts(" ELR="); console_hex(f->elr); console_puts("; process stopped, kernel continues\n");
    p->ctx = *f;                                             /* kept for post-mortem; a FAULTED process is never scheduled */
    ipc_cleanup(p);
    set_state(p, PROC_FAULTED);
    reschedule(f);
}

void sched_after_irq(struct exc_frame *f)
{
    if (!active) return;
    struct proc *p = current();
    if (timed_out) {                                         /* watchdog: give control back to the kernel */
        stats.timeouts++;
        if (p && p->state == PROC_RUNNING) { p->ctx = *f; set_state(p, PROC_READY); }
        return_to_kernel(f, SCHED_TIMEOUT);
        return;
    }
    if (need_resched && p && p->state == PROC_RUNNING) {     /* timer preemption: applied through the exception return path */
        p->preemptions++; stats.preemptions++;
        if (f->elr >= USER_CODE_VA && f->elr < USER_CODE_VA + PAGE_SIZE) stats.preempt_elr_in_user++;
        p->ctx = *f; set_state(p, PROC_READY);
        reschedule(f);
    }
    need_resched = 0;
}

int sched_handle_el0(struct exc_frame *f)
{
    struct proc *p = current();
    if (!active || !p || p->state != PROC_RUNNING) return -1;        /* an EL0 exception with no running process: a kernel bug */
    if ((f->spsr & 0xF) != 0) return -2;                              /* must really have come from EL0t */
    p->el0_exceptions++; p->el0_confirmed++;
    { uint64_t t; __asm__ volatile ("mrs %0, ttbr0_el1" : "=r"(t)); stats.ttbr_checks++; if (t != aspace_ttbr(&p->as)) stats.ttbr_mismatch++; }
    if (f->kind == 9) { irq_dispatch(); sched_after_irq(f); return 0; }
    if (f->kind != 8) return -3;                                      /* FIQ/SError/AArch32 from EL0: not expected */
    uint64_t ec = (f->esr >> 26) & 0x3F;
    if (ec == 0x15 && (f->esr & 0xFFFF) == 0) do_syscall(f, p);       /* only "svc #0" is the system-call instruction */
    else do_fault(f, p, ec);                                          /* data/instruction aborts, illegal instructions, FP/SIMD traps, other SVC numbers */
    return 0;
}
