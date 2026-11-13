/*
 * tests/programs/setapp.c - EL0 client of the native settings registry (SYS_SETTINGS = 9). The kernel launch registry passes a number in x0:
 *   high 32 bits = mode, low 32 bits = a value.
 *   mode 1 WRITER (identity holds the appearance + attention capabilities): saves text scale = <value>, theme dark, reduced motion on, focus rest,
 *          notifications priority; reads them back; then proves every refusal: the system-only aura mode (permission), unknown id, wrong type, a value
 *          out of range, a 64-bit value whose low 32 bits would be valid (no truncation), an unknown operation.
 *   mode 2 GUEST (no write capability): can read every setting, every write and reset is refused as not permitted.
 *   mode 3 SYSTEM UI (system capability only): may write the aura mode and nothing else.
 *   mode 4 READER: the saved values must be exactly the ones the writer saved: text scale = <value>, theme 2, reduced motion 1, focus 2, notifications 1.
 * Exit 0 = every expectation held; any other code names the first one that did not.
 */
#include "common.h"

#define GET(id)      sc(9, 1, (i64)(id), 0, 0, 0)
#define SET(id, v)   sc(9, 2, (i64)(id), (i64)(v), 0, 0)
#define RESET(id)    sc(9, 3, (i64)(id), 0, 0, 0)
#define EPERM_  (-203)
#define EID_    (-201)
#define EVALUE_ (-202)

void __attribute__((section(".text._start"))) _start(u64 arg)
{
    u64 mode = arg >> 32, val = arg & 0xFFFFFFFFUL;
    if (mode == 1) {
        i64 s = GET(2);
        if (s < 75 || s > 200) quit(10);
        if (SET(2, val) != 0) quit(11);
        if (SET(1, 2) != 0) quit(12);
        if (SET(3, 1) != 0) quit(13);
        if (SET(6, 1) != 0) quit(14);
        if (SET(4, 2) != 0) quit(15);
        if (GET(2) != (i64)val) quit(16);
        if (GET(1) != 2) quit(17);
        if (GET(3) != 1) quit(18);
        if (SET(5, 0) != EPERM_) quit(19);                        /* the aura mode is system policy: this identity lacks the capability */
        if (GET(5) < 0) quit(20);                                 /* ...but it may read it */
        if (SET(99, 1) != EID_) quit(21);                         /* unknown setting */
        if (GET(0) != EID_) quit(22);
        if (SET(1, 3) != EVALUE_) quit(23);                       /* theme has three values */
        if (SET(1, 0x100000002UL) != EVALUE_) quit(24);           /* 2^32 + 2: not "2" */
        if (SET(2, 74) != EVALUE_) quit(25);                      /* text scale 75..200 */
        if (SET(2, 201) != EVALUE_) quit(26);
        if (SET(3, 2) != EVALUE_) quit(27);                       /* a boolean is 0 or 1 */
        if (SET(2, (u64)-1) != EVALUE_) quit(28);
        if (GET(2) != (i64)val) quit(29);                         /* refused writes changed nothing */
        if (sc(9, 77, 0, 0, 0, 0) != -1) quit(30);                /* unknown operation: AE_EINVAL */
        say("SETAPP writer done");
        quit(0);
    }
    if (mode == 2) {
        for (int id = 1; id <= 6; id++) if (GET(id) < 0) quit(40 + id);
        if (SET(1, 1) != EPERM_) quit(50);
        if (SET(2, 100) != EPERM_) quit(51);
        if (RESET(3) != EPERM_) quit(52);
        if (SET(5, 2) != EPERM_) quit(53);
        say("SETAPP guest done");
        quit(0);
    }
    if (mode == 3) {
        if (SET(5, 2) != 0) quit(60);
        if (GET(5) != 2) quit(61);
        if (SET(1, 1) != EPERM_) quit(62);                        /* the system capability does not include appearance */
        say("SETAPP system-ui done");
        quit(0);
    }
    if (mode == 4) {
        if (GET(2) != (i64)val) quit(70);
        if (GET(1) != 2) quit(71);
        if (GET(3) != 1) quit(72);
        if (GET(4) != 2) quit(73);
        if (GET(6) != 1) quit(74);
        say("SETAPP reader done");
        quit(0);
    }
    quit(99);
}
