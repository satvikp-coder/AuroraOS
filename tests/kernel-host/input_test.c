/*
 * tests/kernel-host/input_test.c - host tests for kernel/input/input_core.c (modifiers, US text, pointer scaling, queue).
 * Independent oracle for scaling: exact rational arithmetic on 128-bit integers; the keymap is checked against an explicit
 * hand-written table of Linux key codes (not the one in the source).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <input.h>

static int passed, failed;
#define CHECK(name, cond) do { if (cond) { passed++; printf("  PASS: %s\n", name); } else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)

static struct input_state S;
static void fresh(void) { input_init(&S, 1024, 768); input_set_axis(&S, 0, 0, 32767); input_set_axis(&S, 1, 0, 32767); }
static void key(uint16_t k, int v) { input_feed(&S, EV_KEY, k, v); }
static void syn(void) { input_feed(&S, EV_SYN, SYN_REPORT, 0); }
static struct input_event pop1(void) { struct input_event e; memset(&e, 0xEE, sizeof e); if (!input_pop(&S, &e)) e.kind = IN_NONE; return e; }

int main(void)
{
    printf("input_core host tests\n");

    /* --- keymap against an independent table: (code, normal, shifted) from the Linux input-event-codes list --- */
    static const struct { uint16_t c; char n, s; } K[] = {
        {2,'1','!'},{3,'2','@'},{4,'3','#'},{5,'4','$'},{6,'5','%'},{7,'6','^'},{8,'7','&'},{9,'8','*'},{10,'9','('},{11,'0',')'},
        {12,'-','_'},{13,'=','+'},{16,'q','Q'},{17,'w','W'},{18,'e','E'},{19,'r','R'},{20,'t','T'},{21,'y','Y'},{22,'u','U'},
        {23,'i','I'},{24,'o','O'},{25,'p','P'},{26,'[','{'},{27,']','}'},{30,'a','A'},{31,'s','S'},{32,'d','D'},{33,'f','F'},
        {34,'g','G'},{35,'h','H'},{36,'j','J'},{37,'k','K'},{38,'l','L'},{39,';',':'},{40,'\'','"'},{41,'`','~'},{43,'\\','|'},
        {44,'z','Z'},{45,'x','X'},{46,'c','C'},{47,'v','V'},{48,'b','B'},{49,'n','N'},{50,'m','M'},{51,',','<'},{52,'.','>'},{53,'/','?'},
        {57,' ',' '} };
    int km = 1;
    for (unsigned i = 0; i < sizeof K / sizeof K[0]; i++) if (input_us_char(K[i].c, 0) != K[i].n || input_us_char(K[i].c, 1) != K[i].s) { km = 0; printf("    key %u mismatch\n", K[i].c); }
    CHECK("US keymap: 49 printable keys, normal and shifted, match the independent table", km);
    int nontext = 1;
    for (uint16_t c = 0; c < 70; c++) {
        int listed = 0;
        for (unsigned i = 0; i < sizeof K / sizeof K[0]; i++) if (K[i].c == c) listed = 1;
        if (!listed && (input_us_char(c, 0) || input_us_char(c, 1))) nontext = 0;
    }
    CHECK("every key not in the table (Esc, Tab, Backspace, Enter, modifiers, F-keys, ...) produces no text", nontext);
    CHECK("keys above the table (arrows, Home, End, Delete, 0xffff) produce no text", !input_us_char(KEY_UP, 0) && !input_us_char(KEY_HOME, 1) && !input_us_char(0xffff, 0));

    /* --- key down / up and text --- */
    fresh(); key(30, 1);
    struct input_event e = pop1(); struct input_event t = pop1();
    CHECK("press A: KEY_DOWN code 30, no modifiers, not a repeat", e.kind == IN_KEY_DOWN && e.key == 30 && e.mods == 0 && e.repeat == 0);
    CHECK("press A: followed by TEXT 'a'", t.kind == IN_TEXT && t.ch == 'a' && t.key == 30);
    CHECK("A is held", input_key_held(&S, 30));
    key(30, 0); e = pop1();
    CHECK("release A: KEY_UP", e.kind == IN_KEY_UP && e.key == 30 && !input_key_held(&S, 30));
    CHECK("queue empty afterwards", pop1().kind == IN_NONE);

    /* --- modifiers --- */
    fresh(); key(KEY_LEFTSHIFT, 1); (void)pop1(); key(30, 1); (void)pop1(); t = pop1();
    CHECK("Shift+A: TEXT 'A' with MOD_SHIFT", t.kind == IN_TEXT && t.ch == 'A' && (t.mods & MOD_SHIFT));
    key(KEY_LEFTSHIFT, 0); key(30, 0);
    CHECK("releasing the shift key clears MOD_SHIFT", !(S.mods & MOD_SHIFT));
    fresh(); key(KEY_RIGHTSHIFT, 1); key(KEY_LEFTSHIFT, 1); key(KEY_LEFTSHIFT, 0);
    CHECK("two shift keys: still shifted while the right one is held", S.mods == MOD_SHIFT);
    key(KEY_RIGHTSHIFT, 0);
    CHECK("both shift keys released: no modifiers", S.mods == 0);
    fresh(); key(KEY_LEFTCTRL, 1); key(KEY_RIGHTALT, 1);
    CHECK("Ctrl and Alt are tracked together", S.mods == (MOD_CTRL | MOD_ALT));
    while (pop1().kind != IN_NONE) { }
    key(30, 1); e = pop1();
    CHECK("Ctrl+Alt+A: KEY_DOWN carries both modifiers", e.kind == IN_KEY_DOWN && e.mods == (MOD_CTRL | MOD_ALT));
    CHECK("Ctrl+Alt+A: NO text event is produced", pop1().kind == IN_NONE);
    fresh(); key(KEY_LEFTCTRL, 1); while (pop1().kind != IN_NONE) { } key(30, 1); (void)pop1();
    CHECK("Ctrl+A alone: no text event", pop1().kind == IN_NONE);
    fresh(); key(KEY_LEFTALT, 1); while (pop1().kind != IN_NONE) { } key(30, 1); (void)pop1();
    CHECK("Alt+A alone: no text event", pop1().kind == IN_NONE);
    fresh(); key(KEY_LEFTCTRL, 1); key(KEY_LEFTCTRL, 1); key(KEY_LEFTCTRL, 0);
    CHECK("pressing a modifier twice then releasing it once leaves nothing stuck", S.mods == 0);
    fresh(); key(KEY_LEFTSHIFT, 1); key(KEY_LEFTALT, 1); input_feed(&S, EV_SYN, SYN_DROPPED, 0);
    CHECK("SYN_DROPPED (lost events) releases every held key and modifier", S.mods == 0 && !input_key_held(&S, KEY_LEFTSHIFT));
    fresh(); key(KEY_LEFTCTRL, 1); input_reset(&S);
    CHECK("input_reset (device reset) clears held modifiers", S.mods == 0 && S.st.resets == 1);

    /* --- repeat, strays --- */
    fresh(); key(30, 1); (void)pop1(); (void)pop1(); key(30, 2); e = pop1(); t = pop1();
    CHECK("auto-repeat of a held key: KEY_DOWN with repeat=1 and TEXT", e.kind == IN_KEY_DOWN && e.repeat == 1 && t.kind == IN_TEXT && t.repeat == 1);
    fresh(); key(31, 2);
    CHECK("repeat for a key never seen down is ignored", pop1().kind == IN_NONE);
    fresh(); key(31, 0);
    CHECK("release of a key never seen down is ignored", pop1().kind == IN_NONE);
    fresh(); key(30, 3); key(30, -1); key(INPUT_MAX_KEY + 1, 1); key(0xffff, 1);
    CHECK("bad values and out-of-range codes are ignored and counted", pop1().kind == IN_NONE && S.st.ignored >= 4 && !input_key_held(&S, 30));
    fresh(); key(0x130, 1);
    CHECK("an unsupported button code is ignored", pop1().kind == IN_NONE && S.st.ignored == 1);
    fresh(); input_feed(&S, EV_REL, 0, 5); input_feed(&S, 0x17, 1, 1); input_feed(&S, 0xffff, 0xffff, -1);
    CHECK("unknown/relative event types are ignored and counted", pop1().kind == IN_NONE && S.st.ignored == 3 && S.st.events_in == 3);

    /* --- pointer scaling against an exact oracle --- */
    int sc = 1;
    static const int32_t R[][2] = { {0, 32767}, {-100, 100}, {0, 1}, {-2147483647 - 1, 2147483647}, {1000, 1001}, {5, 4000} };
    static const uint32_t EXT[] = { 1024, 768, 1, 2, 4096 };
    for (unsigned r = 0; r < sizeof R / sizeof R[0]; r++) for (unsigned x = 0; x < sizeof EXT / sizeof EXT[0]; x++) {
        int32_t mn = R[r][0], mx = R[r][1];
        int32_t samples[] = { mn, mx, (int32_t)((int64_t)mn + (((int64_t)mx - mn) / 2)), mn > -2147483647 ? mn - 1 : mn, mx < 2147483647 ? mx + 1 : mx, 0 };
        for (unsigned i = 0; i < 6; i++) {
            __int128 v = samples[i]; if (v < mn) v = mn; if (v > mx) v = mx;
            long exp = (long)(((v - mn) * (__int128)(EXT[x] - 1)) / ((__int128)mx - mn));
            if (input_scale_axis(samples[i], mn, mx, EXT[x]) != exp) { sc = 0; printf("    scale mismatch r=%u x=%u i=%u\n", r, x, i); }
        }
    }
    CHECK("axis scaling matches an exact 128-bit oracle for 6 ranges (incl. the full int32 range), 5 extents, 6 samples each", sc);
    CHECK("scaled coordinates always inside [0, extent-1]", input_scale_axis(-9999, 0, 32767, 1024) == 0 && input_scale_axis(99999, 0, 32767, 1024) == 1023 && input_scale_axis(32767, 0, 32767, 768) == 767);
    CHECK("degenerate range (max <= min) or extent 0 returns 0, never divides by zero", input_scale_axis(5, 7, 7, 1024) == 0 && input_scale_axis(5, 9, 3, 1024) == 0 && input_scale_axis(5, 0, 9, 0) == 0);
    fresh();
    CHECK("an unusable axis range is refused", input_set_axis(&S, 0, 10, 10) == -1 && input_set_axis(&S, 0, 10, 5) == -1 && input_set_axis(&S, 2, 0, 5) == -1);

    /* --- pointer events --- */
    fresh();
    CHECK("pointer starts at the screen centre", S.px == 512 && S.py == 384);
    input_feed(&S, EV_ABS, ABS_X, 0); input_feed(&S, EV_ABS, ABS_Y, 32767);
    CHECK("no PTR_MOVE until SYN_REPORT", pop1().kind == IN_NONE);
    syn(); e = pop1();
    CHECK("SYN_REPORT emits one PTR_MOVE at (0, 767)", e.kind == IN_PTR_MOVE && e.x == 0 && e.y == 767 && pop1().kind == IN_NONE);
    input_feed(&S, EV_ABS, ABS_X, 0); syn();
    CHECK("a report that does not change the position emits nothing", pop1().kind == IN_NONE);
    input_feed(&S, EV_ABS, ABS_X, 16384); input_feed(&S, EV_ABS, ABS_Y, 16384); key(BTN_LEFT, 1);
    e = pop1(); t = pop1();
    CHECK("a press in the same report as a move is delivered AFTER the move, at the new position", e.kind == IN_PTR_MOVE && t.kind == IN_PTR_DOWN && t.x == 511 && t.y == 383);
    key(BTN_LEFT, 1);
    CHECK("a duplicate press is ignored", pop1().kind == IN_NONE);
    key(BTN_LEFT, 0); e = pop1();
    CHECK("release emits PTR_UP", e.kind == IN_PTR_UP && e.x == 511);
    key(BTN_LEFT, 0);
    CHECK("a release without a press is ignored", pop1().kind == IN_NONE);
    fresh(); input_init(&S, 1024, 768);
    input_feed(&S, EV_ABS, ABS_X, 100); syn();
    CHECK("an axis with no declared range (pointer device absent) is ignored and counted", pop1().kind == IN_NONE && S.st.ignored == 1);
    fresh(); input_feed(&S, EV_ABS, 2, 5); input_feed(&S, EV_ABS, 0xffff, 5); syn();
    CHECK("unknown absolute axes are ignored", pop1().kind == IN_NONE && S.st.ignored == 2);
    fresh(); input_feed(&S, EV_ABS, ABS_X, 0x7fffffff); input_feed(&S, EV_ABS, ABS_Y, (int32_t)0x80000000); syn(); e = pop1();
    CHECK("out-of-range raw values clamp to the screen", e.kind == IN_PTR_MOVE && e.x == 1023 && e.y == 0);

    /* --- queue bound --- */
    fresh();
    for (int i = 0; i < 100; i++) key(30 + (uint16_t)(i & 1), (i & 2) ? 0 : 1);
    uint32_t n = 0; while (pop1().kind != IN_NONE) n++;
    CHECK("the queue holds at most INPUT_QUEUE_LEN events and never overflows", n <= INPUT_QUEUE_LEN);
    fresh();
    for (int i = 0; i < 40; i++) { key(30, 1); key(30, 0); }   /* 3 events per iteration = 120 pushes > 64 */
    CHECK("overflow drops the newest events and counts them", S.count == INPUT_QUEUE_LEN && S.st.dropped_out == 120 - INPUT_QUEUE_LEN);
    e = pop1();
    CHECK("the oldest unread event survives an overflow", e.kind == IN_KEY_DOWN && e.key == 30);
    fresh(); for (int r = 0; r < 500; r++) { key(30, 1); (void)pop1(); (void)pop1(); key(30, 0); (void)pop1(); }
    CHECK("ring wraparound over 1500 push/pop cycles leaves an empty, consistent queue", S.count == 0 && S.st.dropped_out == 0 && S.head < INPUT_QUEUE_LEN);

    printf("input_core: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
