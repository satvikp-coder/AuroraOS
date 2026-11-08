/*
 * tests/kernel-host/policy_test.c — host tests for kernel/mm/mmpolicy.c.
 *
 * Besides hand-written cases, a brute-force check compares mm_policy_check() with an independent
 * per-page oracle for every attribute and every 1..4 page range starting around each region boundary.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <mmpolicy.h>

static int passed, failed;
#define CHECK(name, cond) do { if (cond) { passed++; printf("  PASS: %s\n", name); } else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)

#define PG 4096ULL
static struct mm_policy P;

static void layout(void)
{
    memset(&P, 0, sizeof P);
    P.ram    = (struct mm_range){ 0x40000000ULL, 0x40100000ULL };
    P.text   = (struct mm_range){ 0x40010000ULL, 0x40014000ULL };
    P.rodata = (struct mm_range){ 0x40014000ULL, 0x40018000ULL };
    mm_policy_add_nomap(&P, 0x40030000ULL, 0x2000);
    mm_policy_add_mmio(&P, 0x08000000ULL, 0x200000);
    mm_policy_add_mmio(&P, 0x09000000ULL, 0x1000);
}

enum { C_OUT, C_RAM, C_TEXT, C_RODATA, C_NOMAP, C_MMIO0, C_MMIO1 };
static int classify(uint64_t a)
{
    if (a >= P.nomap[0].start && a < P.nomap[0].end) return C_NOMAP;
    if (a >= P.text.start && a < P.text.end) return C_TEXT;
    if (a >= P.rodata.start && a < P.rodata.end) return C_RODATA;
    if (a >= P.ram.start && a < P.ram.end) return C_RAM;
    if (a >= P.mmio[0].start && a < P.mmio[0].end) return C_MMIO0;
    if (a >= P.mmio[1].start && a < P.mmio[1].end) return C_MMIO1;
    return C_OUT;
}
/* Independent oracle: decided page by page. */
static int oracle(uint64_t pa, int pages, int attr)
{
    int all_ram = 1, all_text = 1, any_text = 0, any_ro = 0, same_mmio = 1, any_ram = 0, any_nomap = 0;
    int first = classify(pa);
    for (int i = 0; i < pages; i++) {
        int c = classify(pa + (uint64_t)i * PG);
        if (c == C_NOMAP) any_nomap = 1;
        if (c == C_TEXT) any_text = 1;
        if (c == C_RODATA) any_ro = 1;
        if (c != C_RAM && c != C_TEXT && c != C_RODATA) all_ram = 0;
        if (c != C_TEXT) all_text = 0;
        if (c == C_RAM || c == C_TEXT || c == C_RODATA) any_ram = 1;
        if (c != first || (c != C_MMIO0 && c != C_MMIO1)) same_mmio = 0;
    }
    if (any_nomap) return 0;
    if (attr == MMP_ATTR_DEVICE) return !any_ram && same_mmio;
    if (!all_ram) return 0;
    if (attr == MMP_ATTR_NORMAL_RW) return !any_text && !any_ro;
    if (attr == MMP_ATTR_NORMAL_RX) return all_text;
    return 1;
}

int main(void)
{
    layout();
    printf("-- hand-written cases --\n");
    CHECK("RW text refused", mm_policy_check(&P, 0x40010000ULL, PG, MMP_ATTR_NORMAL_RW) == MMP_DENY_TEXT_WRITE);
    CHECK("RW last text page refused", mm_policy_check(&P, 0x40013000ULL, PG, MMP_ATTR_NORMAL_RW) == MMP_DENY_TEXT_WRITE);
    CHECK("RW rodata refused", mm_policy_check(&P, 0x40014000ULL, PG, MMP_ATTR_NORMAL_RW) == MMP_DENY_RODATA_WRITE);
    CHECK("RW page just before text allowed", mm_policy_check(&P, 0x4000F000ULL, PG, MMP_ATTR_NORMAL_RW) == MMP_OK);
    CHECK("RW page just after rodata allowed", mm_policy_check(&P, 0x40018000ULL, PG, MMP_ATTR_NORMAL_RW) == MMP_OK);
    CHECK("RW crossing into text refused", mm_policy_check(&P, 0x4000F000ULL, 2 * PG, MMP_ATTR_NORMAL_RW) == MMP_DENY_TEXT_WRITE);
    CHECK("RW crossing out of rodata refused", mm_policy_check(&P, 0x40017000ULL, 2 * PG, MMP_ATTR_NORMAL_RW) == MMP_DENY_RODATA_WRITE);
    CHECK("RO and RX of text allowed, RO of rodata allowed", mm_policy_check(&P, 0x40010000ULL, 4 * PG, MMP_ATTR_NORMAL_RX) == MMP_OK && mm_policy_check(&P, 0x40010000ULL, PG, MMP_ATTR_NORMAL_RO) == MMP_OK && mm_policy_check(&P, 0x40014000ULL, PG, MMP_ATTR_NORMAL_RO) == MMP_OK);
    CHECK("RX outside text refused (data, rodata, crossing)", mm_policy_check(&P, 0x40018000ULL, PG, MMP_ATTR_NORMAL_RX) == MMP_DENY_EXEC_OUTSIDE_TEXT && mm_policy_check(&P, 0x40014000ULL, PG, MMP_ATTR_NORMAL_RX) == MMP_DENY_EXEC_OUTSIDE_TEXT && mm_policy_check(&P, 0x40013000ULL, 2 * PG, MMP_ATTR_NORMAL_RX) == MMP_DENY_EXEC_OUTSIDE_TEXT);
    CHECK("no-map refused for all four attributes", mm_policy_check(&P, 0x40030000ULL, PG, MMP_ATTR_NORMAL_RW) == MMP_DENY_NOMAP && mm_policy_check(&P, 0x40030000ULL, PG, MMP_ATTR_NORMAL_RO) == MMP_DENY_NOMAP && mm_policy_check(&P, 0x40030000ULL, PG, MMP_ATTR_NORMAL_RX) == MMP_DENY_NOMAP && mm_policy_check(&P, 0x40031000ULL, PG, MMP_ATTR_DEVICE) == MMP_DENY_NOMAP);
    CHECK("range touching no-map from either side refused", mm_policy_check(&P, 0x4002F000ULL, 2 * PG, MMP_ATTR_NORMAL_RO) == MMP_DENY_NOMAP && mm_policy_check(&P, 0x40031000ULL, 2 * PG, MMP_ATTR_NORMAL_RO) == MMP_DENY_NOMAP);
    CHECK("pages adjacent to no-map are fine", mm_policy_check(&P, 0x4002F000ULL, PG, MMP_ATTR_NORMAL_RW) == MMP_OK && mm_policy_check(&P, 0x40032000ULL, PG, MMP_ATTR_NORMAL_RW) == MMP_OK);
    CHECK("normal on MMIO refused", mm_policy_check(&P, 0x09000000ULL, PG, MMP_ATTR_NORMAL_RW) == MMP_DENY_NORMAL_ON_MMIO && mm_policy_check(&P, 0x08000000ULL, PG, MMP_ATTR_NORMAL_RO) == MMP_DENY_NORMAL_ON_MMIO);
    CHECK("normal outside RAM refused; crossing RAM end refused", mm_policy_check(&P, 0x40100000ULL, PG, MMP_ATTR_NORMAL_RW) == MMP_DENY_NORMAL_OUTSIDE_RAM && mm_policy_check(&P, 0x400FF000ULL, 2 * PG, MMP_ATTR_NORMAL_RW) == MMP_DENY_NORMAL_OUTSIDE_RAM && mm_policy_check(&P, 0x3FFFF000ULL, 2 * PG, MMP_ATTR_NORMAL_RW) == MMP_DENY_NORMAL_OUTSIDE_RAM);
    CHECK("device inside windows allowed", mm_policy_check(&P, 0x09000000ULL, PG, MMP_ATTR_DEVICE) == MMP_OK && mm_policy_check(&P, 0x08000000ULL, 0x200000, MMP_ATTR_DEVICE) == MMP_OK);
    CHECK("device outside windows / past a window end / across windows refused", mm_policy_check(&P, 0x0A000000ULL, PG, MMP_ATTR_DEVICE) == MMP_DENY_DEVICE_OUTSIDE_WINDOW && mm_policy_check(&P, 0x09000000ULL, 2 * PG, MMP_ATTR_DEVICE) == MMP_DENY_DEVICE_OUTSIDE_WINDOW && mm_policy_check(&P, 0x081FF000ULL, 0x900000, MMP_ATTR_DEVICE) == MMP_DENY_DEVICE_OUTSIDE_WINDOW);
    CHECK("device on RAM refused", mm_policy_check(&P, 0x40000000ULL, PG, MMP_ATTR_DEVICE) == MMP_DENY_DEVICE_ON_RAM);
    CHECK("zero size and overflow refused", mm_policy_check(&P, 0x40000000ULL, 0, MMP_ATTR_NORMAL_RW) == MMP_DENY_OVERFLOW && mm_policy_check(&P, ~0ULL - 100, 4096, MMP_ATTR_NORMAL_RW) == MMP_DENY_OVERFLOW);
    CHECK("unknown attributes refused", mm_policy_check(&P, 0x40000000ULL, PG, 0) == MMP_DENY_ATTR && mm_policy_check(&P, 0x40000000ULL, PG, 5) == MMP_DENY_ATTR && mm_policy_check(&P, 0x40000000ULL, PG, -1) == MMP_DENY_ATTR);
    CHECK("an empty policy denies everything", ({ struct mm_policy E; memset(&E, 0, sizeof E); mm_policy_check(&E, 0x40000000ULL, PG, MMP_ATTR_NORMAL_RO) != MMP_OK && mm_policy_check(&E, 0x09000000ULL, PG, MMP_ATTR_DEVICE) != MMP_OK; }));

    printf("-- registration --\n");
    CHECK("MMIO over RAM or no-map refused", mm_policy_add_mmio(&P, 0x40000000ULL, PG) == MMP_DENY_BAD_REGISTRATION && mm_policy_add_mmio(&P, 0x40030000ULL, PG) == MMP_DENY_BAD_REGISTRATION);
    CHECK("empty or overflowing registrations refused", mm_policy_add_mmio(&P, 0x0B000000ULL, 0) == MMP_DENY_BAD_REGISTRATION && mm_policy_add_mmio(&P, ~0ULL - 5, 100) == MMP_DENY_BAD_REGISTRATION && mm_policy_add_nomap(&P, 0x40040000ULL, 0) == MMP_DENY_BAD_REGISTRATION && mm_policy_add_nomap(&P, ~0ULL - 5, 100) == MMP_DENY_BAD_REGISTRATION);
    CHECK("registering a window makes exactly that window mappable", mm_policy_check(&P, 0x0B000000ULL, PG, MMP_ATTR_DEVICE) != MMP_OK && mm_policy_add_mmio(&P, 0x0B000000ULL, PG) == MMP_OK && mm_policy_check(&P, 0x0B000000ULL, PG, MMP_ATTR_DEVICE) == MMP_OK && mm_policy_check(&P, 0x0B001000ULL, PG, MMP_ATTR_DEVICE) != MMP_OK);
    { struct mm_policy F; memset(&F, 0, sizeof F); int ok = 1; for (int i = 0; i < MMP_MAX_MMIO; i++) ok &= mm_policy_add_mmio(&F, 0x0C000000ULL + (uint64_t)i * 0x10000, PG) == MMP_OK; CHECK("MMIO table capacity is enforced", ok && mm_policy_add_mmio(&F, 0x0D000000ULL, PG) == MMP_DENY_BAD_REGISTRATION); }
    { struct mm_policy F; memset(&F, 0, sizeof F); int ok = 1; for (int i = 0; i < MMP_MAX_NOMAP; i++) ok &= mm_policy_add_nomap(&F, 0x40000000ULL + (uint64_t)i * 0x10000, PG) == MMP_OK; CHECK("no-map table capacity is enforced", ok && mm_policy_add_nomap(&F, 0x40900000ULL, PG) == MMP_DENY_BAD_REGISTRATION); }

    printf("-- brute force against the per-page oracle --\n");
    layout();
    static const uint64_t anchors[] = { 0x08000000ULL, 0x081FF000ULL, 0x08200000ULL, 0x09000000ULL, 0x09001000ULL, 0x0A000000ULL,
        0x3FFFF000ULL, 0x40000000ULL, 0x4000F000ULL, 0x40010000ULL, 0x40012000ULL, 0x40013000ULL, 0x40014000ULL, 0x40017000ULL,
        0x40018000ULL, 0x4002F000ULL, 0x40030000ULL, 0x40031000ULL, 0x40032000ULL, 0x400FE000ULL, 0x400FF000ULL, 0x40100000ULL };
    long cases = 0, mismatches = 0;
    for (size_t a = 0; a < sizeof anchors / sizeof *anchors; a++)
        for (int pages = 1; pages <= 4; pages++)
            for (int attr = MMP_ATTR_NORMAL_RW; attr <= MMP_ATTR_DEVICE; attr++) {
                int got = mm_policy_check(&P, anchors[a], (uint64_t)pages * PG, attr) == MMP_OK;
                int want = oracle(anchors[a], pages, attr);
                cases++;
                if (got != want) { mismatches++; printf("    mismatch pa=%#llx pages=%d attr=%d got=%d want=%d\n", (unsigned long long)anchors[a], pages, attr, got, want); }
            }
    printf("    %ld cases compared\n", cases);
    CHECK("policy agrees with the independent oracle on every case", mismatches == 0 && cases == (long)(sizeof anchors / sizeof *anchors) * 16);
    CHECK("reasons have text", mm_policy_reason(MMP_DENY_NOMAP)[0] != 0 && mm_policy_reason(12345)[0] != 0);

    printf("\n== Summary ==\nPassed: %d\nFailed: %d\n", passed, failed);
    return failed ? 1 : 0;
}
