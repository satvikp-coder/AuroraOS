/* kernel/ui/ledger_kernel.c - binds the pure ledger (ledger_store.c) to AuroraFS and the console. The ledger files belong to PRINCIPAL_PASSPORT with
 * owner-only permissions: no system call can name that principal, so no process can read, write, list or even see them. */
#include <blk.h>
#include <console.h>
#include <fs.h>
#include <ledger.h>
#include <proc.h>
#include <timersys.h>
#include <trail.h>

static int k_mounted(void) { return fs_mounted(); }
static int k_readonly(void) { return blk_readonly(); }
static int k_lookup(uint32_t owner, const char *name, uint64_t *id) { fs_id_t i = 0; int rc = fs_lookup_owner(owner, name, &i); *id = i; return rc; }
static int k_create(uint32_t owner, const char *name, uint64_t *id) { fs_id_t i = 0; int rc = fs_create(owner, name, FS_PERM_OWNER_R | FS_PERM_OWNER_W, &i); *id = i; return rc; }
static int k_size(uint64_t id, uint32_t *size) { struct fs_stat st; int rc = fs_stat(0, id, &st); if (rc == FS_OK) *size = st.size; return rc; }
static int k_read(uint64_t id, void *buf, uint32_t len) { return fs_read(0, id, 0, buf, len); }
static int k_write(uint32_t owner, uint64_t id, const void *buf, uint32_t len) { return fs_write(owner, id, 0, buf, len); }
static const struct lg_io KERNEL_IO = { k_mounted, k_readonly, k_lookup, k_create, k_size, k_read, k_write };

static void dump(void)
{
    const struct passport *p = ledger_passport();
    uint32_t n = 0;
    for (int i = 0; i < PP_MAX_GRANTS; i++) if (p->g[i].used) {
        n++;
        console_puts("ledger: grant principal="); console_dec(p->g[i].principal); console_puts(" perm="); console_dec(p->g[i].perm);
        console_puts(p->g[i].gen ? " instance\n" : " durable\n");
    }
    console_puts("ledger: grants="); console_dec(n); console_puts("\n");
}

static void tdump(void)
{
    const struct trail *t = ledger_trail();
    console_puts("ledger: trail state="); console_puts(ledger_state_name(ledger_trail_state())); console_puts(" records="); console_dec(t->n);
    console_puts(" commit="); console_dec(ledger_trail_commit()); console_puts(" ok="); console_dec((uint64_t)ledger_trail_ok()); console_puts("\n");
    for (uint32_t i = 0; i < t->n; i++) { console_puts("ledger: trail rec seq="); console_dec(t->r[i].seq); console_puts(" kind="); console_dec(t->r[i].kind); console_puts(" principal="); console_dec(t->r[i].principal); console_puts("\n"); }
}

static int trail_note(uint8_t kind, uint32_t principal)
{
    struct trail_rec r; uint8_t *z = (uint8_t *)&r; for (unsigned i = 0; i < sizeof r; i++) z[i] = 0;
    r.kind = kind; r.principal = principal; r.uptime_ms = (uint32_t)timersys_now_ms();
    return ledger_trail_append(&r);
}

void ledger_boot(void)
{
    int s = ledger_attach(&KERNEL_IO);
    console_puts("ledger: state="); console_puts(ledger_state_name(s)); console_puts(" commit="); console_dec(ledger_commit());
    console_puts(" ok="); console_dec((uint64_t)ledger_passport_ok()); console_puts(" fs_err="); console_dec((uint64_t)(uint32_t)(-ledger_last_fs_error())); console_puts("\n");
    dump();
    tdump();
    { int rc = trail_note(14, 0);                                                /* the boot marker: kind 14, requester = the kernel */
      console_puts("ledger: boot marker rc="); console_dec((uint64_t)(uint32_t)(-rc)); console_puts(" trail="); console_puts(ledger_state_name(ledger_trail_state())); console_puts("\n"); }
#ifdef AURORA_LEDGER_TEST
    /* TEST BUILDS ONLY: a fixed script, chosen at build time, that exercises the durable API so tests/qemu/ledger-tests.sh can restart the guest and look
     * at the result. 1: three durable grants and a RAM-only one; 2: revoke one; 3: fill the table (33 principals); 4: revoke what was never granted. */
    #define OP(label, call) do { int rc_ = (call); console_puts("ledger: op " label " rc="); console_dec((uint64_t)(uint32_t)(-rc_)); console_puts(" state="); console_puts(ledger_state_name(ledger_state())); console_puts("\n"); } while (0)
    #if AURORA_LEDGER_TEST == 1
    OP("grant 1001 files-write", ledger_grant(1001, PP_FILES_WRITE));
    OP("grant 1001 timer", ledger_grant(1001, PP_TIMER));
    OP("grant 1002 navigate", ledger_grant(1002, PP_NAVIGATE));
    OP("instance 1003 timer gen 5", ledger_grant_instance(1003, 5, PP_TIMER));
    OP("reserved 0x7F000004 navigate", ledger_grant(PRINCIPAL_PASSPORT, PP_NAVIGATE));
    OP("principal 0 navigate", ledger_grant(0, PP_NAVIGATE));
    #elif AURORA_LEDGER_TEST == 2
    OP("revoke 1001 files-write", ledger_revoke(1001, PP_FILES_WRITE));
    OP("revoke 4242 timer (never granted)", ledger_revoke(4242, PP_TIMER));
    #elif AURORA_LEDGER_TEST == 3
    for (uint32_t k = 0; k < 40; k++) { int rc_ = ledger_grant(2000 + k, PP_NAVIGATE); if (rc_ != LG_E_OK) { console_puts("ledger: op fill stopped at "); console_dec(k); console_puts(" rc="); console_dec((uint64_t)(uint32_t)(-rc_)); console_puts("\n"); break; } }
    OP("grant after full", ledger_grant(3000, PP_NAVIGATE));
    #elif AURORA_LEDGER_TEST == 4
    OP("trail append grant record", trail_note(12, 1001));
    OP("trail append revoke record", trail_note(13, 1001));
    { struct trail_rec bad; uint8_t *z = (uint8_t *)&bad; for (unsigned i = 0; i < sizeof bad; i++) z[i] = 0; bad.kind = 99; OP("trail append invalid kind 99", ledger_trail_append(&bad)); }
    #endif
    dump();
    tdump();
    console_puts("ledger: after state="); console_puts(ledger_state_name(ledger_state())); console_puts(" commit="); console_dec(ledger_commit()); console_puts("\n");
#endif
}
