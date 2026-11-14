#!/usr/bin/env python3
"""tests/kernel-host/mutate_ui_proto.py - mutation check for kernel/ui/ui_proto.c: each mutant injects one representative defect; the host suite
(ui_proto_test.c) must FAIL for every one. Run from the repository root: python3 tests/kernel-host/mutate_ui_proto.py
Not part of CI (it compiles the suite once per mutant); its result is recorded in the PR."""
import os, subprocess, sys, tempfile, shutil

SRC = "kernel/ui/ui_proto.c"
TEST = "tests/kernel-host/ui_proto_test.c"
# Equivalent mutants (documented, expected to survive): the generation and the principal checks inside the surface lookup are defence in depth.
# A caller with another generation or principal is treated as a NEW instance, and the previous instance's surfaces are dropped before any lookup,
# so no surface owned by a different generation or principal can still be reached when the lookup runs. uip_check_invariants asserts that every
# live surface's owner generation equals the registered instance's.
EQUIVALENT = ("surface lookup ignores the owner generation", "surface lookup ignores the owner principal")
MUTANTS = [
    ("surface lookup ignores the owner pid", "s->owner_pid != w->pid ||", ""),
    ("surface lookup ignores the owner generation", "s->owner_gen != w->gen ||", ""),
    ("surface lookup ignores the owner principal", "|| s->owner_principal != w->principal", ""),
    ("serial is not checked against the handle", "|| s->serial != (uint16_t)(handle >> 8)", ""),
    ("SET_TEXT writes the visible copy directly (not atomic)", "g->has_text = m->tlen ? 1 : 0; g->style = m->b;", "s->vis[m->a].has_text = m->tlen ? 1 : 0; g->has_text = m->tlen ? 1 : 0; g->style = m->b;"),
    ("COMMIT copies nothing", "for (int i = 0; i < UIP_WIDGETS; i++) s->vis[i] = s->pend[i];", ""),
    ("per-client quota off by one", "if (c->nsurf >= UIP_PER_CLIENT) return UIP_EQUOTA;", "if (c->nsurf > UIP_PER_CLIENT) return UIP_EQUOTA;"),
    ("global quota ignored", "if (slot < 0) return UIP_EQUOTA;", "if (slot < 0) slot = 0;"),
    ("rect: an exactly fitting width is rejected", "(int32_t)m->x + (int32_t)m->w > sw", "(int32_t)m->x + (int32_t)m->w >= sw"),
    ("rect: one pixel too wide is accepted", "(int32_t)m->x + (int32_t)m->w > sw", "(int32_t)m->x + (int32_t)m->w > sw + 1"),
    ("rect: negative x accepted", "m->x < 0 || m->y < 0 ||", "m->y < 0 ||"),
    ("rect: 16-bit overflow arithmetic", "(int32_t)m->y + (int32_t)m->h > sh", "(int16_t)(m->y + m->h) > sh"),
    ("decoder accepts a zero-size rectangle", "if (m.w == 0 || m.h == 0) return UIP_EBADARG;", ""),
    ("DEL accepted in text", "s[i] > 0x7e", "s[i] > 0x7f"),
    ("control characters accepted in text", "s[i] < 0x20 ||", ""),
    ("decoder ignores the version", "if (b[0] != UIP_VERSION) return UIP_EVERSION;", ""),
    ("decoder accepts trailing bytes on SET_TEXT", "if (len > 11 + tl) return UIP_EBADLEN;", ""),
    ("decoder accepts an oversized text length", "if (tl > UIP_TEXT_MAX) return UIP_EBADLEN;", ""),
    ("decoder lets a client send a compositor type", "default: return UIP_ETYPE;                                    /* includes", "case UIP_STATUS: break; default: return UIP_ETYPE;                                    /* includes"),
    ("REQUEST_FOCUS moves focus", "s->focus_wished = 1;", "s->focus_wished = 1; st->focus = (int8_t)slot;"),
    ("a reused slot keeps its serial (the old handle works again)", "uint16_t serial = (uint16_t)(s->serial + 1);", "uint16_t serial = s->serial ? s->serial : 1;"),
    ("destroy does not clear focus", "if (st->focus == slot) st->focus = -1;", ""),
    ("destroy does not remove the surface from the z-order", "    z_remove(st, slot);\n}\n\nstatic void drop_instance", "}\n\nstatic void drop_instance"),
    ("the outstanding limit is not enforced", "if (c->outstanding >= UIP_MAX_OUTSTANDING) return UIP_EBUSY;", ""),
    ("a replayed CREATE is accepted", "if (c->have_seq && c->last_seq == m->seq) return UIP_EDUP;", ""),
    ("a new instance in the pid slot does not drop the old surfaces", "drop_instance(st, who->pid, c->gen);\n            zero(c, sizeof *c);", "zero(c, sizeof *c);"),
    ("client exit ignores the generation", "if (!c->used || c->gen != gen) return;", "if (!c->used) return;"),
    ("client exit does not destroy the surfaces", "    drop_instance(st, pid, gen);\n    zero(c, sizeof *c);", "    zero(c, sizeof *c);"),
    ("key events go to the wrong owner (slot 0)", "to->pid = s->owner_pid; to->gen = s->owner_gen; }\n    return n;\n}\n\nuint32_t uip_route_pointer", "to->pid = st->s[0].owner_pid; to->gen = st->s[0].owner_gen; }\n    return n;\n}\n\nuint32_t uip_route_pointer"),
    ("pointer events ignore the z-order (bottom first)", "for (int k = (int)st->nz - 1; k >= 0; k--) {", "for (int k = 0; k < (int)st->nz; k++) {"),
    ("pointer events reach unplaced surfaces", "if (!s->live || !s->placed) continue;", "if (!s->live) continue;"),
    ("pointer coordinates are not made surface-local", "(int16_t)(x - s->sx), (int16_t)(y - s->sy)", "(int16_t)x, (int16_t)y"),
    ("placement is not checked against the region", "if ((int32_t)x < st->rx ||", "if (0 && (int32_t)x < st->rx ||"),
    ("a missing UI capability is not enforced", "if (!who->can_ui) {", "if (0) {"),
    ("the status reply is not produced", "if (reply && reply_len) *reply_len = uip_encode_status", "if (0 && reply && reply_len) *reply_len = uip_encode_status"),
    ("serial 0 is allowed (handle 0 becomes a valid handle)", "if (serial == 0) serial = 1;", ""),
    ("handle bits above 24 are ignored", "(handle >> 24) != 0 ||", ""),
]

def run(src_text, tmp):
    c = os.path.join(tmp, "m.c"); open(c, "w").write(src_text)
    exe = os.path.join(tmp, "m.out")
    r = subprocess.run(["cc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-I", "kernel/include", "-o", exe, TEST, c], capture_output=True, text=True)
    if r.returncode: return "COMPILE", r.stderr.splitlines()[:1]
    r = subprocess.run([exe], capture_output=True, text=True, timeout=300)
    return ("SURVIVED" if r.returncode == 0 else "caught"), (r.stdout.count("FAIL") + r.stderr.count("runtime error"))

def main():
    orig = open(SRC).read()
    tmp = tempfile.mkdtemp()
    survivors = 0
    try:
        base, _ = run(orig, tmp)
        if base != "SURVIVED":
            print("the unmutated source does not pass its own suite"); return 2
        for name, old, new in MUTANTS:
            if old not in orig:
                print("NOT APPLIED: %s" % name); survivors += 1; continue
            res, info = run(orig.replace(old, new, 1), tmp)
            print("%-8s %s (%s)" % (res, name, info))
            if res != "caught" and name not in EQUIVALENT: survivors += 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("mutants: %d (%d documented equivalent), unexpected survivors or errors: %d" % (len(MUTANTS), len(EQUIVALENT), survivors))
    return 1 if survivors else 0

if __name__ == "__main__":
    sys.exit(main())
