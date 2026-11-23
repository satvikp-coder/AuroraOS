/* tests/kernel-host/principal_test.c - the reserved-principal policy (kernel/include/principal.h): which identities a process or an application capability may ever
 * hold. Boundaries, every named service identity, and an exhaustive sweep around the reserved range. Host, ASan/UBSan. */
#include <stdio.h>
#include <stdint.h>
#include <principal.h>

static int passed, failed;
#define CHECK(name, cond) do { if (cond) { passed++; } else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)

int main(void)
{
    printf("principal policy tests\n");
    CHECK("the kernel (0) is never assignable", !principal_assignable(0));
    CHECK("PRINCIPAL_NONE (0xFFFFFFFF) is never assignable", !principal_assignable(0xFFFFFFFFu));
    CHECK("installer 0x7F000001, settings 0x7F000002 and shell 0x7F000003 are reserved", principal_is_reserved(0x7F000001u) && principal_is_reserved(0x7F000002u) && principal_is_reserved(PRINCIPAL_SHELL));
    CHECK("Passport and Trail are reserved, distinct, and not assignable", PRINCIPAL_PASSPORT == 0x7F000004u && PRINCIPAL_TRAIL == 0x7F000005u && PRINCIPAL_PASSPORT != PRINCIPAL_TRAIL
          && principal_is_reserved(PRINCIPAL_PASSPORT) && principal_is_reserved(PRINCIPAL_TRAIL) && !principal_assignable(PRINCIPAL_PASSPORT) && !principal_assignable(PRINCIPAL_TRAIL));
    CHECK("inference 0x7F000006 is reserved, distinct from every other service identity, and not assignable", PRINCIPAL_INFER == 0x7F000006u && principal_is_reserved(PRINCIPAL_INFER)
          && !principal_assignable(PRINCIPAL_INFER) && PRINCIPAL_INFER != PRINCIPAL_SHELL && PRINCIPAL_INFER != PRINCIPAL_PASSPORT && PRINCIPAL_INFER != PRINCIPAL_TRAIL);
    { /* the service exception is exactly one identity: no other reserved value, no ordinary identity, not 0 and not none */
        int bad = 0;
        for (uint32_t low = 0; low < 0x10000u; low++) if (principal_service_holdable(0x7F000000u | low) != (low == 6)) bad++;
        uint32_t s = 7; for (int i = 0; i < 1000000; i++) { s = s * 1664525u + 1013904223u; if (principal_service_holdable(s) != (s == PRINCIPAL_INFER)) bad++; }
        CHECK("only PRINCIPAL_INFER is service-holdable (0x7F000000..0x7F00FFFF swept, 1,000,000 random values, 0 and none)", bad == 0 && principal_service_holdable(PRINCIPAL_INFER)
              && !principal_service_holdable(0) && !principal_service_holdable(0xFFFFFFFFu) && !principal_service_holdable(PRINCIPAL_PASSPORT) && !principal_service_holdable(101));
    }
    CHECK("the whole 0x7F000000..0x7FFFFFFF range is reserved (first, last, middle)", !principal_assignable(0x7F000000u) && !principal_assignable(0x7FFFFFFFu) && !principal_assignable(0x7F123456u));
    CHECK("the neighbours of the range are assignable", principal_assignable(0x7EFFFFFFu) && principal_assignable(0x80000000u) && principal_assignable(0x80000001u) && principal_assignable(0xFFFFFFFEu));
    CHECK("ordinary application identities are assignable", principal_assignable(1) && principal_assignable(101) && principal_assignable(0x1001u) && principal_assignable(0x00FFFFFFu) && principal_assignable(0x01000000u));
    CHECK("principals named in the tests and documentation as 'applications' (301..509) are assignable", principal_assignable(301) && principal_assignable(401) && principal_assignable(508) && principal_assignable(509));
    { /* exhaustive over the top byte: assignable iff the top byte is not 0x7F, with the two global exceptions */
        int bad = 0;
        for (uint32_t top = 0; top < 256; top++)
            for (uint32_t low = 0; low < 70000; low += (low < 300 ? 1 : 997)) {
                uint32_t p = (top << 24) | low;
                int want = !(top == 0x7F) && p != 0 && p != 0xFFFFFFFFu;
                if (principal_assignable(p) != want) bad++;
            }
        CHECK("sweep: assignable <=> not 0, not none, top byte != 0x7F", bad == 0);
    }
    { /* a random sample of 1,000,000 32-bit values agrees with the definition */
        uint32_t s = 1; int bad = 0;
        for (int i = 0; i < 1000000; i++) { s = s * 1664525u + 1013904223u; uint32_t p = s; int want = p != 0 && p != 0xFFFFFFFFu && (p >> 24) != 0x7F; if (principal_assignable(p) != want) bad++; }
        CHECK("1,000,000 random identities agree with the definition", bad == 0);
    }
    printf("principal policy: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
