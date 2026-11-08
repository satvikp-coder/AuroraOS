/*
 * kernel/arch/aarch64/exceptions.c — exception dispatch and diagnostics.
 *
 * Kernel code runs on SP_EL0 (kind 0-3 vectors: "current EL, SP_EL0"). Every exception taken to EL1
 * switches to SP_EL1, a dedicated exception stack with its own guard page, so entry cannot be broken by
 * an exhausted kernel stack. Supported cases:
 *   - IRQ from kernel code (kind 1)       -> irq_dispatch(), then return (ERET) to the interrupted code
 *   - synchronous exception from kernel   -> decoded diagnostics, then panic (no recovery yet)
 *   - anything taken while already on the exception stack (kinds 4-7: a fault inside a handler)
 *                                         -> reported as NESTED and panics
 *   - FIQ, SError, and exceptions from EL0 / AArch32 -> panic (none are expected yet)
 * Interrupts are masked on exception entry, so handlers never nest.
 */
#include <arch.h>
#include <console.h>
#include <irq.h>
#include <mm.h>

void exceptions_init(void)
{
    __asm__ volatile ("msr vbar_el1, %0\n isb" :: "r"((uint64_t)(uintptr_t)exception_vectors) : "memory");
}

static const char *kind_name(uint64_t k)
{
    static const char *n[16] = {
        "sync/EL1t", "irq/EL1t", "fiq/EL1t", "serror/EL1t",
        "sync/EL1h", "irq/EL1h", "fiq/EL1h", "serror/EL1h",
        "sync/EL0-a64", "irq/EL0-a64", "fiq/EL0-a64", "serror/EL0-a64",
        "sync/EL0-a32", "irq/EL0-a32", "fiq/EL0-a32", "serror/EL0-a32" };
    return k < 16 ? n[k] : "?";
}

static const char *ec_name(uint64_t ec)
{
    switch (ec) {
    case 0x00: return "unknown reason";
    case 0x15: return "SVC (AArch64)";
    case 0x20: return "instruction abort, lower EL";
    case 0x21: return "instruction abort, same EL";
    case 0x22: return "PC alignment fault";
    case 0x24: return "data abort, lower EL";
    case 0x25: return "data abort, same EL";
    case 0x26: return "SP alignment fault";
    default:   return "other";
    }
}

static const char *fsc_name(uint64_t fsc)
{
    switch (fsc) {
    case 0x04: return "translation fault, level 0";
    case 0x05: return "translation fault, level 1";
    case 0x06: return "translation fault, level 2";
    case 0x07: return "translation fault, level 3";
    case 0x09: return "access flag fault, level 1";
    case 0x0A: return "access flag fault, level 2";
    case 0x0B: return "access flag fault, level 3";
    case 0x0D: return "permission fault, level 1";
    case 0x0E: return "permission fault, level 2";
    case 0x0F: return "permission fault, level 3";
    case 0x10: return "synchronous external abort";
    case 0x21: return "alignment fault";
    default:   return "other fault";
    }
}

static int in_page(uint64_t a, const uint8_t *page) { return a >= (uint64_t)(uintptr_t)page && a < (uint64_t)(uintptr_t)page + PAGE_SIZE; }

static void panic_halt(void)
{
    console_puts("KERNEL PANIC: unhandled exception, halting.\n");
    for (;;) __asm__ volatile ("msr daifset, #0xf\n wfi");
}

void exception_handler(struct exc_frame *f)
{
    /* The frame lives on the exception stack and must be 16-byte aligned and inside it. */
    uint64_t fp = (uint64_t)(uintptr_t)f;
    int frame_ok = !(fp & 15) && fp >= (uint64_t)(uintptr_t)__exc_stack_bottom && fp + sizeof *f <= (uint64_t)(uintptr_t)__exc_stack_top;

    if (frame_ok && f->kind == 1) {                  /* IRQ taken from kernel code running on SP_EL0 */
        irq_dispatch();
        return;
    }

    uint64_t ec = (f->esr >> 26) & 0x3F, iss = f->esr & 0x1FFFFFF;

    /* Self-test hook: SVC #0x7e57 from kernel code (SP_EL0) is the register-preservation test in
     * exc_selftest.S. It rewrites two registers IN THE SAVED FRAME and returns; ELR already points past the SVC. */
    if (frame_ok && f->kind == 0 && ec == 0x15 && (iss & 0xFFFF) == 0x7E57) {
        f->x[0] = 0xAAAA5555ULL;
        f->x[27] = 0xC0DE0027ULL;
        return;
    }

    console_puts("\n[FAULT] exception "); console_puts(kind_name(f->kind));
    if (f->kind >= 4 && f->kind <= 7) console_puts("\n[FAULT] NESTED exception: taken while already on the exception stack");
    if (!frame_ok) console_puts("\n[FAULT] exception frame is misaligned or outside the exception stack");
    console_puts("\n[FAULT] EC="); console_hex(ec); console_puts(" ("); console_puts(ec_name(ec)); console_puts(")");
    if (ec == 0x24 || ec == 0x25 || ec == 0x20 || ec == 0x21) {
        console_puts("\n[FAULT] FSC="); console_hex(iss & 0x3F); console_puts(" ("); console_puts(fsc_name(iss & 0x3F)); console_puts(")");
        if (ec == 0x24 || ec == 0x25) { console_puts(" access="); console_puts((iss & (1u << 6)) ? "write" : "read"); }
        console_puts("\n[FAULT] FAR="); console_hex(f->far);
    }
    console_puts("\n[FAULT] ELR="); console_hex(f->elr);
    console_puts(" ESR="); console_hex(f->esr);
    console_puts(" SPSR="); console_hex(f->spsr);
    console_puts("\n[FAULT] interrupted SP_EL0="); console_hex(f->sp_el0);
    console_puts(" handler SP="); console_hex(fp);
    if (in_page(f->far, __stack_guard) || in_page(f->sp_el0, __stack_guard))
        console_puts("\n[FAULT] STACK OVERFLOW detected: kernel stack hit its guard page; diagnostics are running on the separate exception stack");
    if (in_page(f->far, __exc_guard))
        console_puts("\n[FAULT] EXCEPTION STACK OVERFLOW detected: exception stack hit its guard page");
    console_puts("\n");
    panic_halt();
}
