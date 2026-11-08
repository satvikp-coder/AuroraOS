/*
 * kernel/arch/aarch64/exceptions.c — exception diagnostics.
 *
 * Decodes ESR_EL1 (ARM DDI 0487 D17.2) and prints a serial report. Any
 * unexpected synchronous exception is fatal: the kernel prints
 * "KERNEL PANIC" and halts. Interrupts are not handled yet (issue #5).
 */
#include <arch.h>
#include <console.h>

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

void exception_handler(struct exc_frame *f)
{
    uint64_t ec = (f->esr >> 26) & 0x3F, iss = f->esr & 0x1FFFFFF;
    console_puts("\n[FAULT] exception ");
    console_puts(kind_name(f->kind));
    console_puts("\n[FAULT] EC="); console_hex(ec); console_puts(" ("); console_puts(ec_name(ec)); console_puts(")");
    if (ec == 0x24 || ec == 0x25 || ec == 0x20 || ec == 0x21) {
        console_puts("\n[FAULT] FSC="); console_hex(iss & 0x3F); console_puts(" ("); console_puts(fsc_name(iss & 0x3F)); console_puts(")");
        if (ec == 0x24 || ec == 0x25) { console_puts(" access="); console_puts((iss & (1u << 6)) ? "write" : "read"); }
        console_puts("\n[FAULT] FAR="); console_hex(f->far);
    }
    console_puts("\n[FAULT] ELR="); console_hex(f->elr);
    console_puts(" ESR="); console_hex(f->esr);
    console_puts(" SPSR="); console_hex(f->spsr);
    console_puts("\nKERNEL PANIC: unhandled exception, halting.\n");
    for (;;) __asm__ volatile ("msr daifset, #0xf\n wfi");
}
