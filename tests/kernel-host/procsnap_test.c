#define _POSIX_C_SOURCE 200809L
/* tests/kernel-host/procsnap_test.c - the read-only process snapshot (kernel/proc/procsnap.c) against a fake process table. */
#include <procsnap.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int failed, total;
#define CHECK(name, cond) do { total++; if (!(cond)) { failed++; printf("FAIL: %s\n", name); } } while (0)

static struct proc tab[PROC_MAX];
static const struct proc *get(int pid) { return (pid >= 1 && pid <= PROC_MAX && tab[pid - 1].state != PROC_UNUSED) ? &tab[pid - 1] : 0; }
static void clear(void) { memset(tab, 0, sizeof tab); }
static void mk(int pid, enum proc_state st, const char *name, uint64_t gen)
{
    struct proc *p = &tab[pid - 1];
    memset(p, 0, sizeof *p);
    p->pid = pid; p->state = st; p->gen = gen; p->as.npages = 7;
    strncpy(p->name, name, sizeof p->name - 1);
}
static uint64_t xs = 88172645463325252ULL;
static uint64_t rnd(void) { xs ^= xs << 13; xs ^= xs >> 7; xs ^= xs << 17; return xs; }

int main(void)
{
    struct procsnap s;
    clear();
    CHECK("empty table: no entries", procsnap_take(&s, get) == 0 && s.n == 0);
    CHECK("NULL out / NULL getter return 0 and do not crash", procsnap_take(0, get) == 0 && procsnap_take(&s, 0) == 0);

    mk(3, PROC_READY, "alpha", 11);
    s.n = 99;
    CHECK("one process", procsnap_take(&s, get) == 1 && s.n == 1 && s.e[0].pid == 3 && s.e[0].state == PSS_READY && s.e[0].gen == 11 && !strcmp(s.e[0].name, "alpha") && s.e[0].pages == 7);

    clear();
    for (int i = 1; i <= PROC_MAX; i++) { char n[8] = "p0"; n[1] = (char)('0' + i); mk(i, (enum proc_state)(PROC_NEW + (i - 1) % 6), n, (uint64_t)i * 3); }
    CHECK("all eight slots are listed", procsnap_take(&s, get) == PROC_MAX);
    int asc = 1; for (uint32_t i = 0; i < s.n; i++) if (s.e[i].pid != i + 1) asc = 0;
    CHECK("ascending pids", asc);
    CHECK("every state maps to its own view state", s.e[0].state == PSS_NEW && s.e[1].state == PSS_READY && s.e[2].state == PSS_RUNNING && s.e[3].state == PSS_BLOCKED && s.e[4].state == PSS_EXITED && s.e[5].state == PSS_FAULTED);

    clear(); mk(2, PROC_TERMINATED, "done", 5); tab[1].exit_code = 42; mk(4, PROC_FAULTED, "bad", 6); tab[3].exit_code = 9;
    procsnap_take(&s, get);
    CHECK("exit code is shown for exited processes only", s.e[0].exit_code == 42 && s.e[1].exit_code == 0);

    clear(); mk(1, PROC_READY, "old", 1); procsnap_take(&s, get); uint64_t g1 = s.e[0].gen;
    mk(1, PROC_READY, "new", 2); procsnap_take(&s, get);
    CHECK("pid reuse: same pid, different generation, new name", s.e[0].pid == 1 && s.e[0].gen != g1 && !strcmp(s.e[0].name, "new"));

    clear(); mk(1, PROC_UNUSED, "x", 1); tab[0].state = PROC_UNUSED;
    CHECK("unused slots are not listed", procsnap_take(&s, get) == 0);
    mk(1, PROC_READY, "x", 1); tab[0].pid = 5;
    CHECK("a slot whose pid field disagrees with its index is not listed", procsnap_take(&s, get) == 0);
    mk(1, (enum proc_state)77, "x", 1);
    CHECK("a corrupt state value is not listed", procsnap_take(&s, get) == 0);

    clear(); mk(1, PROC_READY, "", 1);
    procsnap_take(&s, get);
    CHECK("an empty name becomes a question mark", !strcmp(s.e[0].name, "?"));
    mk(1, PROC_READY, "", 1); memset(tab[0].name, 'A', sizeof tab[0].name);   /* no terminator at all */
    procsnap_take(&s, get);
    CHECK("a name without a terminator is truncated and terminated", strlen(s.e[0].name) == PROCSNAP_NAME - 1);
    mk(1, PROC_READY, "", 1); tab[0].name[0] = 'a'; tab[0].name[1] = '\n'; tab[0].name[2] = (char)0xC3; tab[0].name[3] = 'b'; tab[0].name[4] = 0;
    procsnap_take(&s, get);
    CHECK("control and non-ASCII bytes in a name are replaced", !strcmp(s.e[0].name, "a??b"));

    clear(); mk(1, PROC_RUNNING, "c", 1);
    tab[0].ticks_run = 0xFFFFFFFFFFFFFFFFULL; tab[0].switches_in = 0xFFFFFFFFULL; tab[0].yields = 1ULL << 40; tab[0].preemptions = 0; tab[0].as.npages = -3;
    procsnap_take(&s, get);
    CHECK("64-bit counters are carried unchanged; a negative page count is shown as 0", s.e[0].ticks == 0xFFFFFFFFFFFFFFFFULL && s.e[0].switches == 0xFFFFFFFFULL && s.e[0].yields == (1ULL << 40) && s.e[0].pages == 0);

    clear(); for (int i = 1; i <= PROC_MAX; i++) mk(i, PROC_READY, "z", (uint64_t)i);
    static struct proc before[PROC_MAX]; memcpy(before, tab, sizeof tab);
    procsnap_take(&s, get);
    CHECK("taking a snapshot leaves the process table byte-identical", !memcmp(before, tab, sizeof tab));

    /* Nothing private is in the snapshot: fill every private field with a marker and look for it in the output bytes. */
    clear(); mk(1, PROC_READY, "m", 1);
    tab[0].code_pa = 0xAAAAAAAAAAAAAAAAULL; tab[0].data_pa = 0xBBBBBBBBBBBBBBBBULL; tab[0].stack_pa = 0xCCCCCCCCCCCCCCCCULL; tab[0].entry = 0xDDDDDDDDDDDDDDDDULL;
    tab[0].principal = 0xEEEEEEEEu; tab[0].fault_far = 0x1111111111111111ULL; tab[0].fault_elr = 0x2222222222222222ULL; tab[0].as.l1_pa = 0x3333333333333333ULL; uint64_t pages[] = { 0x4444444444444444ULL };
    tab[0].as.pages = pages;
    tab[0].ipc_grant_gen[2] = 0x5555555555555555ULL; tab[0].ctx.x[0] = 0x6666666666666666ULL;
    memset(&s, 0, sizeof s); procsnap_take(&s, get);
    const unsigned char *b = (const unsigned char *)&s; int leak = 0; static const unsigned char marks[] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };
    for (size_t i = 0; i + 4 <= sizeof s; i++) for (size_t m = 0; m < sizeof marks; m++) if (b[i] == marks[m] && b[i + 1] == marks[m] && b[i + 2] == marks[m] && b[i + 3] == marks[m]) leak++;
    CHECK("no kernel pointer, physical address, principal, fault state, grant or register value appears in the snapshot", leak == 0);

    clear(); mk(1, PROC_READY, "a", 1); mk(2, PROC_BLOCKED, "b", 2);
    procsnap_take(&s, get); uint64_t d0 = procsnap_digest(&s); procsnap_take(&s, get);
    CHECK("digest is stable for an unchanged table", procsnap_digest(&s) == d0);
    int sens = 0, tries = 0;
    for (int field = 0; field < 8; field++) {
        static struct proc save[PROC_MAX]; memcpy(save, tab, sizeof tab);
        struct proc *p = &tab[1];
        switch (field) { case 0: p->state = PROC_READY; break; case 1: p->gen++; break; case 2: p->ticks_run++; break; case 3: p->switches_in++; break;
                         case 4: p->yields++; break; case 5: p->preemptions++; break; case 6: p->as.npages++; break; default: p->name[0] = 'q'; }
        procsnap_take(&s, get); tries++; if (procsnap_digest(&s) != d0) sens++;
        memcpy(tab, save, sizeof tab);
    }
    CHECK("the digest changes when any displayed field changes", sens == tries);

    int bad = 0;
    for (int it = 0; it < 20000; it++) {
        clear();
        for (int pid = 1; pid <= PROC_MAX; pid++) {
            if (rnd() % 3 == 0) continue;
            struct proc *p = &tab[pid - 1]; memset(p, 0, sizeof *p);
            p->pid = (rnd() % 16 == 0) ? (int)(rnd() % 20) : pid; p->state = (enum proc_state)(rnd() % 9); p->gen = rnd();
            for (int k = 0; k < 16; k++) p->name[k] = (char)(rnd() % 256);
            p->ticks_run = rnd(); p->as.npages = (int)(rnd() % 40) - 5;
        }
        uint32_t n = procsnap_take(&s, get);
        if (n > PROC_MAX || n != s.n) { bad++; continue; }
        int last = 0;
        for (uint32_t i = 0; i < n; i++) {
            const struct procsnap_entry *e = &s.e[i];
            if (e->pid <= last || e->pid > PROC_MAX || e->state < PSS_NEW || e->state > PSS_FAULTED) bad++;
            last = e->pid;
            size_t L = strnlen(e->name, PROCSNAP_NAME); if (L == PROCSNAP_NAME || L == 0) bad++;
            for (size_t k = 0; k < L; k++) if ((unsigned char)e->name[k] < 0x20 || (unsigned char)e->name[k] > 0x7e) bad++;
        }
    }
    CHECK("20,000 random tables: bounded, ascending, valid states, printable terminated names", bad == 0);

    CHECK("state names", !strcmp(procsnap_state_name(PSS_RUNNING), "running") && !strcmp(procsnap_state_name(PSS_FAULTED), "faulted") && !strcmp(procsnap_state_name(0), "?") && !strcmp(procsnap_state_name(200), "?"));
    printf("procsnap_test: %d checks, %d failed\n", total, failed);
    return failed ? 1 : 0;
}
