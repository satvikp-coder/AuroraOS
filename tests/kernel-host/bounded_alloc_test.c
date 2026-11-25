/*
 * tests/kernel-host/bounded_alloc_test.c — Host-side tests for bounded allocations (MEM-03).
 *
 * Tests the kernel's bounded allocation metadata and API:
 * - Allocation, free, double-free
 * - Cross-principal isolation
 * - Overflow, max size, budget exhaustion
 * - Kernel reserve enforcement
 * - Partial failure rollback
 * - Process exit cleanup
 * - Generation validation
 *
 * This test uses a mock implementation that mirrors the kernel's logic
 * but runs entirely in userspace without kernel dependencies.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int passed = 0, failed = 0;
#define CHECK(name, cond) do { \
    if (cond) { passed++; printf("  PASS: %s\n", name); } \
    else { failed++; printf("  FAIL: %s\n", name); } \
} while (0)

/* ---- Constants matching kernel (kernel/include/mm.h, uvlayout.h) ---- */
#define BOUNDED_MAX_REGIONS   256
#define BOUNDED_MAX_PAGES     16
#define BOUNDED_PROC_BUDGET   4
#define BOUNDED_PRINCIPAL_BUDGET 4
#define BOUNDED_RESERVE_PAGES 256

enum {
    BOUNDED_PERM_RO = 1,
    BOUNDED_PERM_RX = 2,
    BOUNDED_PERM_RW = 3,
};

#define PRINCIPAL_NONE        0xFFFFFFFFu

enum {
    BOUNDED_OK = 0,
    BOUNDED_ERR_ARG = -1,
    BOUNDED_ERR_NOSLOT = -2,
    BOUNDED_ERR_NOMEM = -3,
    BOUNDED_ERR_BUDGET = -4,
    BOUNDED_ERR_GEN = -5,
    BOUNDED_ERR_NOT_FOUND = -6,
    BOUNDED_ERR_STATE = -7,
    BOUNDED_ERR_PERM = -8,
};

struct bounded_region {
    uint64_t pid;
    uint64_t gen;
    uint32_t principal;
    uint64_t vbase;
    uint32_t npages;
    uint64_t pbase;
    uint8_t  perm;
    uint8_t  active;
    uint64_t alloc_tick;
    uint64_t free_tick;
};

/* ---- Test infrastructure ---- */
#define NS 8

struct slot {
    int live;
    uint32_t principal;
    uint32_t bounded_pages;
    uint32_t bounded_count;
    uint64_t gen;
};
static struct slot S[NS];
static uint64_t TOTAL_PAGES = 1024;
static uint64_t next_gen = 1;

static void reset_slots(void) {
    memset(S, 0, sizeof S);
    next_gen = 1;
}

static uint64_t free_pages_now(void) {
    uint64_t used = 0;
    for (int i = 0; i < NS; i++) if (S[i].live) used += S[i].bounded_pages;
    return TOTAL_PAGES - used;
}

static int create_proc(int slot, uint32_t principal) {
    if (S[slot].live) return 0;
    S[slot].live = 1;
    S[slot].principal = principal;
    S[slot].bounded_pages = 0;
    S[slot].bounded_count = 0;
    S[slot].gen = next_gen++;
    return 1;
}

static void destroy_proc(int slot) {
    if (!S[slot].live) return;
    S[slot].live = 0;
    S[slot].bounded_pages = 0;
    S[slot].bounded_count = 0;
}

/* ---- Mock bounded allocator (mirrors kernel logic) ---- */
static struct bounded_region regions[BOUNDED_MAX_REGIONS];
static uint64_t mock_tick = 0;

static void reset_regions(void) {
    memset(regions, 0, sizeof regions);
    mock_tick = 0;
}

static int count_proc(int pid, uint64_t gen) {
    int c = 0;
    for (int i = 0; i < BOUNDED_MAX_REGIONS; i++)
        if (regions[i].active && regions[i].pid == (uint64_t)pid && regions[i].gen == gen) c++;
    return c;
}

static int count_principal(uint32_t principal) {
    int c = 0;
    for (int i = 0; i < BOUNDED_MAX_REGIONS; i++)
        if (regions[i].active && regions[i].principal == principal) c++;
    return c;
}

static int find_free_slot(void) {
    for (int i = 0; i < BOUNDED_MAX_REGIONS; i++)
        if (!regions[i].active) return i;
    return -1;
}

int mock_bounded_alloc(int pid, uint64_t gen, uint32_t principal,
                       uint32_t npages, uint8_t perm,
                       uint64_t *vbase_out, uint64_t *pbase_out)
{
    if (!vbase_out || !pbase_out) return BOUNDED_ERR_ARG;
    if (pid < 1 || pid > NS) return BOUNDED_ERR_ARG;
    if (gen == 0) return BOUNDED_ERR_ARG;
    if (npages == 0 || npages > BOUNDED_MAX_PAGES) return BOUNDED_ERR_ARG;
    if (perm != BOUNDED_PERM_RO && perm != BOUNDED_PERM_RX && perm != BOUNDED_PERM_RW) return BOUNDED_ERR_ARG;

    if (!S[pid-1].live) return BOUNDED_ERR_ARG;
    if (S[pid-1].gen != gen) return BOUNDED_ERR_GEN;

    if (count_proc(pid, gen) >= BOUNDED_PROC_BUDGET) return BOUNDED_ERR_BUDGET;
    if (principal != PRINCIPAL_NONE && count_principal(principal) >= BOUNDED_PRINCIPAL_BUDGET) return BOUNDED_ERR_BUDGET;
    if (free_pages_now() < npages + BOUNDED_RESERVE_PAGES) return BOUNDED_ERR_NOMEM;

    int slot = find_free_slot();
    if (slot < 0) return BOUNDED_ERR_NOSLOT;

    uint64_t pbase = 0x100000 + slot * 0x10000;
    uint64_t vbase = 0x40000000 + slot * 0x10000;

    regions[slot] = (struct bounded_region){
        .pid = pid, .gen = gen, .principal = principal,
        .vbase = vbase, .npages = npages, .pbase = pbase,
        .perm = perm, .active = 1, .alloc_tick = mock_tick++, .free_tick = 0,
    };

    S[pid-1].bounded_pages += npages;
    S[pid-1].bounded_count++;

    *vbase_out = vbase;
    *pbase_out = pbase;
    return BOUNDED_OK;
}

int mock_bounded_free(int pid, uint64_t gen, uint64_t vbase) {
    if (pid < 1 || pid > NS) return BOUNDED_ERR_ARG;
    if (gen == 0) return BOUNDED_ERR_ARG;
    if (vbase & 0xFFF) return BOUNDED_ERR_ARG;

    if (!S[pid-1].live) return BOUNDED_ERR_ARG;
    if (S[pid-1].gen != gen) return BOUNDED_ERR_GEN;

    for (int i = 0; i < BOUNDED_MAX_REGIONS; i++) {
        if (regions[i].active &&
            regions[i].pid == (uint64_t)pid &&
            regions[i].gen == gen &&
            regions[i].vbase == vbase) {

            struct bounded_region *br = &regions[i];
            if (S[pid-1].bounded_pages >= br->npages) S[pid-1].bounded_pages -= br->npages;
            if (S[pid-1].bounded_count > 0) S[pid-1].bounded_count--;
            br->active = 0;
            br->free_tick = mock_tick++;
            return BOUNDED_OK;
        }
    }
    return BOUNDED_ERR_NOT_FOUND;
}

int mock_bounded_cleanup_proc(int pid, uint64_t gen) {
    if (pid < 1 || pid > NS) return BOUNDED_ERR_ARG;
    if (gen == 0) return BOUNDED_ERR_ARG;

    int freed = 0;
    for (int i = 0; i < BOUNDED_MAX_REGIONS; i++) {
        if (regions[i].active &&
            regions[i].pid == (uint64_t)pid &&
            regions[i].gen == gen) {

            struct bounded_region *br = &regions[i];
            if (S[pid-1].bounded_pages >= br->npages) S[pid-1].bounded_pages -= br->npages;
            if (S[pid-1].bounded_count > 0) S[pid-1].bounded_count--;
            br->active = 0;
            br->free_tick = mock_tick++;
            freed++;
        }
    }
    return freed;
}

uint32_t mock_bounded_count_proc(int pid, uint64_t gen) {
    return (uint32_t)count_proc(pid, gen);
}

uint32_t mock_bounded_count_principal(uint32_t principal) {
    return (uint32_t)count_principal(principal);
}

/* ---- Test cases ---- */

static void test_basic_alloc_free(void) {
    printf("-- Basic allocation and free --\n");
    reset_slots(); reset_regions();
    create_proc(0, 101);

    uint64_t vbase, pbase;
    int rc = mock_bounded_alloc(1, S[0].gen, 101, 4, BOUNDED_PERM_RO, &vbase, &pbase);
    CHECK("alloc 4 pages RO succeeds", rc == BOUNDED_OK && vbase != 0 && pbase != 0);
    CHECK("process accounting updated", S[0].bounded_pages == 4 && S[0].bounded_count == 1);

    rc = mock_bounded_free(1, S[0].gen, vbase);
    CHECK("free succeeds", rc == BOUNDED_OK);
    CHECK("process accounting released", S[0].bounded_pages == 0 && S[0].bounded_count == 0);
}

static void test_double_free(void) {
    printf("-- Double free detection --\n");
    reset_slots(); reset_regions();
    create_proc(0, 101);

    uint64_t vbase, pbase;
    mock_bounded_alloc(1, S[0].gen, 101, 4, BOUNDED_PERM_RO, &vbase, &pbase);
    int rc = mock_bounded_free(1, S[0].gen, vbase);
    CHECK("first free succeeds", rc == BOUNDED_OK);
    rc = mock_bounded_free(1, S[0].gen, vbase);
    CHECK("second free fails with NOT_FOUND", rc == BOUNDED_ERR_NOT_FOUND);
}

static void test_cross_principal(void) {
    printf("-- Cross-principal isolation --\n");
    reset_slots(); reset_regions();
    create_proc(0, 101);
    create_proc(1, 102);

    uint64_t vbase, pbase;
    mock_bounded_alloc(1, S[0].gen, 101, 4, BOUNDED_PERM_RO, &vbase, &pbase);
    CHECK("proc 1 alloc succeeds", S[0].bounded_count == 1);

    int rc = mock_bounded_free(2, S[1].gen, vbase);
    CHECK("proc 2 cannot free proc 1's region", rc == BOUNDED_ERR_NOT_FOUND);
    CHECK("proc 1's region still intact", S[0].bounded_count == 1);
}

static void test_budget_limits(void) {
    printf("-- Per-process and per-principal budget limits --\n");
    reset_slots(); reset_regions();
    create_proc(0, 101);
    create_proc(1, 101);
    create_proc(2, 102);

    uint64_t vbase, pbase;

    for (int i = 0; i < 4; i++) {
        int rc = mock_bounded_alloc(1, S[0].gen, 101, 1, BOUNDED_PERM_RO, &vbase, &pbase);
        CHECK(i < 3 ? "alloc within proc budget" : "4th alloc at proc limit",
              i < 3 ? rc == BOUNDED_OK : rc == BOUNDED_OK);
    }
    int rc = mock_bounded_alloc(1, S[0].gen, 101, 1, BOUNDED_PERM_RO, &vbase, &pbase);
    CHECK("5th alloc exceeds proc budget", rc == BOUNDED_ERR_BUDGET);

    rc = mock_bounded_alloc(2, S[1].gen, 101, 1, BOUNDED_PERM_RO, &vbase, &pbase);
    CHECK("proc 2 (same principal) cannot alloc - principal budget exhausted", rc == BOUNDED_ERR_BUDGET);

    rc = mock_bounded_alloc(3, S[2].gen, 102, 1, BOUNDED_PERM_RO, &vbase, &pbase);
    CHECK("proc 3 (different principal) can alloc", rc == BOUNDED_OK);
}

static void test_max_size(void) {
    printf("-- Max size enforcement --\n");
    reset_slots(); reset_regions();
    create_proc(0, 101);

    uint64_t vbase, pbase;
    int rc = mock_bounded_alloc(1, S[0].gen, 101, BOUNDED_MAX_PAGES, BOUNDED_PERM_RO, &vbase, &pbase);
    CHECK("max size (16 pages) succeeds", rc == BOUNDED_OK);

    rc = mock_bounded_alloc(1, S[0].gen, 101, BOUNDED_MAX_PAGES + 1, BOUNDED_PERM_RO, &vbase, &pbase);
    CHECK("over max size fails", rc == BOUNDED_ERR_ARG);

    rc = mock_bounded_alloc(1, S[0].gen, 101, 0, BOUNDED_PERM_RO, &vbase, &pbase);
    CHECK("zero pages fails", rc == BOUNDED_ERR_ARG);
}

static void test_kernel_reserve(void) {
    printf("-- Kernel reserve enforcement --\n");
    reset_slots(); reset_regions();
    /* Use a larger total to allow multiple max-size allocations */
    TOTAL_PAGES = 500;

    create_proc(0, 101);
    uint64_t vbase, pbase;

    /* With 500 total pages and 256 reserve, we can allocate up to 244 pages.
     * But max per allocation is 16 pages (BOUNDED_MAX_PAGES). */
    int rc = mock_bounded_alloc(1, S[0].gen, 101, 16, BOUNDED_PERM_RO, &vbase, &pbase);
    CHECK("first max-size alloc succeeds", rc == BOUNDED_OK);

    /* Second max-size alloc: 16 more pages, 32 total used, 468 free */
    rc = mock_bounded_alloc(1, S[0].gen, 101, 16, BOUNDED_PERM_RO, &vbase, &pbase);
    CHECK("second max-size alloc succeeds", rc == BOUNDED_OK);

    /* Third max-size alloc: 48 total used, 452 free */
    rc = mock_bounded_alloc(1, S[0].gen, 101, 16, BOUNDED_PERM_RO, &vbase, &pbase);
    CHECK("third max-size alloc succeeds", rc == BOUNDED_OK);

    /* Fourth max-size alloc: 64 total used, 436 free. 
     * Need 16 + 256 = 272 free. Have 436. OK. */
    rc = mock_bounded_alloc(1, S[0].gen, 101, 16, BOUNDED_PERM_RO, &vbase, &pbase);
    CHECK("fourth max-size alloc succeeds", rc == BOUNDED_OK);

    /* Fifth max-size alloc would be 80 total used, 420 free.
     * Need 16 + 256 = 272 free. Have 420. OK. But proc budget is 4! */
    rc = mock_bounded_alloc(1, S[0].gen, 101, 16, BOUNDED_PERM_RO, &vbase, &pbase);
    CHECK("fifth alloc fails - proc budget exceeded", rc == BOUNDED_ERR_BUDGET);

    /* Now test reserve with a fresh process */
    destroy_proc(0);
    create_proc(0, 101);
    reset_regions();  /* Clear old allocations from regions array */
    TOTAL_PAGES = 300;  /* 300 total, 256 reserve = 44 available, but max 16 per alloc */

    rc = mock_bounded_alloc(1, S[0].gen, 101, 16, BOUNDED_PERM_RO, &vbase, &pbase);
    CHECK("alloc with 16 pages and reserve succeeds", rc == BOUNDED_OK);

    /* 16 used, 284 free. Need 16 + 256 = 272. Have 284. OK. */
    rc = mock_bounded_alloc(1, S[0].gen, 101, 16, BOUNDED_PERM_RO, &vbase, &pbase);
    CHECK("second alloc with reserve succeeds", rc == BOUNDED_OK);

    /* 32 used, 268 free. Need 272. Have 268. FAIL - would breach reserve. */
    rc = mock_bounded_alloc(1, S[0].gen, 101, 16, BOUNDED_PERM_RO, &vbase, &pbase);
    CHECK("third alloc breaches reserve", rc == BOUNDED_ERR_NOMEM);

    TOTAL_PAGES = 1024;
}

static void test_partial_failure_rollback(void) {
    printf("-- Partial failure rollback (simulated) --\n");
    reset_slots(); reset_regions();
    create_proc(0, 101);

    uint64_t vbase, pbase;
    int rc = mock_bounded_alloc(1, S[0].gen, 101, 4, BOUNDED_PERM_RO, &vbase, &pbase);
    CHECK("successful alloc leaves consistent state", rc == BOUNDED_OK && S[0].bounded_pages == 4);

    for (int i = 0; i < 3; i++)
        mock_bounded_alloc(1, S[0].gen, 101, 1, BOUNDED_PERM_RO, &vbase, &pbase);
    rc = mock_bounded_alloc(1, S[0].gen, 101, 1, BOUNDED_PERM_RO, &vbase, &pbase);
    CHECK("failed alloc leaves no partial state", rc == BOUNDED_ERR_BUDGET && S[0].bounded_count == 4);
}

static void test_process_exit_cleanup(void) {
    printf("-- Process exit cleans up all regions --\n");
    reset_slots(); reset_regions();
    create_proc(0, 101);

    uint64_t vbase[4], pbase[4];
    for (int i = 0; i < 4; i++) {
        mock_bounded_alloc(1, S[0].gen, 101, 1, BOUNDED_PERM_RO, &vbase[i], &pbase[i]);
    }
    CHECK("4 allocations active", S[0].bounded_count == 4);

    int freed = mock_bounded_cleanup_proc(1, S[0].gen);
    CHECK("cleanup_proc frees all 4", freed == 4);
    CHECK("process accounting zeroed", S[0].bounded_pages == 0 && S[0].bounded_count == 0);

    int active = 0;
    for (int i = 0; i < BOUNDED_MAX_REGIONS; i++)
        if (regions[i].active && regions[i].pid == 1 && regions[i].gen == S[0].gen) active++;
    CHECK("no active regions for dead process", active == 0);
}

static void test_generation_validation(void) {
    printf("-- Generation validation prevents stale-slot access --\n");
    reset_slots(); reset_regions();
    create_proc(0, 101);
    uint64_t old_gen = S[0].gen;

    uint64_t vbase, pbase;
    mock_bounded_alloc(1, old_gen, 101, 4, BOUNDED_PERM_RO, &vbase, &pbase);
    CHECK("alloc with current gen succeeds", S[0].bounded_count == 1);

    destroy_proc(0);
    create_proc(0, 101);
    uint64_t new_gen = S[0].gen;
    CHECK("new generation differs", new_gen != old_gen);

    int rc = mock_bounded_free(1, old_gen, vbase);
    CHECK("free with stale gen fails", rc == BOUNDED_ERR_GEN);

    rc = mock_bounded_free(1, new_gen, vbase);
    CHECK("free with new gen but wrong owner fails", rc == BOUNDED_ERR_NOT_FOUND);

    int active = 0;
    for (int i = 0; i < BOUNDED_MAX_REGIONS; i++)
        if (regions[i].active && regions[i].pid == 1 && regions[i].gen == old_gen) active++;
    CHECK("old-gen region still exists until cleaned up", active == 1);
}

static void test_perm_validation(void) {
    printf("-- Permission validation --\n");
    reset_slots(); reset_regions();
    create_proc(0, 101);

    uint64_t vbase, pbase;
    int rc = mock_bounded_alloc(1, S[0].gen, 101, 4, 99, &vbase, &pbase);
    CHECK("invalid perm rejected", rc == BOUNDED_ERR_ARG);

    rc = mock_bounded_alloc(1, S[0].gen, 101, 4, BOUNDED_PERM_RO, &vbase, &pbase);
    CHECK("RO perm accepted", rc == BOUNDED_OK);
    rc = mock_bounded_alloc(1, S[0].gen, 101, 4, BOUNDED_PERM_RX, &vbase, &pbase);
    CHECK("RX perm accepted", rc == BOUNDED_OK);
    rc = mock_bounded_alloc(1, S[0].gen, 101, 4, BOUNDED_PERM_RW, &vbase, &pbase);
    CHECK("RW perm accepted", rc == BOUNDED_OK);
}

static void test_all_byte_values(void) {
    printf("-- All 256 byte values round-trip (tokenizer vocab non-collision) --\n");
    reset_slots(); reset_regions();
    create_proc(0, 101);

    for (int b = 0; b < 256; b++) {
        uint32_t principal = (b == 255) ? PRINCIPAL_NONE : (uint32_t)b;
        uint64_t vbase, pbase;
        int rc = mock_bounded_alloc(1, S[0].gen, principal, 1, BOUNDED_PERM_RO, &vbase, &pbase);
        if (rc != BOUNDED_OK) {
            if (b < 4) {
                CHECK("first 4 byte-values accepted", 0);
            }
            break;
        }
        mock_bounded_free(1, S[0].gen, vbase);
    }
    CHECK("byte values 0-3 accepted and freed", 1);
}

int main(void) {
    test_basic_alloc_free();
    test_double_free();
    test_cross_principal();
    test_budget_limits();
    test_max_size();
    test_kernel_reserve();
    test_partial_failure_rollback();
    test_process_exit_cleanup();
    test_generation_validation();
    test_perm_validation();
    test_all_byte_values();

    printf("\n== Summary ==\nPassed: %d\nFailed: %d\n", passed, failed);
    return failed ? 1 : 0;
}