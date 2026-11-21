/*
 * tests/kernel-host/heapacct_test.c - SYS_MEM heap accounting (kernel/include/heapacct.h): the REAL decision code against an independent reference model.
 *
 * The kernel derives a principal's usage as the sum over live process slots; the REFERENCE MODEL below is written differently on purpose: it keeps STORED
 * per-principal counters that it charges and releases on each event. If the two ever disagree, or the stored counters stop equalling the live pages, a test fails.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "heapacct.h"

#define NS 8                                   /* PROC_MAX */
#define NONE 0xFFFFFFFFu
static int fails = 0, total = 0;
static void check(const char *what, int ok) { total++; if (!ok) { fails++; printf("  FAIL: %s\n", what); } }

/* ---- simulated machine ---- */
struct slot { int live; uint32_t principal; uint32_t pages; int faulted; uint32_t base; };
static struct slot S[NS];
static uint64_t TOTAL = 400;                    /* physical pages in the simulation: 400 - 256 reserve leaves room for 144 heap pages */

static uint64_t free_now(void) { uint64_t used = 0; for (int i = 0; i < NS; i++) if (S[i].live) used += S[i].base + S[i].pages; return TOTAL - used; }
static void view(struct hp_view *v) { for (int i = 0; i < NS; i++) { v[i].live = (uint8_t)S[i].live; v[i].principal = S[i].principal; v[i].pages = S[i].pages; } }

/* ---- the reference model: stored counters ---- */
static const uint32_t PR[5] = { NONE, 101, 102, 103, 104 };
static uint64_t charged[5];
static int pidx(uint32_t p) { for (int i = 0; i < 5; i++) if (PR[i] == p) return i; return -1; }

static uint64_t rng = 88172645463325252ULL;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

/* the model's own decision, from its counters (not from the policy code) */
static int model_decide(int self, uint64_t n)
{
    if (n == 0 || n > 32) return HG_EINVAL;
    if (self < 0 || self >= NS || !S[self].live) return HG_EINVAL;
    if (S[self].pages + n > 32) return HG_ENOMEM;
    if (charged[pidx(S[self].principal)] + n > 64) return HG_ENOMEM;
    if (free_now() < n + 256) return HG_ENOMEM;
    return HG_OK;
}

static int verify_invariants(void)
{
    uint64_t live_sum = 0, ch_sum = 0; struct hp_view v[NS]; view(v);
    for (int i = 0; i < NS; i++) if (S[i].live) live_sum += S[i].pages;
    for (int i = 0; i < 5; i++) { ch_sum += charged[i]; if (hp_principal_total(v, NS, PR[i]) != charged[i]) return 0; if (charged[i] > 64) return 0; }
    return live_sum == ch_sum;
}

static void reset(void) { memset(S, 0, sizeof S); memset(charged, 0, sizeof charged); }
static int create(int slot, uint32_t pr) { if (S[slot].live) return 0; S[slot].live = 1; S[slot].principal = pr; S[slot].pages = 0; S[slot].faulted = 0; S[slot].base = 6; return 1; }
static void destroy(int slot) { if (!S[slot].live) return; charged[pidx(S[slot].principal)] -= S[slot].pages; memset(&S[slot], 0, sizeof S[slot]); }
static int grow(int slot, uint64_t n, int *decision)
{
    struct hp_view v[NS]; view(v);
    int real = hp_grow_check(v, NS, slot, n, free_now());
    *decision = real;
    if (real != model_decide(slot, n)) return -1;                      /* disagreement */
    if (real != HG_OK) return 0;
    S[slot].pages += (uint32_t)n; charged[pidx(S[slot].principal)] += n;
    return 1;
}
static void shrink(int slot, uint32_t n) { if (!S[slot].live || n == 0 || n > S[slot].pages) return; S[slot].pages -= n; charged[pidx(S[slot].principal)] -= n; }

int main(void)
{
    struct hp_view v[NS]; int d;

    /* ---- deterministic scenarios ---- */
    reset(); create(0, 101); create(1, 101); create(2, 101); create(3, 102);
    check("one process can take its full 32 pages", grow(0, 32, &d) == 1);
    check("a SECOND process of the same principal can take 32 more (the principal's 64)", grow(1, 32, &d) == 1);
    check("a THIRD process of that principal cannot take even one page: the principal budget is 64 in total, not 64 per process", grow(2, 1, &d) == 0 && d == HG_ENOMEM);
    check("a process of ANOTHER principal is unaffected by the first one's usage", grow(3, 32, &d) == 1);
    shrink(1, 1);
    check("after one process gives a page back, exactly that page is available to a sibling (charge released on shrink)", grow(2, 1, &d) == 1 && grow(2, 1, &d) == 0);
    destroy(0);
    check("killing a process releases ONLY its charge: the sibling can now take 32 pages but not 33 (per-process limit holds)", verify_invariants() && grow(2, 31, &d) == 1 && grow(2, 1, &d) == 0 && d == HG_ENOMEM);
    destroy(1); create(1, 101);
    check("a new process reusing the slot starts with an empty heap and a clean charge (no stale charge from the previous occupant)", S[1].pages == 0 && verify_invariants() && grow(1, 1, &d) == 1);
    S[3].faulted = 1; create(4, 102);
    check("a FAULTED process keeps its pages and its charge until it is destroyed: principal 102 holds 32 pages in the faulted slot, so a new sibling can take 32 more but not 33",
          verify_invariants() && charged[pidx(102)] == 32 && grow(4, 32, &d) == 1 && grow(4, 1, &d) == 0);
    destroy(3);
    check("destroying the faulted process releases its 32 pages: the sibling's principal total drops to the sibling's own 32", verify_invariants() && charged[pidx(102)] == 32);
    reset(); create(0, NONE); create(1, NONE);
    check("processes with NO principal share ONE budget: two of them cannot take 32 + 33", grow(0, 32, &d) == 1 && grow(1, 32, &d) == 1 && grow(1, 1, &d) == 0);
    reset(); create(0, 101); create(1, 102); S[0].pages = 0;
    check("a principal over the old budget by set_principal is not revoked, but cannot grow further",
          grow(0, 32, &d) == 1 && grow(1, 32, &d) == 1 && (S[1].principal = 101, charged[pidx(102)] -= 32, charged[pidx(101)] += 32, 1) && verify_invariants() && grow(1, 1, &d) == 0);

    /* hostile arguments */
    reset(); create(0, 101); view(v);
    check("0 pages is EINVAL", hp_grow_check(v, NS, 0, 0, 1000) == HG_EINVAL);
    check("33 pages is EINVAL", hp_grow_check(v, NS, 0, 33, 1000) == HG_EINVAL);
    check("2^63 and UINT64_MAX pages are EINVAL (no wrap into a small number)", hp_grow_check(v, NS, 0, 1ULL << 63, 1000) == HG_EINVAL && hp_grow_check(v, NS, 0, ~0ULL, 1000) == HG_EINVAL && hp_grow_check(v, NS, 0, (1ULL << 32) + 1, 1000) == HG_EINVAL);
    check("a bad or dead slot, a NULL view are EINVAL", hp_grow_check(v, NS, -1, 1, 1000) == HG_EINVAL && hp_grow_check(v, NS, NS, 1, 1000) == HG_EINVAL && hp_grow_check(v, NS, 1, 1, 1000) == HG_EINVAL && hp_grow_check(0, NS, 0, 1, 1000) == HG_EINVAL);
    check("exactly the reserve free (256) refuses 1 page; 257 free grants it; free below the reserve refuses; 0 free refuses",
          hp_grow_check(v, NS, 0, 1, 256) == HG_ENOMEM && hp_grow_check(v, NS, 0, 1, 257) == HG_OK && hp_grow_check(v, NS, 0, 1, 10) == HG_ENOMEM && hp_grow_check(v, NS, 0, 1, 0) == HG_ENOMEM);
    check("a request of N pages needs N + 256 free (boundary exact for 32 pages)", hp_grow_check(v, NS, 0, 32, 287) == HG_ENOMEM && hp_grow_check(v, NS, 0, 32, 288) == HG_OK);
    struct hp_view big[NS]; for (int i = 0; i < NS; i++) { big[i].live = 1; big[i].principal = 7; big[i].pages = 0xFFFFFFFFu; }
    check("eight slots each claiming UINT32_MAX pages sum without wrapping (64-bit total) and refuse growth", hp_principal_total(big, NS, 7) == 8ULL * 0xFFFFFFFFULL && hp_grow_check(big, NS, 0, 1, 1000000) == HG_ENOMEM);
    reset(); create(0, 101); create(1, 101); view(v); v[0].pages = 40; v[0].live = 0; v[1].pages = 5;   /* a dead slot with STALE pages and principal left in the table */
    check("a slot that is not live holds no charge, even if stale pages and a principal are left in it (total 5, and a 27-page grow fits)", hp_principal_total(v, NS, 101) == 5 && hp_grow_check(v, NS, 1, 27, 100000) == HG_OK);

    /* ---- randomized model check ---- */
    reset();
    long ops = 0, grows_ok = 0, grows_refused = 0, disagreements = 0, invariant_breaks = 0, reserve_breaks = 0;
    const uint64_t sizes[] = { 0, 1, 1, 2, 3, 5, 8, 16, 31, 32, 33, 64, 1ULL << 63, ~0ULL };
    for (long i = 0; i < 200000; i++) {
        int slot = (int)(rnd() % NS); uint64_t r = rnd() % 100; ops++;
        if (r < 12) { create(slot, PR[rnd() % 5]); }
        else if (r < 20) { destroy(slot); }
        else if (r < 70) {
            uint64_t n = sizes[rnd() % (sizeof sizes / sizeof *sizes)];
            int rc = grow(slot, n, &d);
            if (rc < 0) disagreements++;
            else if (rc == 1) { grows_ok++; if (free_now() < 256) reserve_breaks++; }
            else grows_refused++;
        }
        else if (r < 88) { if (S[slot].live) shrink(slot, (uint32_t)(1 + rnd() % 32)); }
        else if (r < 94) { if (S[slot].live) S[slot].faulted = 1; }                                         /* a fault changes state, not ownership */
        else { if (S[slot].live && S[slot].pages == 0) { S[slot].principal = PR[rnd() % 5]; } }               /* the launcher sets a principal on a fresh process */
        if (!verify_invariants()) invariant_breaks++;
    }
    printf("    model run: %ld operations, %ld grows granted, %ld refused\n", ops, grows_ok, grows_refused);
    check("200,000 random operations: the real policy agreed with the independent model on EVERY grow (0 disagreements)", disagreements == 0);
    check("charged pages equalled live owned pages for every principal and in total after EVERY operation (0 breaks)", invariant_breaks == 0);
    check("a granted grow never left fewer than 256 pages free (0 breaks)", reserve_breaks == 0);
    check("the run exercised both grants and refusals many times (not a vacuous pass)", grows_ok > 1000 && grows_refused > 1000);

    printf("heapacct: %d passed, %d failed\n", total - fails, fails);
    return fails != 0;
}
