/* tests/kernel-host/ui_proto_test.c - host tests (ASan/UBSan) for kernel/ui/ui_proto.c, the EL0 UI protocol v1.
 * Golden byte vectors are written out by hand (not produced by the encoders), the negative table covers every case in the Cycle 17 brief, and a
 * randomized run is checked against an independent oracle model written in this file (its own idea of ownership, quotas and visibility). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ui_proto.h>

static int passed, failed;
#define CHECK(name, cond) do { if (cond) { passed++; } else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)

static struct uip_state S;
static const struct uip_caller A = { 1, 100, 1001, 1 }, B = { 2, 200, 1002, 1 }, C3 = { 3, 300, 1003, 1 };
static uint8_t rep[UIP_MSG_MAX]; static uint32_t rlen;
static int send(const struct uip_caller *w, const uint8_t *m, uint32_t n) { rlen = 0; return uip_handle(&S, w, m, n, rep, sizeof rep, &rlen); }
static void init(void) { uip_init(&S, 40, 96, 944, 520); }
static uint32_t reply_handle(void) { return (uint32_t)rep[4] | ((uint32_t)rep[5] << 8) | ((uint32_t)rep[6] << 16) | ((uint32_t)rep[7] << 24); }

static uint16_t seqn;
static uint32_t create(const struct uip_caller *w, uint8_t sc, const char *title)
{
    uint8_t b[64]; uint32_t n = uip_encode_create(b, sizeof b, ++seqn, sc, title);
    int rc = send(w, b, n); uip_reply_consumed(&S, w->pid, w->gen);
    return rc == UIP_OK ? reply_handle() : 0;
}
static int do_text(const struct uip_caller *w, uint32_t h, uint8_t wi, const char *t) { uint8_t b[64]; uint32_t n = uip_encode_set_text(b, sizeof b, ++seqn, h, wi, 1, t); int rc = send(w, b, n); uip_reply_consumed(&S, w->pid, w->gen); return rc; }
static int do_rect(const struct uip_caller *w, uint32_t h, uint8_t wi, int x, int y, int ww, int hh) { uint8_t b[64]; uint32_t n = uip_encode_set_rect(b, sizeof b, ++seqn, h, wi, 0, (int16_t)x, (int16_t)y, (uint16_t)ww, (uint16_t)hh); int rc = send(w, b, n); uip_reply_consumed(&S, w->pid, w->gen); return rc; }
static int do_simple(const struct uip_caller *w, uint8_t type, uint32_t h) { uint8_t b[64]; uint32_t n = uip_encode_simple(b, sizeof b, type, ++seqn, h); int rc = send(w, b, n); uip_reply_consumed(&S, w->pid, w->gen); return rc; }

int main(void)
{
    printf("ui_proto tests\n");

    /* ---- golden wire vectors (hand-written bytes) ---- */
    {
        static const uint8_t g_create[] = { 1, 1, 0x34, 0x12, 0, 0, 0, 0, 1, 3, 'A', 'b', 'c' };
        struct uip_msg m;
        CHECK("golden CREATE decodes: seq 0x1234 (little endian), size class 1, title Abc", uip_decode(g_create, sizeof g_create, &m) == UIP_OK && m.type == UIP_CREATE_SURFACE && m.seq == 0x1234 && m.a == 1 && m.tlen == 3 && !strcmp(m.text, "Abc") && m.handle == 0);
        uint8_t b[64];
        CHECK("the encoder produces exactly the golden CREATE bytes", uip_encode_create(b, sizeof b, 0x1234, 1, "Abc") == sizeof g_create && !memcmp(b, g_create, sizeof g_create));
        static const uint8_t g_text[] = { 1, 2, 5, 0, 0x02, 0x01, 0, 0, 7, 3, 2, 'h', 'i' };
        CHECK("golden SET_TEXT decodes: handle 0x102, widget 7, style 3, text hi", uip_decode(g_text, sizeof g_text, &m) == UIP_OK && m.handle == 0x102 && m.a == 7 && m.b == 3 && !strcmp(m.text, "hi"));
        CHECK("the encoder produces exactly the golden SET_TEXT bytes", uip_encode_set_text(b, sizeof b, 5, 0x102, 7, 3, "hi") == sizeof g_text && !memcmp(b, g_text, sizeof g_text));
        static const uint8_t g_rect[] = { 1, 3, 9, 0, 0x01, 0x03, 0, 0, 2, 1, 0x0A, 0x00, 0x14, 0x00, 0x2C, 0x01, 0x90, 0x00 };
        CHECK("golden SET_RECT decodes: widget 2, role 1, x 10, y 20, w 300, h 144", uip_decode(g_rect, sizeof g_rect, &m) == UIP_OK && m.a == 2 && m.b == 1 && m.x == 10 && m.y == 20 && m.w == 300 && m.h == 144 && m.handle == 0x301);
        CHECK("the encoder produces exactly the golden SET_RECT bytes", uip_encode_set_rect(b, sizeof b, 9, 0x301, 2, 1, 10, 20, 300, 144) == sizeof g_rect && !memcmp(b, g_rect, sizeof g_rect));
        static const uint8_t g_status[] = { 1, 0x81, 0x07, 0x00, 0x05, 0x01, 0, 0, 0xFA, 0xFF, 2 };
        CHECK("the STATUS reply for seq 7, handle 0x105, code -6, request SET_TEXT is the golden bytes", uip_encode_status(b, sizeof b, 7, 0x105, UIP_ESTALE, UIP_SET_TEXT) == sizeof g_status && !memcmp(b, g_status, sizeof g_status));
        static const uint8_t g_key[] = { 1, 0x82, 0x01, 0x00, 0x02, 0x01, 0, 0, 0x1E, 0x00, 0x02, 0x00, 1, 0 };
        CHECK("EVENT_KEY golden bytes (code 30, mods 2, down)", uip_encode_key(b, sizeof b, 1, 0x102, 30, 2, 1, 0) == sizeof g_key && !memcmp(b, g_key, sizeof g_key));
        static const uint8_t g_ptr[] = { 1, 0x83, 0x02, 0x00, 0x02, 0x01, 0, 0, 1, 1, 0xFE, 0xFF, 0x05, 0x00 };
        CHECK("EVENT_POINTER golden bytes (down, button 1, x -2, y 5)", uip_encode_pointer(b, sizeof b, 2, 0x102, 1, 1, -2, 5) == sizeof g_ptr && !memcmp(b, g_ptr, sizeof g_ptr));
        CHECK("every message fits the 64-byte IPC message: the largest client message (SET_TEXT with 40 bytes) is 51 bytes", uip_encode_set_text(b, sizeof b, 1, 1, 0, 0, "0123456789012345678901234567890123456789") == 51 && uip_encode_set_text(b, sizeof b, 1, 1, 0, 0, "01234567890123456789012345678901234567890") == 0);
        CHECK("an encoder refuses a buffer that is too small", uip_encode_status(b, 10, 1, 1, 0, 1) == 0 && uip_encode_create(b, 9, 1, 0, "") == 0);
    }

    /* ---- the decoder's negative table ---- */
    {
        struct uip_msg m; uint8_t b[80];
        uint32_t n = uip_encode_set_text(b, sizeof b, 1, 0x101, 1, 1, "ok");
        CHECK("empty buffer is truncated", uip_decode(b, 0, &m) == UIP_ETRUNC);
        CHECK("NULL buffer and NULL output are refused", uip_decode(0, 5, &m) != UIP_OK && uip_decode(b, n, 0) != UIP_OK);
        uint8_t v[64]; memcpy(v, b, n); v[0] = 2;
        CHECK("wrong protocol version (2)", uip_decode(v, n, &m) == UIP_EVERSION);
        v[0] = 0; CHECK("wrong protocol version (0)", uip_decode(v, n, &m) == UIP_EVERSION);
        memcpy(v, b, n); v[1] = 0x7f;
        CHECK("unknown message type", uip_decode(v, n, &m) == UIP_ETYPE);
        v[1] = 0; CHECK("type 0 is unknown", uip_decode(v, n, &m) == UIP_ETYPE);
        v[1] = UIP_STATUS; CHECK("a client cannot send a compositor-to-client type (STATUS)", uip_decode(v, n, &m) == UIP_ETYPE);
        v[1] = UIP_EVENT_KEY; CHECK("nor EVENT_KEY", uip_decode(v, n, &m) == UIP_ETYPE);
        v[1] = UIP_CLOSE_REQUEST; CHECK("nor CLOSE_REQUEST", uip_decode(v, n, &m) == UIP_ETYPE);
        CHECK("shorter than the header is truncated", uip_decode(b, 7, &m) == UIP_ETRUNC);
        CHECK("SET_TEXT without its length byte is truncated", uip_decode(b, 10, &m) == UIP_ETRUNC);
        CHECK("SET_TEXT whose text is cut short is truncated", uip_decode(b, n - 1, &m) == UIP_ETRUNC);
        memset(v, 'x', sizeof v); memcpy(v, b, 11); v[10] = 2;
        CHECK("SET_TEXT with trailing bytes is rejected as too long", uip_decode(v, n + 1, &m) == UIP_EBADLEN);
        memcpy(v, b, n); v[10] = 41; memset(v + 11, 'a', 41);
        CHECK("an oversized embedded text length (41) is rejected", uip_decode(v, 52, &m) == UIP_EBADLEN);
        v[10] = 255;
        CHECK("an embedded text length of 255 is rejected", uip_decode(v, 64, &m) == UIP_EBADLEN);
        memset(v, 0, sizeof v); v[0] = 1; v[1] = UIP_COMMIT;
        CHECK("a message longer than 64 bytes is rejected", uip_decode(v, 65, &m) == UIP_EBADLEN && uip_decode(v, 4096, &m) == UIP_EBADLEN);
        CHECK("COMMIT with a payload is rejected", uip_decode(v, 9, &m) == UIP_EBADLEN && uip_decode(v, 8, &m) == UIP_OK);
        uint8_t r[18]; uip_encode_set_rect(r, sizeof r, 1, 0x101, 3, 1, 5, 5, 10, 10);
        CHECK("SET_RECT truncated and over-long", uip_decode(r, 17, &m) == UIP_ETRUNC && ({ uint8_t q[19]; memcpy(q, r, 18); q[18] = 0; uip_decode(q, 19, &m); }) == UIP_EBADLEN);
        r[8] = 16; CHECK("invalid widget index 16", uip_decode(r, 18, &m) == UIP_EBADARG);
        r[8] = 255; CHECK("invalid widget index 255", uip_decode(r, 18, &m) == UIP_EBADARG);
        r[8] = 3; r[9] = 4; CHECK("invalid role 4", uip_decode(r, 18, &m) == UIP_EBADARG);
        r[9] = 1; r[14] = r[15] = 0; CHECK("zero width", uip_decode(r, 18, &m) == UIP_EBADARG);
        r[14] = 10; r[16] = r[17] = 0; CHECK("zero height", uip_decode(r, 18, &m) == UIP_EBADARG);
        uint8_t t[64]; uip_encode_set_text(t, sizeof t, 1, 0x101, 0, 4, "a");
        CHECK("invalid style 4", uip_decode(t, 12, &m) == UIP_EBADARG);
        uip_encode_set_text(t, sizeof t, 1, 0x101, 0, 0, "a"); t[11] = 0x1f;
        CHECK("control character in text (0x1F)", uip_decode(t, 12, &m) == UIP_EBADTEXT);
        t[11] = 0x7f; CHECK("DEL in text", uip_decode(t, 12, &m) == UIP_EBADTEXT);
        t[11] = 0xC3; CHECK("a non-ASCII byte in text (UTF-8 lead 0xC3): invalid encoding", uip_decode(t, 12, &m) == UIP_EBADTEXT);
        t[11] = 0; CHECK("an embedded NUL in text", uip_decode(t, 12, &m) == UIP_EBADTEXT);
        uint8_t c[64]; uint32_t cn = uip_encode_create(c, sizeof c, 1, 0, "Title");
        c[8] = 3; CHECK("invalid size class 3", uip_decode(c, cn, &m) == UIP_EBADARG);
        c[8] = 0; c[4] = 1; CHECK("CREATE with a non-zero handle", uip_decode(c, cn, &m) == UIP_EBADARG);
        c[4] = 0; c[9] = 25; CHECK("a 25-byte title is too long", uip_decode(c, 10 + 25, &m) == UIP_EBADLEN);
        c[9] = 5; c[12] = 0x80; CHECK("a non-ASCII title byte", uip_decode(c, cn, &m) == UIP_EBADTEXT);
        CHECK("the decoder leaves *out untouched on failure", ({ struct uip_msg q; memset(&q, 0x5a, sizeof q); uip_decode(c, cn, &q); ((uint8_t *)&q)[0]; }) == 0x5a);
    }

    /* ---- creating, staging and committing ---- */
    init();
    {
        uint32_t h = create(&A, 1, "Demo");
        CHECK("a client with the capability creates a surface and gets a non-zero handle", h != 0 && uip_slot_of(h) >= 0);
        CHECK("the handle is (serial << 8) | slot with a 16-bit non-zero serial and no pointer bits", (h >> 24) == 0 && (h >> 8) != 0 && (h & 0xff) < UIP_MAX_SURFACES);
        CHECK("the reply is a STATUS for the request: type, code 0, request type CREATE, echoed seq", rep[1] == UIP_STATUS && rlen == 11 && rep[8] == 0 && rep[9] == 0 && rep[10] == UIP_CREATE_SURFACE);
        const struct uip_surface *s = uip_surface(&S, uip_slot_of(h));
        CHECK("the model records the owner instance and the title; the surface is unplaced", s && s->owner_pid == 1 && s->owner_gen == 100 && s->owner_principal == 1001 && !strcmp(s->title, "Demo") && !s->placed);
        CHECK("a new surface is on top of the z-order and nothing has focus", S.nz == 1 && S.focus == -1);
        CHECK("text and rect are staged", do_text(&A, h, 0, "Hello") == UIP_OK && do_rect(&A, h, 0, 10, 10, 100, 20) == UIP_OK);
        s = uip_surface(&S, uip_slot_of(h));
        CHECK("nothing is visible before COMMIT, although the staged copy has the content", !s->vis[0].has_text && !s->vis[0].has_rect && s->pend[0].has_text && s->pend[0].has_rect && !strcmp(s->pend[0].text, "Hello"));
        CHECK("COMMIT succeeds", do_simple(&A, UIP_COMMIT, h) == UIP_OK);
        s = uip_surface(&S, uip_slot_of(h));
        CHECK("after COMMIT the visible model equals the staged one", s->vis[0].has_text && s->vis[0].has_rect && !strcmp(s->vis[0].text, "Hello") && s->vis[0].x == 10 && s->vis[0].w == 100 && s->commits == 1);
        do_text(&A, h, 0, "Changed");
        s = uip_surface(&S, uip_slot_of(h));
        CHECK("a later SET_TEXT does not alter the visible model until the next COMMIT", !strcmp(s->vis[0].text, "Hello") && !strcmp(s->pend[0].text, "Changed"));
        CHECK("an empty text clears the staged text", do_text(&A, h, 0, "") == UIP_OK && !uip_surface(&S, uip_slot_of(h))->pend[0].has_text);
        CHECK("rects that exactly fit the size class (medium 480x320) are accepted, one pixel more is rejected", do_rect(&A, h, 1, 0, 0, 480, 320) == UIP_OK && do_rect(&A, h, 1, 0, 0, 481, 320) == UIP_EBADARG && do_rect(&A, h, 1, 0, 0, 480, 321) == UIP_EBADARG && do_rect(&A, h, 1, 1, 0, 480, 320) == UIP_EBADARG);
        CHECK("negative coordinates are rejected", do_rect(&A, h, 2, -1, 0, 10, 10) == UIP_EBADARG && do_rect(&A, h, 2, 0, -1, 10, 10) == UIP_EBADARG);
        CHECK("coordinates that overflow 16-bit arithmetic are rejected (x 32767 + w 32768)", do_rect(&A, h, 2, 32767, 0, 32768, 10) == UIP_EBADARG && do_rect(&A, h, 2, 0, 32767, 10, 65535) == UIP_EBADARG);
        CHECK("a failed SET_RECT did not stage anything", !uip_surface(&S, uip_slot_of(h))->pend[2].has_rect);
        CHECK("invariants hold", uip_check_invariants(&S));
    }

    /* ---- an invalid message changes nothing (state digest) ---- */
    init();
    {
        uint32_t h = create(&A, 0, "T"); do_text(&A, h, 0, "x"); do_rect(&A, h, 0, 1, 1, 10, 10); do_simple(&A, UIP_COMMIT, h);
        uint32_t hb = create(&B, 0, "U");
        uint32_t d0 = uip_digest(&S);
        uint8_t b[80]; uint32_t n; int ok = 1;
        n = uip_encode_set_text(b, sizeof b, 1, h, 0, 0, "bad"); b[11] = 1; ok &= send(&A, b, n) == UIP_EBADTEXT; uip_reply_consumed(&S, 1, 100);
        n = uip_encode_set_rect(b, sizeof b, 2, h, 0, 0, 0, 0, 0, 5); ok &= send(&A, b, n) == UIP_EBADARG; uip_reply_consumed(&S, 1, 100);
        n = uip_encode_set_rect(b, sizeof b, 3, h, 0, 0, 250, 0, 50, 5); ok &= send(&A, b, n) == UIP_EBADARG; uip_reply_consumed(&S, 1, 100);
        n = uip_encode_set_text(b, sizeof b, 4, hb, 0, 0, "mine now"); ok &= send(&A, b, n) == UIP_ESTALE; uip_reply_consumed(&S, 1, 100);
        n = uip_encode_simple(b, sizeof b, UIP_COMMIT, 5, 0xdead01); ok &= send(&A, b, n) == UIP_ESTALE; uip_reply_consumed(&S, 1, 100);
        b[0] = 9; ok &= send(&A, b, n) == UIP_EVERSION; uip_reply_consumed(&S, 1, 100);
        n = uip_encode_set_text(b, sizeof b, 6, h, 99, 0, "x"); ok &= send(&A, b, n) == UIP_EBADARG; uip_reply_consumed(&S, 1, 100);
        CHECK("seven different invalid messages produced the expected errors", ok);
        CHECK("and the whole model (visible and staged content, z-order, focus, owners) is bit-for-bit unchanged", uip_digest(&S) == d0);
    }

    /* ---- ownership, principals, generation and pid reuse ---- */
    init();
    {
        uint32_t ha = create(&A, 0, "A"); uint32_t hb = create(&B, 0, "B");
        CHECK("A cannot update, commit, destroy or focus B's surface: the answer is ESTALE each time",
              do_text(&A, hb, 0, "x") == UIP_ESTALE && do_rect(&A, hb, 0, 0, 0, 5, 5) == UIP_ESTALE && do_simple(&A, UIP_COMMIT, hb) == UIP_ESTALE && do_simple(&A, UIP_DESTROY_SURFACE, hb) == UIP_ESTALE && do_simple(&A, UIP_REQUEST_FOCUS, hb) == UIP_ESTALE);
        CHECK("B's surface survived, untouched", uip_surface(&S, uip_slot_of(hb)) && !uip_surface(&S, uip_slot_of(hb))->pend[0].has_text);
        CHECK("a live foreign handle and a never-existing handle give the SAME answer (no existence oracle)", do_text(&A, hb, 0, "x") == do_text(&A, 0x7700 | 5, 0, "x"));
        struct uip_caller forged = { 1, 100, 9999, 1 };           /* same pid and generation, different principal */
        CHECK("same pid and generation but a different principal is another identity: ESTALE", do_text(&forged, ha, 0, "x") == UIP_ESTALE);
        CHECK("a principal change on a live instance is treated as a NEW instance: the old surfaces are dropped, never inherited", uip_surface(&S, uip_slot_of(ha)) == 0 && uip_check_invariants(&S));
        init();
        ha = create(&A, 0, "A");
        struct uip_caller A2 = { 1, 101, 1001, 1 };               /* the pid is reused: a new generation */
        CHECK("a reused pid (new generation) cannot touch the previous instance's surface", do_text(&A2, ha, 0, "x") == UIP_ESTALE);
        CHECK("and the previous instance's surfaces were dropped when the new instance first spoke", uip_surface(&S, uip_slot_of(ha)) == 0 && S.nz == 0 && uip_check_invariants(&S));
        init(); ha = create(&A, 0, "A"); do_simple(&A, UIP_REQUEST_FOCUS, ha); uip_set_focus(&S, uip_slot_of(ha));
        uip_client_gone(&S, 1, 999);
        CHECK("a stale exit notification (wrong generation) does not touch the live instance", uip_surface(&S, uip_slot_of(ha)) != 0 && S.focus == uip_slot_of(ha));
        uip_client_gone(&S, 1, 100);
        CHECK("the real exit notification destroys the surfaces, clears focus and the z-order", uip_surface(&S, uip_slot_of(ha)) == 0 && S.focus == -1 && S.nz == 0 && uip_check_invariants(&S));
        CHECK("after the exit the old handle is dead for everyone, even the same pid and generation again", do_text(&A, ha, 0, "x") == UIP_ESTALE);
        init(); ha = create(&A, 0, "A");
        uip_client_gone(&S, 1, 100);
        uint32_t ha2 = create(&A, 0, "A again");
        CHECK("a recreated surface in the freed slot gets a DIFFERENT handle (serial changed), and the old handle stays dead", ha2 != ha && uip_slot_of(ha2) == uip_slot_of(ha) && do_text(&A, ha, 0, "x") == UIP_ESTALE && do_text(&A, ha2, 0, "x") == UIP_OK);
    }

    /* ---- gaps found by mutation checking ---- */
    init();
    {
        uint32_t ha = create(&A, 0, "A");
        struct uip_caller P2 = { 2, 100, 1001, 1 };                 /* same generation and principal as A, but another pid */
        CHECK("only the pid differs: another process still cannot reach the surface", do_text(&P2, ha, 0, "x") == UIP_ESTALE && do_simple(&P2, UIP_COMMIT, ha) == UIP_ESTALE && do_simple(&P2, UIP_DESTROY_SURFACE, ha) == UIP_ESTALE && uip_surface(&S, uip_slot_of(ha)) != 0);
        CHECK("a handle with stray high bits (bit 24 or bit 31) is not the same handle", do_text(&A, ha | 0x01000000u, 0, "x") == UIP_ESTALE && do_text(&A, ha | 0x80000000u, 0, "x") == UIP_ESTALE && do_text(&A, ha, 0, "x") == UIP_OK);
        CHECK("rectangles whose sum wraps a 16-bit integer are rejected (y 100 + h 65535, x 100 + w 65535)", do_rect(&A, ha, 3, 0, 100, 10, 65535) == UIP_EBADARG && do_rect(&A, ha, 3, 100, 0, 65535, 10) == UIP_EBADARG && do_rect(&A, ha, 3, 65535 - 100, 100, 200, 10) == UIP_EBADARG);
        int zero_serial = 0, dead_ok = 1, foreign_ok = 1; uint32_t first = ha;
        do_simple(&A, UIP_DESTROY_SURFACE, ha);
        for (int i = 0; i < 70000; i++) {                           /* the 16-bit serial wraps after 65535 reuses of one slot: it must skip 0 */
            uint32_t h = create(&A, 0, "w");
            if (!h || (h >> 8) == 0) zero_serial = 1;
            if (i < 60000 && do_text(&A, first, 0, "x") != UIP_ESTALE) dead_ok = 0;             /* a dead handle stays dead for 60000 reuses of its slot */
            if (do_text(&B, first, 0, "x") != UIP_ESTALE || do_text(&B, h, 0, "x") != UIP_ESTALE) foreign_ok = 0;   /* and no other instance can use either handle, wrapped or not */
            do_simple(&A, UIP_DESTROY_SURFACE, h);
        }
        CHECK("70000 create/destroy cycles: no handle ever has serial 0, a dead handle stays dead for 60000 reuses, and another client never reaches a surface even after the serial wraps", !zero_serial && dead_ok && foreign_ok && uip_check_invariants(&S));
    }

    /* ---- capability and identity checks ---- */
    init();
    {
        struct uip_caller none = { 1, 100, 1001, 0 };
        uint8_t b[64]; uint32_t n = uip_encode_create(b, sizeof b, 1, 0, "x");
        CHECK("without the UI capability CREATE is EPERM and still gets a reply", send(&none, b, n) == UIP_EPERM && rlen == 11 && S.nz == 0);
        struct uip_caller bad0 = { 0, 1, 1, 1 }, bad9 = { 9, 1, 1, 1 };
        CHECK("pid 0 and pid 9 are not processes: EPERM and NO reply", send(&bad0, b, n) == UIP_EPERM && rlen == 0 && send(&bad9, b, n) == UIP_EPERM && rlen == 0);
        CHECK("a NULL state or caller is refused", uip_handle(0, &A, b, n, rep, 64, &rlen) != UIP_OK && uip_handle(&S, 0, b, n, rep, 64, &rlen) != UIP_OK);
        CHECK("no surface was created by any of that", S.nz == 0 && uip_check_invariants(&S));
    }

    /* ---- quotas, duplicates, double destroy ---- */
    init();
    {
        uint32_t h[UIP_PER_CLIENT + 1]; int ok = 1;
        for (int i = 0; i < UIP_PER_CLIENT; i++) { h[i] = create(&A, 0, "s"); ok &= h[i] != 0; }
        CHECK("a client can create up to UIP_PER_CLIENT surfaces", ok);
        CHECK("one more is EQUOTA and creates nothing", ({ uint8_t b[64]; uint32_t n = uip_encode_create(b, 64, ++seqn, 0, "x"); int rc = send(&A, b, n); uip_reply_consumed(&S, 1, 100); rc; }) == UIP_EQUOTA && S.nz == UIP_PER_CLIENT);
        uint32_t hb[UIP_PER_CLIENT]; int cnt = 0;
        for (int i = 0; i < UIP_PER_CLIENT; i++) { hb[i] = create(&B, 0, "b"); cnt += hb[i] != 0; }
        CHECK("the table holds UIP_MAX_SURFACES in total: B fills the rest", cnt == UIP_MAX_SURFACES - UIP_PER_CLIENT && S.nz == UIP_MAX_SURFACES);
        CHECK("a third client finds the table full: EQUOTA", create(&C3, 0, "c") == 0 && ({ uint8_t b[64]; uint32_t n = uip_encode_create(b, 64, ++seqn, 0, "x"); int rc = send(&C3, b, n); uip_reply_consumed(&S, 3, 300); rc; }) == UIP_EQUOTA);
        CHECK("destroying a surface frees quota", do_simple(&A, UIP_DESTROY_SURFACE, h[0]) == UIP_OK && create(&C3, 0, "c") != 0);
        CHECK("destroying twice: the second is ESTALE", do_simple(&A, UIP_DESTROY_SURFACE, h[0]) == UIP_ESTALE);
        CHECK("every update after destroy is ESTALE", do_text(&A, h[0], 0, "x") == UIP_ESTALE && do_rect(&A, h[0], 0, 0, 0, 5, 5) == UIP_ESTALE && do_simple(&A, UIP_COMMIT, h[0]) == UIP_ESTALE && do_simple(&A, UIP_REQUEST_FOCUS, h[0]) == UIP_ESTALE);
        CHECK("invariants hold", uip_check_invariants(&S));
        init();
        uint8_t b[64]; uint32_t n = uip_encode_create(b, 64, 77, 0, "dup");
        int r1 = send(&A, b, n); uip_reply_consumed(&S, 1, 100); int r2 = send(&A, b, n); uip_reply_consumed(&S, 1, 100);
        CHECK("a repeated CREATE with the same sequence number creates exactly one surface (second: EDUP)", r1 == UIP_OK && r2 == UIP_EDUP && S.nz == 1);
        n = uip_encode_create(b, 64, 78, 0, "dup"); CHECK("a new sequence number creates another", send(&A, b, n) == UIP_OK && S.nz == 2);
        uip_reply_consumed(&S, 1, 100);
    }

    /* ---- outstanding replies ---- */
    init();
    {
        uint8_t b[64]; uint32_t n = uip_encode_create(b, 64, 1, 0, "x");
        int r1 = send(&A, b, n); n = uip_encode_create(b, 64, 2, 0, "y"); int r2 = send(&A, b, n);
        uint32_t d = uip_digest(&S);
        n = uip_encode_create(b, 64, 3, 0, "z"); int r3 = send(&A, b, n);
        CHECK("two unconsumed replies are allowed, the third request is EBUSY with no reply and no change", r1 == UIP_OK && r2 == UIP_OK && r3 == UIP_EBUSY && rlen == 0 && uip_digest(&S) == d);
        CHECK("a different client is not affected by A's backlog", create(&B, 0, "b") != 0);
        uip_reply_consumed(&S, 1, 999);
        CHECK("a consume for a stale instance is ignored", ({ n = uip_encode_create(b, 64, 4, 0, "w"); send(&A, b, n); }) == UIP_EBUSY);
        uip_reply_consumed(&S, 1, 100);
        n = uip_encode_create(b, 64, 5, 0, "w");
        CHECK("after the client consumes a reply it may send again", send(&A, b, n) == UIP_OK && uip_check_invariants(&S));
        uip_reply_consumed(&S, 1, 100); uip_reply_consumed(&S, 1, 100); uip_reply_consumed(&S, 1, 100); uip_reply_consumed(&S, 1, 100);
        CHECK("extra consumes never push the counter below zero", S.c[0].outstanding == 0);
    }

    /* ---- focus, placement and event routing ---- */
    init();
    {
        uint32_t ha = create(&A, 0, "A"), hb = create(&B, 0, "B");
        int sa = uip_slot_of(ha), sb = uip_slot_of(hb);
        CHECK("REQUEST_FOCUS is advisory: it answers OK and moves nothing", do_simple(&A, UIP_REQUEST_FOCUS, ha) == UIP_OK && S.focus == -1 && uip_surface(&S, sa)->focus_wished);
        uint8_t ev[64]; struct uip_target to;
        CHECK("without focus a key event goes nowhere", uip_route_key(&S, 30, 0, 1, 0, &to, ev, sizeof ev) == 0);
        CHECK("only the compositor sets focus", uip_set_focus(&S, sa) == UIP_OK && S.focus == sa && !uip_surface(&S, sa)->focus_wished);
        uint32_t n = uip_route_key(&S, 30, 2, 1, 0, &to, ev, sizeof ev);
        CHECK("a key event reaches the owner of the focused surface only (A, generation 100) and is a well-formed EVENT_KEY for A's handle",
              n == 14 && to.pid == 1 && to.gen == 100 && ev[1] == UIP_EVENT_KEY && (ev[4] | (ev[5] << 8)) == (int)(ha & 0xffff) && ev[8] == 30 && ev[10] == 2 && ev[12] == 1);
        uip_set_focus(&S, sb);
        n = uip_route_key(&S, 31, 0, 1, 0, &to, ev, sizeof ev);
        CHECK("when focus moves, B receives the keys and A none", n == 14 && to.pid == 2 && to.gen == 200);
        CHECK("an unknown slot cannot take focus", uip_set_focus(&S, 7) == UIP_ESTALE && uip_set_focus(&S, -2) == UIP_ESTALE && uip_set_focus(&S, 99) == UIP_ESTALE && S.focus == sb);
        CHECK("focusing raises the surface to the top of the z-order", S.z[S.nz - 1] == sb);
        CHECK("an unplaced surface receives no pointer events", uip_route_pointer(&S, 100, 100, 1, 1, &to, ev, sizeof ev) == 0);
        CHECK("placement outside the content region is refused (left, top, right, bottom, overflow)", uip_place(&S, sa, 39, 96) == UIP_EBADARG && uip_place(&S, sa, 40, 95) == UIP_EBADARG && uip_place(&S, sa, 40 + 944 - 255, 96) == UIP_EBADARG && uip_place(&S, sa, 40, 96 + 520 - 159) == UIP_EBADARG && uip_place(&S, sa, 32767, 32767) == UIP_EBADARG);
        CHECK("placement inside the region, flush with its edges, is accepted", uip_place(&S, sa, 40, 96) == UIP_OK && uip_place(&S, sb, 40 + 944 - 256, 96 + 520 - 160) == UIP_OK);
        n = uip_route_pointer(&S, 50, 106, 1, 1, &to, ev, sizeof ev);
        CHECK("a pointer event inside A's surface goes to A in surface-local coordinates (10, 10)", n == 14 && to.pid == 1 && ev[8] == 1 && (int16_t)(ev[10] | (ev[11] << 8)) == 10 && (int16_t)(ev[12] | (ev[13] << 8)) == 10);
        CHECK("a pointer event outside every surface goes nowhere", uip_route_pointer(&S, 20, 20, 1, 1, &to, ev, sizeof ev) == 0 && uip_route_pointer(&S, 40 + 256, 96, 1, 1, &to, ev, sizeof ev) == 0);
        uip_place(&S, sb, 40, 96);                                       /* now B overlaps A exactly and is on top */
        n = uip_route_pointer(&S, 60, 110, 1, 1, &to, ev, sizeof ev);
        CHECK("where surfaces overlap the topmost one gets the event, the other none", n == 14 && to.pid == 2);
        CHECK("a close request goes to the focused surface's owner only", uip_route_close(&S, &to, ev, sizeof ev) == 8 && to.pid == 2 && ev[1] == UIP_CLOSE_REQUEST);
        do_simple(&B, UIP_DESTROY_SURFACE, hb);
        CHECK("destroying the focused surface clears focus, so no key reaches anyone", S.focus == -1 && uip_route_key(&S, 30, 0, 1, 0, &to, ev, sizeof ev) == 0 && uip_route_close(&S, &to, ev, sizeof ev) == 0);
        CHECK("a placement of a dead slot is ESTALE", uip_place(&S, sb, 40, 96) == UIP_ESTALE);
        CHECK("an event buffer that is too small yields no event", ({ uip_set_focus(&S, sa); uip_route_key(&S, 1, 0, 1, 0, &to, ev, 5); }) == 0);
        CHECK("invariants hold", uip_check_invariants(&S));
    }

    /* ---- hostile bytes: never crash, never corrupt, never change state on failure ---- */
    {
        init();
        uint32_t h = create(&A, 1, "fz"); do_text(&A, h, 0, "seed"); do_rect(&A, h, 0, 5, 5, 50, 20); do_simple(&A, UIP_COMMIT, h);
        create(&B, 0, "other");
        unsigned seed = 4242u; int inv = 1, unchanged = 1;
        for (int it = 0; it < 200000; it++) {
            uint8_t b[96]; uint32_t n = 0;
            seed = seed * 1103515245u + 12345u;
            uint32_t mode = (seed >> 16) % 4;
            n = (seed >> 8) % 70;
            for (uint32_t i = 0; i < n; i++) { seed = seed * 1103515245u + 12345u; b[i] = (uint8_t)(seed >> 16); }
            if (mode == 0 && n >= 8) { b[0] = 1; b[1] = (uint8_t)(1 + (seed >> 5) % 6); }              /* a plausible header */
            if (mode == 1 && n >= 8) { b[0] = 1; b[1] = (uint8_t)(1 + (seed >> 5) % 6); b[4] = (uint8_t)h; b[5] = (uint8_t)(h >> 8); b[6] = 0; b[7] = 0; }          /* aim at a real handle */
            const struct uip_caller *w = (seed >> 9) & 1 ? &A : &B;
            uint32_t d0 = uip_digest(&S);
            uint8_t r2[64]; uint32_t rl = 0;
            int rc = uip_handle(&S, w, b, n, r2, sizeof r2, &rl);
            uip_reply_consumed(&S, w->pid, w->gen);
            if (rc != UIP_OK && uip_digest(&S) != d0) unchanged = 0;
            if (!uip_check_invariants(&S)) { inv = 0; break; }
        }
        CHECK("200000 hostile or random byte strings: no crash, the internal tables stay consistent", inv);
        CHECK("and every rejected message left the whole model unchanged", unchanged);
    }

    /* ---- a randomized run against an independent oracle ---- */
    {
        init();
        struct mw { int has_text, has_rect; int x, y, w, h; char text[41]; };
        struct ms { int live; uint32_t handle; int pid; uint64_t gen; int sc; struct mw pend[16], vis[16]; };
        static struct ms model[200]; int nm = 0;
        static uint64_t gens[5] = { 0, 100, 200, 300, 400 };
        int out[5] = { 0 }, ownd[5] = { 0 }, stale[5] = { 0 }; int have_seq[5] = { 0 }; uint16_t last_seq[5] = { 0 };
        unsigned seed = 99u; int mismatches = 0, inv = 1, orphan = 0;
        for (int it = 0; it < 120000; it++) {
            seed = seed * 1103515245u + 12345u;
            int pid = 1 + (int)((seed >> 16) % 4);
            struct uip_caller w = { (uint8_t)pid, gens[pid], 1000u + (uint32_t)pid, 1 };
            uint32_t op = (seed >> 8) % 14;
            if (op == 12) {                                                 /* the instance exits; sometimes it is announced, sometimes the next message reveals it */
                int announce = (seed >> 20) & 1;
                if (announce) { uip_client_gone(&S, (uint8_t)pid, gens[pid]); for (int i = 0; i < nm; i++) if (model[i].live && model[i].pid == pid && model[i].gen == gens[pid]) model[i].live = 0; ownd[pid] = 0; out[pid] = 0; have_seq[pid] = 0; }
                gens[pid] += 1000;                                          /* a new instance owns the pid from now on */
                if (!announce) { stale[pid] = 1; ownd[pid] = 0; out[pid] = 0; have_seq[pid] = 0; }   /* not announced: the old surfaces stay (and count against the quotas) until the new instance first speaks */
                continue;
            }
            if (op == 13) { if (out[pid] > 0) { uip_reply_consumed(&S, (uint8_t)pid, gens[pid]); out[pid]--; } continue; }
            w.gen = gens[pid];
            if (stale[pid]) { for (int i = 0; i < nm; i++) if (model[i].live && model[i].pid == pid && model[i].gen != gens[pid]) model[i].live = 0; stale[pid] = 0; }   /* the new instance speaks: the model drops the old one's surfaces now */
            /* pick a handle: one of the model's (any owner, live or dead) or a bogus one */
            uint32_t h = 0xbad00u | (uint32_t)((seed >> 12) & 7);
            int mi = -1;
            if (nm && ((seed >> 4) & 3)) { mi = (int)((seed >> 11) % (unsigned)nm); h = model[mi].handle; }
            uint8_t b[64]; uint32_t n = 0; int expect = 0; uint32_t d0 = uip_digest(&S);
            int expect_create = 0; int sw = 0, sh = 0;
            /* the oracle's view of the caller's instance: a new generation means the previous surfaces were dropped */
            int is_new = (!ownd[pid]);                                      /* first message of this instance: registers it and drops the previous instance's surfaces */
            struct ms *m = (mi >= 0 && model[mi].live && model[mi].pid == pid && model[mi].gen == gens[pid]) ? &model[mi] : 0;
            if (op <= 1) {                                                  /* CREATE */
                uint16_t seq = (uint16_t)((seed >> 7) & 7);
                n = uip_encode_create(b, sizeof b, seq, (uint8_t)((seed >> 3) % 3), "r");
                expect_create = 1;
                int live_all = 0; for (int i = 0; i < nm; i++) if (model[i].live) live_all++;
                int mine = 0; for (int i = 0; i < nm; i++) if (model[i].live && model[i].pid == pid) mine++;
                if (out[pid] >= UIP_MAX_OUTSTANDING) expect = UIP_EBUSY;
                else if (have_seq[pid] && last_seq[pid] == seq) expect = UIP_EDUP;
                else if (mine >= UIP_PER_CLIENT || live_all >= UIP_MAX_SURFACES) expect = UIP_EQUOTA;
                if (out[pid] < UIP_MAX_OUTSTANDING) { have_seq[pid] = 1; last_seq[pid] = seq; }     /* every CREATE that reaches the decoder records its sequence */
            } else if (op <= 4) {                                           /* SET_TEXT */
                char t[8]; t[0] = (char)('a' + (seed >> 3) % 26); t[1] = (seed >> 9) & 1 ? 'z' : 0; t[2] = 0;
                int wi = (int)((seed >> 5) % 16);
                n = uip_encode_set_text(b, sizeof b, 1, h, (uint8_t)wi, 1, t);
                if (out[pid] >= UIP_MAX_OUTSTANDING) expect = UIP_EBUSY; else expect = m ? UIP_OK : UIP_ESTALE;
                if (expect == UIP_OK) { m->pend[wi].has_text = 1; strcpy(m->pend[wi].text, t); }
            } else if (op <= 7) {                                           /* SET_RECT */
                int wi = (int)((seed >> 5) % 16);
                int x = (int)((seed >> 10) % 600) - 20, y = (int)((seed >> 13) % 400) - 20, ww = (int)((seed >> 17) % 300), hh = (int)((seed >> 21) % 200);
                n = uip_encode_set_rect(b, sizeof b, 1, h, (uint8_t)wi, 0, (int16_t)x, (int16_t)y, (uint16_t)ww, (uint16_t)hh);
                if (out[pid] >= UIP_MAX_OUTSTANDING) expect = UIP_EBUSY;
                else if (ww == 0 || hh == 0) expect = UIP_EBADARG;              /* the decoder rejects first, even for a dead handle */
                else if (!m) expect = UIP_ESTALE;
                else {
                    static const int W3[3] = { 256, 480, 720 }, H3[3] = { 160, 320, 420 };
                    sw = W3[m->sc]; sh = H3[m->sc];
                    expect = (x < 0 || y < 0 || x + ww > sw || y + hh > sh) ? UIP_EBADARG : UIP_OK;
                    if (expect == UIP_OK) { m->pend[wi].has_rect = 1; m->pend[wi].x = x; m->pend[wi].y = y; m->pend[wi].w = ww; m->pend[wi].h = hh; }
                }
            } else if (op <= 9) {                                           /* COMMIT */
                n = uip_encode_simple(b, sizeof b, UIP_COMMIT, 1, h);
                if (out[pid] >= UIP_MAX_OUTSTANDING) expect = UIP_EBUSY; else expect = m ? UIP_OK : UIP_ESTALE;
                if (expect == UIP_OK) memcpy(m->vis, m->pend, sizeof m->vis);
            } else if (op == 10) {                                          /* DESTROY */
                n = uip_encode_simple(b, sizeof b, UIP_DESTROY_SURFACE, 1, h);
                if (out[pid] >= UIP_MAX_OUTSTANDING) expect = UIP_EBUSY; else expect = m ? UIP_OK : UIP_ESTALE;
                if (expect == UIP_OK) m->live = 0;
            } else {                                                        /* REQUEST_FOCUS */
                n = uip_encode_simple(b, sizeof b, UIP_REQUEST_FOCUS, 1, h);
                if (out[pid] >= UIP_MAX_OUTSTANDING) expect = UIP_EBUSY; else expect = m ? UIP_OK : UIP_ESTALE;
            }
            /* a first message of a new instance drops the previous instance's surfaces (already applied to the model above) */
            rlen = 0;
            int rc = uip_handle(&S, &w, b, n, rep, sizeof rep, &rlen);
            ownd[pid] = 1;
            if (rc != UIP_EBUSY) out[pid]++;
            if (rc != expect) { if (mismatches++ < 5) printf("    mismatch at %d: op %u pid %d got %d want %d\n", it, op, pid, rc, expect); }
            if (expect_create && rc == UIP_OK && nm < 200) {
                struct ms *e = &model[nm++]; memset(e, 0, sizeof *e);
                e->live = 1; e->handle = reply_handle(); e->pid = pid; e->gen = gens[pid]; e->sc = b[8];
            }
            if (nm >= 190) { nm = 0; memset(model, 0, sizeof model); init(); memset(out, 0, sizeof out); memset(ownd, 0, sizeof ownd); memset(have_seq, 0, sizeof have_seq); }   /* start over before the model array fills */
            if (rc != UIP_OK && !is_new && uip_digest(&S) != d0) { mismatches++; printf("    state changed by a failing op %u (rc %d)\n", op, rc); }
            if (rc == UIP_EBUSY && rlen != 0) mismatches++;
            if (!uip_check_invariants(&S)) { inv = 0; break; }
            /* every live model surface matches the real visible model; no real live surface is unknown to the model */
            if ((it % 64) == 0) {
                int live_real = 0;
                for (int s = 0; s < UIP_MAX_SURFACES; s++) if (S.s[s].live) live_real++;
                int live_model = 0;
                for (int i = 0; i < nm; i++) {
                    if (!model[i].live) continue;
                    live_model++;
                    const struct uip_surface *rs = uip_surface(&S, uip_slot_of(model[i].handle));
                    if (!rs || rs->owner_pid != model[i].pid || rs->owner_gen != model[i].gen) { orphan++; continue; }
                    for (int k = 0; k < 16; k++) {
                        if (rs->vis[k].has_text != model[i].vis[k].has_text || rs->vis[k].has_rect != model[i].vis[k].has_rect) { mismatches++; break; }
                        if (rs->vis[k].has_text && strcmp(rs->vis[k].text, model[i].vis[k].text)) { mismatches++; break; }
                        if (rs->vis[k].has_rect && (rs->vis[k].x != model[i].vis[k].x || rs->vis[k].w != model[i].vis[k].w || rs->vis[k].h != model[i].vis[k].h || rs->vis[k].y != model[i].vis[k].y)) { mismatches++; break; }
                    }
                }
                if (live_real != live_model) { mismatches++; if (mismatches < 6) printf("    live count differs: real %d model %d\n", live_real, live_model); }
            }
        }
        CHECK("120000 random operations from four clients with exits and pid reuse: every status code equals the oracle's", mismatches == 0);
        CHECK("every visible model equals the oracle's, and no live surface is unknown to the oracle", orphan == 0);
        CHECK("the internal tables stayed consistent throughout", inv);
    }

    printf("ui_proto: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
