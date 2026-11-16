/* tests/kernel-host/passport_test.c - the Passport permission ledger (kernel/ui/passport.c): default deny, kernel-only mutation, identity reuse, revocation,
 * the audit ring, the versioned checksummed image (corruption, truncation, version, duplicates), storage failure, restart, and a randomised model check.
 * Host, ASan/UBSan. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <passport.h>

static int passed, failed;
#define CHECK(name, cond) do { if (cond) { passed++; } else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)

static const struct pp_actor KERNEL = { PP_ACTOR_KERNEL, 0, 0, 0 };
static const struct pp_actor PROC = { PP_ACTOR_PROCESS, 3, 30, 0x1003u };
static struct passport P;

/* a fake disk */
static uint8_t disk[PP_IMAGE_MAX + 64]; static size_t disk_len; static int fail_write, fail_read, torn;
static int st_write(const uint8_t *b, size_t n) { if (fail_write) return -1; if (n > sizeof disk) return -1; memcpy(disk, b, n); disk_len = torn ? n / 2 : n; return 0; }
static int st_read(uint8_t *b, size_t cap, size_t *len) { if (fail_read) return -1; if (disk_len > cap) return -1; memcpy(b, disk, disk_len); *len = disk_len; return 0; }
static const struct pp_store STORE = { st_write, st_read };

static uint64_t xs = 0x1234567887654321ULL;
static uint64_t rnd(void) { xs ^= xs << 13; xs ^= xs >> 7; xs ^= xs << 17; return xs; }

int main(void)
{
    printf("passport tests\n");

    /* ---- default deny ---- */
    passport_init(&P);
    CHECK("a new table denies everything", !passport_check(&P, 0x1001, 0, PP_NAVIGATE) && !passport_check(&P, 0x1001, 5, PP_FILES_DELETE));
    CHECK("invalid permission numbers and principals are denied", !passport_check(&P, 0x1001, 0, PP_NONE) && !passport_check(&P, 0x1001, 0, PP_COUNT) && !passport_check(&P, 0x1001, 0, 255) && !passport_check(&P, 0, 0, PP_NAVIGATE) && !passport_check(&P, 0xFFFFFFFFu, 0, PP_NAVIGATE));
    CHECK("a NULL table is denied without a crash", passport_check(0, 1, 0, PP_NAVIGATE) == 0);

    /* ---- only the kernel can grant ---- */
    CHECK("a PROCESS actor cannot grant (to itself)", passport_grant(&P, &PROC, 0x1003, 0, PP_NAVIGATE) == PP_E_ACTOR && !passport_check(&P, 0x1003, 30, PP_NAVIGATE));
    CHECK("a PROCESS actor cannot grant to another principal", passport_grant(&P, &PROC, 0x1004, 0, PP_NAVIGATE) == PP_E_ACTOR);
    CHECK("a NULL actor cannot grant", passport_grant(&P, 0, 0x1003, 0, PP_NAVIGATE) == PP_E_ACTOR);
    { struct pp_actor fake = { 99, 0, 0, 0 }; CHECK("an actor of an unknown kind cannot grant", passport_grant(&P, &fake, 0x1003, 0, PP_NAVIGATE) == PP_E_ACTOR); }
    CHECK("a PROCESS actor cannot revoke either", passport_revoke(&P, &PROC, 0x1003, PP_NAVIGATE) == PP_E_ACTOR);
    CHECK("the kernel can grant", passport_grant(&P, &KERNEL, 0x1003, 0, PP_NAVIGATE) == PP_OK && passport_check(&P, 0x1003, 0, PP_NAVIGATE) && passport_check(&P, 0x1003, 30, PP_NAVIGATE));
    CHECK("a grant of one permission grants nothing else", !passport_check(&P, 0x1003, 30, PP_FILES_DELETE) && !passport_check(&P, 0x1003, 30, PP_SETTINGS_PROPOSE));
    CHECK("a grant to one principal does not apply to another", !passport_check(&P, 0x1004, 0, PP_NAVIGATE));
    CHECK("granting the kernel principal 0, the none principal, or permission NONE/COUNT is refused", passport_grant(&P, &KERNEL, 0, 0, PP_NAVIGATE) == PP_E_PRINCIPAL && passport_grant(&P, &KERNEL, 0xFFFFFFFFu, 0, PP_NAVIGATE) == PP_E_PRINCIPAL && passport_grant(&P, &KERNEL, 5, 0, PP_NONE) == PP_E_PERM && passport_grant(&P, &KERNEL, 5, 0, PP_COUNT) == PP_E_PERM);
    CHECK("granting twice is idempotent (one slot)", passport_grant(&P, &KERNEL, 0x1003, 0, PP_NAVIGATE) == PP_OK && passport_encode(&P, (uint8_t[PP_IMAGE_MAX]){0}, PP_IMAGE_MAX) == 12 + 16 + 4);

    /* ---- identity reuse: a generation-bound grant dies with the instance ---- */
    passport_init(&P);
    passport_grant(&P, &KERNEL, 0x2001, 77, PP_SETTINGS_PROPOSE);
    CHECK("a generation-bound grant applies to that instance", passport_check(&P, 0x2001, 77, PP_SETTINGS_PROPOSE));
    CHECK("... not to a later instance of the same principal", !passport_check(&P, 0x2001, 78, PP_SETTINGS_PROPOSE));
    CHECK("... nor to an unknown generation (0)", !passport_check(&P, 0x2001, 0, PP_SETTINGS_PROPOSE));

    /* ---- revocation ---- */
    passport_grant(&P, &KERNEL, 0x2001, 0, PP_SETTINGS_PROPOSE);
    CHECK("revoke removes every grant of that permission for the principal", passport_revoke(&P, &KERNEL, 0x2001, PP_SETTINGS_PROPOSE) == PP_OK && !passport_check(&P, 0x2001, 77, PP_SETTINGS_PROPOSE) && !passport_check(&P, 0x2001, 0, PP_SETTINGS_PROPOSE));
    CHECK("revoking something not granted reports NOENT and changes nothing", passport_revoke(&P, &KERNEL, 0x2001, PP_SETTINGS_PROPOSE) == PP_E_NOENT);
    CHECK("revoking with a bad principal or permission is refused", passport_revoke(&P, &KERNEL, 0, PP_NAVIGATE) == PP_E_PRINCIPAL && passport_revoke(&P, &KERNEL, 9, PP_COUNT) == PP_E_PERM);

    /* ---- capacity ---- */
    passport_init(&P); int full = 0;
    for (uint32_t i = 0; i < PP_MAX_GRANTS + 5; i++) { int rc = passport_grant(&P, &KERNEL, 0x3000 + i, 0, PP_NAVIGATE); if (i < PP_MAX_GRANTS) CHECK("a grant within capacity succeeds", rc == PP_OK); else if (rc == PP_E_FULL) full++; }
    CHECK("the 33rd and later grants fail closed (FULL) and are not silently dropped into a slot", full == 5 && !passport_check(&P, 0x3000 + PP_MAX_GRANTS, 0, PP_NAVIGATE) && passport_check(&P, 0x3000, 0, PP_NAVIGATE));
    passport_revoke(&P, &KERNEL, 0x3000, PP_NAVIGATE);
    CHECK("freeing a slot lets a new grant in", passport_grant(&P, &KERNEL, 0x3999, 0, PP_NAVIGATE) == PP_OK && passport_check(&P, 0x3999, 0, PP_NAVIGATE));

    /* ---- audit ring ---- */
    passport_init(&P);
    passport_grant(&P, &PROC, 0x1003, 0, PP_NAVIGATE); passport_grant(&P, &KERNEL, 0x1003, 0, PP_NAVIGATE); passport_check(&P, 0x9999, 0, PP_NAVIGATE); passport_revoke(&P, &KERNEL, 0x1003, PP_NAVIGATE);
    { const struct pp_audit_rec *r0 = passport_audit_at(&P, 0), *r1 = passport_audit_at(&P, 1), *r2 = passport_audit_at(&P, 2), *d0 = passport_denial_at(&P, 0);
      CHECK("every grant, refused grant and revoke is in the MUTATION ring in order with sequence numbers", passport_audit_count(&P) == 3 && r0 && r1 && r2 && r0->seq == 0 && r2->seq == 2 && r0->op == PP_OP_GRANT && r0->result == PP_E_ACTOR && r0->actor_kind == PP_ACTOR_PROCESS && r0->actor_pid == 3 &&
            r1->result == PP_OK && r2->op == PP_OP_REVOKE && r2->result == PP_OK);
      CHECK("the denied check is in the separate DENIAL ring", passport_denial_count(&P) == 1 && d0 && d0->op == PP_OP_CHECK_DENY && d0->principal == 0x9999);
      CHECK("an out-of-range audit index returns NULL in both rings", passport_audit_at(&P, 3) == 0 && passport_audit_at(&P, 1000) == 0 && passport_denial_at(&P, 1) == 0); }
    for (int i = 0; i < 10000; i++) passport_check(&P, 0x5000 + (uint32_t)i, 0, PP_NAVIGATE);
    CHECK("the denial ring is bounded and keeps the newest PP_AUDIT records", passport_denial_count(&P) == PP_AUDIT && passport_denial_at(&P, PP_AUDIT - 1)->seq == P.dseq - 1 && passport_denial_at(&P, 0)->seq == P.dseq - PP_AUDIT);
    CHECK("10,000 denials cannot evict a single grant or revoke record (the mutation ring is untouched)", passport_audit_count(&P) == 3 && passport_audit_at(&P, 0)->op == PP_OP_GRANT && passport_audit_at(&P, 0)->result == PP_E_ACTOR && passport_audit_at(&P, 2)->op == PP_OP_REVOKE);
    { passport_init(&P); passport_grant(&P, &KERNEL, 0x1003, 0, PP_NAVIGATE); uint32_t s0 = P.seq, d0 = P.dseq; passport_check(&P, 0x1003, 0, PP_NAVIGATE); passport_peek(&P, 0x7777, 0, PP_NAVIGATE);
      CHECK("an ALLOWED check does not flood either ring, and passport_peek never audits (even a denial)", P.seq == s0 && P.dseq == d0); }
    for (uint32_t i = 0; i < PP_AUDIT + 5; i++) { struct passport Z; passport_init(&Z); for (uint32_t k = 0; k <= i; k++) passport_revoke(&Z, &KERNEL, 5, PP_NAVIGATE); CHECK("the mutation ring count is min(records, PP_AUDIT)", passport_audit_count(&Z) == (i + 1 < PP_AUDIT ? i + 1 : PP_AUDIT)); if (i > 3) break; }

    /* ---- quota per principal, instance cleanup, the new permission ---- */
    passport_init(&P);
    { int q = 0; for (uint8_t perm = 1; perm < PP_COUNT; perm++) { int rc = passport_grant(&P, &KERNEL, 0x6001, 0, perm); if (rc == PP_OK) q++; }
      for (uint64_t g = 1; g <= 10; g++) { int rc = passport_grant(&P, &KERNEL, 0x6001, g, PP_NAVIGATE); if (rc == PP_OK) q++; else if (rc != PP_E_QUOTA) q = -1000; }
      CHECK("one principal can hold at most PP_PER_PRINCIPAL grants (the 9th is QUOTA, not FULL)", q == PP_PER_PRINCIPAL);
      CHECK("another principal is unaffected by that quota", passport_grant(&P, &KERNEL, 0x6002, 0, PP_NAVIGATE) == PP_OK); }
    passport_init(&P); passport_grant(&P, &KERNEL, 0x6003, 91, PP_NAVIGATE); passport_grant(&P, &KERNEL, 0x6004, 91, PP_TIMER); passport_grant(&P, &KERNEL, 0x6003, 92, PP_NAVIGATE); passport_grant(&P, &KERNEL, 0x6005, 0, PP_NAVIGATE);
    CHECK("PP_TIMER is a valid permission", passport_peek(&P, 0x6004, 91, PP_TIMER));
    CHECK("passport_drop_instance drops exactly that generation's grants (2), not others, not gen-free ones", passport_drop_instance(&P, 91) == 2 && !passport_peek(&P, 0x6003, 91, PP_NAVIGATE) && !passport_peek(&P, 0x6004, 91, PP_TIMER) && passport_peek(&P, 0x6003, 92, PP_NAVIGATE) && passport_peek(&P, 0x6005, 0, PP_NAVIGATE));
    CHECK("drop_instance(0) and a NULL table do nothing", passport_drop_instance(&P, 0) == 0 && passport_drop_instance(0, 5) == 0);

    /* ---- the image: encode/decode, restart, corruption ---- */
    passport_init(&P); passport_grant(&P, &KERNEL, 0x1001, 0, PP_NAVIGATE); passport_grant(&P, &KERNEL, 0x1002, 0, PP_SETTINGS_PROPOSE); passport_grant(&P, &KERNEL, 0x1002, 0, PP_FILES_WRITE); passport_grant(&P, &KERNEL, 0x1003, 55, PP_NAVIGATE); passport_grant(&P, &KERNEL, 0x1003, 0xFFFFFFFFFFFFFFFFULL, PP_FILES_WRITE);
    uint8_t img[PP_IMAGE_MAX]; size_t n = passport_encode(&P, img, sizeof img);
    CHECK("encode stores ONLY the 3 generation-free grants (header + 3 records + checksum); the 2 instance-bound grants are RAM-only", n == 12 + 3 * 16 + 4);
    CHECK("encode refuses a buffer that is too small", passport_encode(&P, img, n - 1) == 0 && passport_encode(&P, 0, 100) == 0);
    { struct passport Q; CHECK("decode restores exactly the same permissions (restart)", passport_decode(&Q, img, n) == PP_OK && Q.loaded_ok && passport_check(&Q, 0x1001, 0, PP_NAVIGATE) && passport_check(&Q, 0x1002, 0, PP_SETTINGS_PROPOSE) && passport_check(&Q, 0x1002, 0, PP_FILES_WRITE) && !passport_check(&Q, 0x1001, 0, PP_FILES_DELETE) && !passport_check(&Q, 0x1003, 55, PP_NAVIGATE) && !passport_check(&Q, 0x1003, 0xFFFFFFFFFFFFFFFFULL, PP_FILES_WRITE));
      struct passport Q2; passport_decode(&Q2, img, n); uint8_t re[PP_IMAGE_MAX]; size_t m = passport_encode(&Q2, re, sizeof re); CHECK("encode(decode(x)) is byte-identical", m == n && memcmp(re, img, n) == 0); }
    { /* a well-formed image with an instance-bound record and a valid checksum is refused: process generations restart at 1 every boot */
      uint8_t c[PP_IMAGE_MAX]; memcpy(c, img, n); c[12 + 4] = 5;                       /* record 0: generation low byte = 5 */
      uint32_t h = 2166136261u; for (size_t i = 0; i < n - 4; i++) { h ^= c[i]; h *= 16777619u; } c[n - 4] = (uint8_t)h; c[n - 3] = (uint8_t)(h >> 8); c[n - 2] = (uint8_t)(h >> 16); c[n - 1] = (uint8_t)(h >> 24);
      struct passport Q; CHECK("an image containing a generation-bound grant (valid checksum) is rejected and fails closed", passport_decode(&Q, c, n) == PP_E_FORMAT && !Q.loaded_ok && !passport_peek(&Q, 0x1001, 0, PP_NAVIGATE)); }
    { /* a boot-1 grant bound to generation 5 must not exist on boot 2, where an unrelated process may also be generation 5 */
      struct passport A; passport_init(&A); passport_grant(&A, &KERNEL, 406, 5, PP_FILES_DELETE); uint8_t b2[PP_IMAGE_MAX]; size_t m2 = passport_encode(&A, b2, sizeof b2); struct passport B2; passport_decode(&B2, b2, m2);
      CHECK("bound_grants_never_persist: after a restart no process (whatever its generation) inherits an instance grant", m2 == 16 && !passport_peek(&B2, 406, 5, PP_FILES_DELETE) && !passport_peek(&B2, 406, 0, PP_FILES_DELETE)); }
    { int bad = 0;
      for (size_t i = 0; i < n; i++) for (int bit = 0; bit < 8; bit++) { uint8_t c[PP_IMAGE_MAX]; memcpy(c, img, n); c[i] ^= (uint8_t)(1 << bit); struct passport Q; passport_init(&Q); passport_grant(&Q, &KERNEL, 1, 0, PP_NAVIGATE); int rc = passport_decode(&Q, c, n);
          int any = 0; for (int k = 0; k < PP_MAX_GRANTS; k++) if (Q.g[k].used) any = 1;
          if (rc == PP_OK || Q.loaded_ok || any) bad++; }
      CHECK("every single-bit flip of the image is detected and leaves an EMPTY table (fail closed)", bad == 0); }
    { int bad = 0; for (size_t cut = 0; cut < n; cut++) { struct passport Q; int rc = passport_decode(&Q, img, cut); int any = 0; for (int k = 0; k < PP_MAX_GRANTS; k++) if (Q.g[k].used) any = 1; if (rc == PP_OK || any) bad++; }
      CHECK("every truncation is detected and leaves an empty table", bad == 0); }
    { uint8_t c[PP_IMAGE_MAX]; memcpy(c, img, n); c[4] = 2; struct passport Q; CHECK("an unknown (newer) version is refused, not guessed at", passport_decode(&Q, c, n) == PP_E_VERSION && !Q.loaded_ok); }
    { uint8_t c[PP_IMAGE_MAX]; memcpy(c, img, n); c[0] = 'X'; struct passport Q; CHECK("a wrong magic is refused", passport_decode(&Q, c, n) == PP_E_FORMAT); }
    { uint8_t c[PP_IMAGE_MAX + 8]; memcpy(c, img, n); c[n] = 0; struct passport Q; CHECK("trailing bytes are refused", passport_decode(&Q, c, n + 1) == PP_E_FORMAT); }
    { struct passport Q; CHECK("NULL / empty input is refused", passport_decode(&Q, 0, 0) == PP_E_FORMAT && passport_decode(&Q, img, 0) == PP_E_FORMAT); }
    { /* a well-formed image with a duplicate grant, a principal 0 grant or an unknown permission is rejected even with a correct checksum */
      struct passport A; passport_init(&A); passport_grant(&A, &KERNEL, 7, 0, PP_NAVIGATE);
      uint8_t c[PP_IMAGE_MAX]; size_t m = passport_encode(&A, c, sizeof c);
      uint8_t d[PP_IMAGE_MAX]; memcpy(d, c, m);
      d[5] = 2; memcpy(d + 12 + 16, d + 12, 16);                                                                             /* two identical records */
      uint32_t h = 2166136261u; for (size_t i = 0; i < 12 + 32; i++) { h ^= d[i]; h *= 16777619u; } d[44] = (uint8_t)h; d[45] = (uint8_t)(h >> 8); d[46] = (uint8_t)(h >> 16); d[47] = (uint8_t)(h >> 24);
      struct passport Q; CHECK("a duplicate record with a valid checksum is rejected", passport_decode(&Q, d, 48) == PP_E_FORMAT);
      memcpy(d, c, m); d[12 + 12] = 99; h = 2166136261u; for (size_t i = 0; i < 12 + 16; i++) { h ^= d[i]; h *= 16777619u; } d[28] = (uint8_t)h; d[29] = (uint8_t)(h >> 8); d[30] = (uint8_t)(h >> 16); d[31] = (uint8_t)(h >> 24);
      CHECK("an unknown permission with a valid checksum is rejected", passport_decode(&Q, d, 32) == PP_E_FORMAT);
      memcpy(d, c, m); d[12] = d[13] = d[14] = d[15] = 0; h = 2166136261u; for (size_t i = 0; i < 12 + 16; i++) { h ^= d[i]; h *= 16777619u; } d[28] = (uint8_t)h; d[29] = (uint8_t)(h >> 8); d[30] = (uint8_t)(h >> 16); d[31] = (uint8_t)(h >> 24);
      CHECK("a grant to principal 0 with a valid checksum is rejected", passport_decode(&Q, d, 32) == PP_E_FORMAT); }

    /* ---- storage: save, load, restart, failure ---- */
    passport_init(&P); passport_grant(&P, &KERNEL, 0x1001, 0, PP_NAVIGATE); fail_write = fail_read = torn = 0; disk_len = 0;
    CHECK("save to a working store succeeds", passport_save(&P, &STORE) == PP_OK && disk_len > 0);
    { struct passport Q; CHECK("a NEW table loaded from the store has the permission (restart)", passport_load(&Q, &STORE) == PP_OK && passport_check(&Q, 0x1001, 0, PP_NAVIGATE)); }
    fail_write = 1; CHECK("a failed write is reported and the in-memory table is unchanged", passport_save(&P, &STORE) == PP_E_STORE && passport_check(&P, 0x1001, 0, PP_NAVIGATE)); fail_write = 0;
    fail_read = 1; { struct passport Q; passport_init(&Q); passport_grant(&Q, &KERNEL, 1, 0, PP_NAVIGATE); CHECK("a failed read leaves an EMPTY, denying table", passport_load(&Q, &STORE) == PP_E_STORE && !Q.loaded_ok && !passport_check(&Q, 0x1001, 0, PP_NAVIGATE)); } fail_read = 0;
    torn = 1; passport_save(&P, &STORE); torn = 0; { struct passport Q; CHECK("a torn (half-written) image is refused and fails closed", passport_load(&Q, &STORE) != PP_OK && !passport_check(&Q, 0x1001, 0, PP_NAVIGATE)); }
    CHECK("NULL store callbacks fail closed", passport_save(&P, 0) == PP_E_STORE && passport_load(&P, 0) == PP_E_STORE && !P.loaded_ok);

    /* ---- randomised: the ledger agrees with a simple model ---- */
    {
        passport_init(&P);
        struct { uint32_t pr; uint64_t gen; uint8_t perm; int on; } model[64]; int mn = 0; int bad = 0;
        for (int it = 0; it < 30000; it++) {
            uint32_t pr = 0x4000 + (uint32_t)(rnd() % 6); uint8_t perm = (uint8_t)(1 + rnd() % (PP_COUNT - 1)); uint64_t gen = (rnd() % 3) ? 0 : 1 + rnd() % 3;   /* at most 6 principals x 7 perms x 4 gens */
            switch (rnd() % 4) {
            case 0: { int rc = passport_grant(&P, &KERNEL, pr, gen, perm);
                      int found = -1; for (int i = 0; i < mn; i++) if (model[i].on && model[i].pr == pr && model[i].perm == perm && model[i].gen == gen) found = i;
                      int live = 0; for (int i = 0; i < mn; i++) if (model[i].on) live++;
                      int held = 0; for (int i = 0; i < mn; i++) if (model[i].on && model[i].pr == pr) held++;
                      if (found >= 0) { if (rc != PP_OK) bad++; } else if (held >= PP_PER_PRINCIPAL) { if (rc != PP_E_QUOTA) bad++; } else if (live >= PP_MAX_GRANTS) { if (rc != PP_E_FULL) bad++; } else { if (rc != PP_OK) bad++; else { int s = -1; for (int i = 0; i < mn; i++) if (!model[i].on) { s = i; break; } if (s < 0) s = mn++; model[s].pr = pr; model[s].gen = gen; model[s].perm = perm; model[s].on = 1; } }
                      break; }
            case 1: { int rc = passport_revoke(&P, &KERNEL, pr, perm); int n2 = 0; for (int i = 0; i < mn; i++) if (model[i].on && model[i].pr == pr && model[i].perm == perm) { model[i].on = 0; n2++; } if ((rc == PP_OK) != (n2 > 0)) bad++; break; }
            case 2: { int rc = passport_grant(&P, &PROC, pr, gen, perm); if (rc != PP_E_ACTOR) bad++; break; }
            default: { uint64_t g = rnd() % 5; int want = 0; for (int i = 0; i < mn; i++) if (model[i].on && model[i].pr == pr && model[i].perm == perm && (model[i].gen == 0 || (g != 0 && model[i].gen == g))) want = 1;
                       { if (passport_check(&P, pr, g, perm) != want) bad++; } break; }
            }
            if (it % 500 == 0) { uint8_t b[PP_IMAGE_MAX]; size_t m = passport_encode(&P, b, sizeof b); struct passport Q; if (!m || passport_decode(&Q, b, m) != PP_OK) bad++; else for (int k = 0; k < 8; k++) { uint32_t p2 = 0x4000 + (uint32_t)(rnd() % 6); uint8_t pm = (uint8_t)(1 + rnd() % (PP_COUNT - 1)); uint64_t g = 0; if (passport_check(&Q, p2, g, pm) != passport_check(&P, p2, g, pm)) bad++; } }
        }
        CHECK("30,000 random grant/revoke/refused-grant/check steps (with periodic save-restart) agree with an independent model", bad == 0);
    }

    printf("passport: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
