/* tests/kernel-host/trail_test.c - the Trail record chain (kernel/ui/trail.c): sequence numbers, hash chain, bounded ring with eviction, commit-only-after-
 * store, storage failure and torn writes, restart, damage recovery (longest verified prefix), version/format refusal and a randomised model check.
 * Host, ASan/UBSan. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <trail.h>

static int passed, failed;
#define CHECK(name, cond) do { if (cond) { passed++; } else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)

static uint8_t disk[TR_IMAGE_MAX + 64]; static size_t disk_len; static int fail_write, fail_read, torn, writes;
static int st_write(const uint8_t *b, size_t n) { writes++; if (fail_write) return -1; memcpy(disk, b, n); disk_len = torn ? n / 2 : n; return 0; }
static int st_read(uint8_t *b, size_t cap, size_t *len) { if (fail_read) return -1; if (disk_len > cap) return -1; memcpy(b, disk, disk_len); *len = disk_len; return 0; }
static const struct tr_store STORE = { st_write, st_read };
static struct trail T;
static uint64_t xs = 0xCAFEF00DD15EA5E5ULL;
static uint64_t rnd(void) { xs ^= xs << 13; xs ^= xs >> 7; xs ^= xs << 17; return xs; }

static struct trail_rec mk(uint32_t pid, uint8_t kind, uint8_t state)
{
    struct trail_rec r; memset(&r, 0, sizeof r);
    r.pid = pid; r.principal = 0x1000 + pid; r.kind = kind; r.target = 3; r.risk = 0; r.decision = 1; r.reason = 0; r.state = state; r.arg = -7; r.uptime_ms = 12345;
    return r;
}
static void reset(void) { trail_init(&T); memset(disk, 0, sizeof disk); disk_len = 0; fail_write = fail_read = torn = writes = 0; }

int main(void)
{
    printf("trail tests\n");

    reset();
    CHECK("a new trail is empty, healthy, verifies, and starts at sequence 1", T.n == 0 && T.healthy && trail_verify(&T) == TR_OK && T.next_seq == 1);
    struct trail_rec r = mk(2, 1, 1);
    CHECK("append succeeds, assigns sequence 1 and a chain value, and persists", trail_append(&T, &STORE, &r) == TR_OK && T.n == 1 && T.r[0].seq == 1 && T.r[0].chain != 0 && disk_len > 0 && writes == 1);
    CHECK("the caller's seq/chain fields are ignored (assigned by the trail)", ({ struct trail_rec x = mk(2, 1, 1); x.seq = 999; x.chain = 0xDEAD; trail_append(&T, &STORE, &x) == TR_OK && T.r[1].seq == 2 && T.r[1].chain != 0xDEAD; }));
    CHECK("sequence numbers are strictly increasing and the chain verifies", T.r[1].seq == T.r[0].seq + 1 && trail_verify(&T) == TR_OK && T.r[1].chain != T.r[0].chain);
    CHECK("a record keeps the requester, action, decision and ACTUAL result, with uptime (not wall-clock) time", T.r[0].pid == 2 && T.r[0].principal == 0x1002 && T.r[0].kind == 1 && T.r[0].state == 1 && T.r[0].uptime_ms == 12345 && T.r[0].arg == -7);

    /* ---- argument validation ---- */
    { struct trail_rec b = mk(1, 99, 1); CHECK("an out-of-range kind is refused", trail_append(&T, &STORE, &b) == TR_E_ARG);
      b = mk(1, 1, 99); CHECK("an out-of-range state is refused", trail_append(&T, &STORE, &b) == TR_E_ARG);
      b = mk(1, 1, 1); b.risk = 4; CHECK("risk 4 is refused", trail_append(&T, &STORE, &b) == TR_E_ARG);
      b = mk(1, 1, 1); b.decision = 5; CHECK("decision 5 is refused", trail_append(&T, &STORE, &b) == TR_E_ARG);
      b = mk(1, 1, 1); b.reason = 17; CHECK("reason 17 is refused", trail_append(&T, &STORE, &b) == TR_E_ARG);
      CHECK("NULL arguments are refused", trail_append(0, &STORE, &b) == TR_E_ARG && trail_append(&T, 0, &b) == TR_E_ARG && trail_append(&T, &STORE, 0) == TR_E_ARG);
      CHECK("a refused append left the trail unchanged", T.n == 2 && trail_verify(&T) == TR_OK); }

    /* ---- storage failure: not committed in memory either ---- */
    { uint32_t n0 = T.n, s0 = T.next_seq; struct trail_rec b = mk(3, 1, 1); fail_write = 1;
      CHECK("a failed write returns STORE and does NOT commit the record in memory (the caller must not run the action)", trail_append(&T, &STORE, &b) == TR_E_STORE && T.n == n0 && T.next_seq == s0 && trail_verify(&T) == TR_OK);
      fail_write = 0; CHECK("after the store recovers the same record appends with the NEXT sequence number (no gap, no duplicate)", trail_append(&T, &STORE, &b) == TR_OK && T.r[T.n - 1].seq == s0); }

    /* ---- restart ---- */
    { struct trail Q; CHECK("a new trail loaded from disk equals the written one (restart)", trail_load(&Q, &STORE) == TR_OK && Q.healthy && Q.n == T.n && Q.next_seq == T.next_seq && !memcmp(Q.r, T.r, sizeof(struct trail_rec) * T.n) && trail_verify(&Q) == TR_OK);
      struct trail_rec b = mk(4, 2, 3); CHECK("the restarted trail continues the sequence and the chain", trail_append(&Q, &STORE, &b) == TR_OK && Q.r[Q.n - 1].seq == T.next_seq && trail_verify(&Q) == TR_OK);
      CHECK("encode(decode(x)) is byte-identical", ({ uint8_t a[TR_IMAGE_MAX], c[TR_IMAGE_MAX]; struct trail Z; size_t la = trail_encode(&T, a, sizeof a); trail_decode(&Z, a, la); size_t lc = trail_encode(&Z, c, sizeof c); la == lc && !memcmp(a, c, la); })); }
    fail_read = 1; { struct trail Q; CHECK("a failed read leaves an empty, UNHEALTHY trail", trail_load(&Q, &STORE) == TR_E_STORE && Q.n == 0 && !Q.healthy); } fail_read = 0;
    CHECK("a NULL store fails closed", ({ struct trail Q; trail_load(&Q, 0) == TR_E_STORE && !Q.healthy; }));

    /* ---- torn write ---- */
    { struct trail_rec b = mk(5, 1, 1); torn = 1; trail_append(&T, &STORE, &b); torn = 0;
      struct trail Q; CHECK("a half-written image is refused (length mismatch) and leaves an empty unhealthy trail", trail_load(&Q, &STORE) != TR_OK && Q.n == 0 && !Q.healthy); reset(); }

    /* ---- the chain detects edits, reordering, deletion, splicing ---- */
    reset(); for (int i = 0; i < 6; i++) { struct trail_rec b = mk((uint32_t)(i % 4) + 1, (uint8_t)(i % 3), 1); trail_append(&T, &STORE, &b); }
    CHECK("six records verify", T.n == 6 && trail_verify(&T) == TR_OK);
    { struct trail C = T; C.r[2].state = 2; CHECK("editing a record's RESULT breaks the chain", trail_verify(&C) == TR_E_CHAIN); C = T; C.r[3].principal ^= 1; CHECK("editing the principal breaks it", trail_verify(&C) == TR_E_CHAIN);
      C = T; struct trail_rec tmp = C.r[1]; C.r[1] = C.r[2]; C.r[2] = tmp; CHECK("swapping two records breaks it", trail_verify(&C) != TR_OK);
      C = T; for (uint32_t i = 2; i + 1 < C.n; i++) C.r[i] = C.r[i + 1]; C.n--; CHECK("deleting a record from the middle breaks it", trail_verify(&C) != TR_OK);
      C = T; C.n--; CHECK("dropping the newest record is detected (the sequence counter no longer matches)", trail_verify(&C) != TR_OK);
      C = T; C.r[4].seq = 99; CHECK("a sequence jump breaks it", trail_verify(&C) != TR_OK); C = T; C.r[0].uptime_ms ^= 1; CHECK("editing the uptime breaks it", trail_verify(&C) == TR_E_CHAIN); }

    /* ---- damage recovery: keep the longest verified prefix, refuse appends ---- */
    { uint8_t img[TR_IMAGE_MAX]; size_t n = trail_encode(&T, img, sizeof img);
      img[24 + 3 * TR_REC_BYTES + 17] ^= 1;                                                        /* damage record index 3's result */
      struct trail Q; int rc = trail_decode(&Q, img, n);
      CHECK("damage inside record 3: the first 3 records are recovered, 3 are reported lost, the trail is UNHEALTHY", rc == TR_E_CHAIN && Q.recovered == 3 && Q.lost == 3 && Q.n == 3 && !Q.healthy && trail_verify(&Q) == TR_OK);
      struct trail_rec b = mk(1, 1, 1); CHECK("a damaged trail is not extended (append refused)", trail_append(&Q, &STORE, &b) != TR_OK);
      n = trail_encode(&T, img, sizeof img); img[n - 1] ^= 1; CHECK("damage only in the checksum: all records verify but the trail is still marked unhealthy", trail_decode(&Q, img, n) == TR_E_CHAIN && Q.n == 6 && !Q.healthy); }
    { uint8_t img[TR_IMAGE_MAX]; size_t n = trail_encode(&T, img, sizeof img); int bad = 0;
      for (size_t i = 0; i < n; i++) for (int bit = 0; bit < 8; bit++) { uint8_t c[TR_IMAGE_MAX]; memcpy(c, img, n); c[i] ^= (uint8_t)(1 << bit); struct trail Q; int rc = trail_decode(&Q, c, n); if (rc == TR_OK || Q.healthy) bad++; if (trail_verify(&Q) != TR_OK && Q.n) bad++; }
      CHECK("EVERY single-bit flip of the image is detected (never reported healthy) and the retained prefix always verifies", bad == 0);
      bad = 0; for (size_t cut = 0; cut < n; cut++) { struct trail Q; if (trail_decode(&Q, img, cut) == TR_OK) bad++; }
      CHECK("every truncation is refused", bad == 0);
      uint8_t c[TR_IMAGE_MAX]; memcpy(c, img, n); c[4] = 9; struct trail Q; CHECK("an unknown version is refused", trail_decode(&Q, c, n) == TR_E_VERSION && Q.n == 0 && !Q.healthy);
      memcpy(c, img, n); c[0] = 'X'; CHECK("a wrong magic is refused", trail_decode(&Q, c, n) == TR_E_FORMAT);
      CHECK("NULL and tiny inputs are refused", trail_decode(&Q, 0, 0) == TR_E_FORMAT && trail_decode(&Q, img, 5) == TR_E_FORMAT); }

    /* ---- sequence limits and the anchor head ---- */
    reset(); { struct trail_rec b = mk(1, 1, 1); trail_append(&T, &STORE, &b); trail_append(&T, &STORE, &b);
      CHECK("trail_head is the newest record's chain value, and base_chain when empty", trail_head(&T) == T.r[1].chain && ({ struct trail E; trail_init(&E); trail_head(&E) == 0; }));
      uint8_t img[TR_IMAGE_MAX]; size_t n = trail_encode(&T, img, sizeof img); struct trail Q;
      uint8_t c[TR_IMAGE_MAX]; memcpy(c, img, n); c[24 + 0] = 0; c[24 + 1] = 0; c[24 + 2] = 0; c[24 + 3] = 0;
      CHECK("decode_rejects_seq_zero: a record with sequence 0 is damage (prefix empty, unhealthy)", trail_decode(&Q, c, n) != TR_OK && Q.n == 0 && !Q.healthy);
      memcpy(c, img, n); c[24] = 0xFF; c[25] = 0xFF; c[26] = 0xFF; c[27] = 0xFF;
      CHECK("decode_rejects_seq_max: sequence 0xFFFFFFFF (which would wrap next_seq to 0) is damage", trail_decode(&Q, c, n) != TR_OK && Q.healthy == 0);
      memcpy(c, img, n); c[8] = c[9] = c[10] = c[11] = 0;
      CHECK("a header next_seq of 0 is refused outright", trail_decode(&Q, c, n) == TR_E_FORMAT && Q.n == 0 && !Q.healthy);
      T.next_seq = 0xFFFFFFFFu; { struct trail_rec x = mk(1, 1, 1); CHECK("append refuses to issue the last sequence number (no wrap)", trail_append(&T, &STORE, &x) == TR_E_ARG); } }

    /* ---- bounded ring: eviction keeps the retained records verifiable ---- */
    reset();
    for (int i = 0; i < TR_MAX + 20; i++) { struct trail_rec b = mk((uint32_t)(i % 8), (uint8_t)(i % 5), (uint8_t)(i % 7)); b.arg = i; if (trail_append(&T, &STORE, &b) != TR_OK) { CHECK("append within a long run", 0); break; } }
    CHECK("the ring holds exactly TR_MAX records, the oldest 20 were evicted, sequences continue, and what remains verifies", T.n == TR_MAX && T.evicted == 20 && T.r[0].seq == 21 && T.r[TR_MAX - 1].seq == TR_MAX + 20 && trail_verify(&T) == TR_OK);
    { struct trail Q; CHECK("a full, evicted trail survives a restart intact", trail_load(&Q, &STORE) == TR_OK && Q.n == TR_MAX && Q.evicted == 20 && Q.r[0].seq == 21 && trail_verify(&Q) == TR_OK); }
    { uint8_t img[TR_IMAGE_MAX]; size_t n = trail_encode(&T, img, sizeof img); CHECK("encode refuses a too-small buffer", trail_encode(&T, img, n - 1) == 0 && trail_encode(&T, 0, n) == 0); }

    /* ---- randomised: random appends, failures and restarts agree with a model ---- */
    reset();
    { uint32_t model_n = 0, model_next = 1; int bad = 0;
      for (int it = 0; it < 6000; it++) {
          uint64_t r2 = rnd();
          if (r2 % 17 == 0) { struct trail Q; if (trail_load(&Q, &STORE) == TR_OK) T = Q; else if (disk_len) bad++; if (T.next_seq != model_next) bad++; continue; }
          struct trail_rec b = mk((uint32_t)(r2 % 9), (uint8_t)((r2 >> 8) % 6), (uint8_t)((r2 >> 16) % 7)); b.arg = (int32_t)(r2 >> 24); b.decision = (uint8_t)((r2 >> 40) % 5);
          fail_write = (r2 % 11 == 0);
          int rc = trail_append(&T, &STORE, &b);
          if (fail_write) { if (rc != TR_E_STORE) bad++; } else { if (rc != TR_OK) bad++; else { model_n = model_n < TR_MAX ? model_n + 1 : TR_MAX; model_next++; } }
          fail_write = 0;
          if (T.n != model_n || T.next_seq != model_next || trail_verify(&T) != TR_OK) bad++;
      }
      CHECK("6000 random appends with injected store failures and restarts: counts, sequence, chain and disk always agree", bad == 0); }

    printf("trail: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
