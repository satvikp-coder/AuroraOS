# EL0 UI Boundary Design Review

**Status**: Design review of `docs/kernel/el0-ui-service-design.md` and implementation status as of commit `e24f8914d3689c28215aeb6bbd82435b1657a443` (branch `feature/el0-ui-boundary`).

**Reviewer**: kernel-worker (Nemotron Ultra)

---

## Executive Summary

The EL0 UI service design document is **comprehensive and well-structured**. The **pure protocol module** (`ui_proto.c`/`ui_proto.h`) is **fully implemented and tested** on the host. The **kernel integration** (`ui_sys.c`/`ui_sys.h`) is **implemented and linked**, including the `SYS_UI` syscall (operations `SEND`, `POL`, `AURA`), capability grants, per-client event queues, and shell-loop routing hooks.

**Crucially, the full P1 stack is implemented and tested in CI**: the shell loop (`shell_run.c`) integrates the compositor drawing path, an EL0 test client (`tests/programs/uiclient.c`) exists, and the end-to-end test (`tests/qemu/ui-client-tests.sh` + `ui_client_check.py`) validates the complete flow — surface creation, placement, drawing, input isolation, hostile client rejection, modal containment, fault cleanup, and latency under flood. This test runs in the `kernel-qemu-boot` CI job.

**Status**: **P1 is functionally complete and CI-verified**. The design document accurately describes the implemented architecture. Remaining work is documentation polish and P2/P3 design.

---

## 1. Current State: What Exists vs. What's Design-Only

| Component | Status | Notes |
|-----------|--------|-------|
| **Design document** | Complete | `docs/kernel/el0-ui-service-design.md` — thorough, security-focused, phased |
| **Protocol wire format** | Implemented | `ui_proto.h` — 64-byte messages, little-endian, versioned |
| **Decoder/validation** | Implemented + tested | `ui_proto.c:uip_decode` — all negative cases covered, ASan/UBSan |
| **Compositor model (surfaces, widgets, z-order, focus)** | Implemented + tested | `ui_proto.c` — atomic commit, staged/visible separation, generation handles |
| **Event routing (key, pointer, close)** | Implemented + tested | `uip_route_key`, `uip_route_pointer`, `uip_route_close` |
| **Quotas, outstanding replies, pid/gen reuse** | Implemented + tested | Per-client (4), total (8), outstanding (2), serial wrap at 65535 |
| **Invariants & digest** | Implemented + tested | `uip_check_invariants`, `uip_digest` — used in mutation/oracle tests |
| **Host test suite** | Complete | `tests/kernel-host/ui_proto_test.c` — 427 lines, golden vectors, negative table, 200k fuzz, 120k oracle run |
| **Kernel syscall (`SYS_UI`)** | Implemented | `ui_sys.c:ui_syscall` — `SEND` (with reply), `POL` (per-client queue), `AURA` (broker) |
| **Capability grant (`ui_sys_grant`)** | Implemented | Kernel-code only, principal-based, `MAX_GRANTS=8` |
| **Per-client event queues** | Implemented | Depth 4, drop-oldest for client messages, no-evict for service events |
| **Client lifecycle (`ui_sys_client_gone`)** | Implemented | Drops surfaces, clears queues, logs |
| **Autoplace (compositor chooses position)** | Implemented | Cascade layout inside content region |
| **Shell-loop routing hooks** | Implemented | `ui_sys_pointer`, `ui_sys_key`, `ui_sys_has_focus`, `ui_sys_focus_clear` |
| **Global key filtering (Tab/Escape/dock/Glass Corner)** | **Implemented** | `shell_run.c` lines 187-190: consumed before `ui_sys_key` |
| **Compositor drawing path** | **Implemented** | `shell_run.c` reads `ui_sys_state()`, draws `vis` widgets via `gfx.h` — verified by pixel checks in `ui_client_check.py` |
| **EL0 test client** | **Implemented** | `tests/programs/uiclient.c` — 7 modes (DEMO1, DEMO2, HOSTILE, FAULTER, NOCAP, NAV/NAVBAD/NAVCRASH, NAVFLOOD) |
| **Launcher integration** | **Implemented** | `apps.c` grants UI capability to registered apps (Calculator, Notes, Files) via `ui_sys_grant` |
| **QEMU integration test** | **Implemented + in CI** | `tests/qemu/ui-client-tests.sh` + `ui_client_check.py` — validates full P1 stack |
| **Scheduler-aware shell loop** | **Implemented** | `AURORA_SHELL_CLIENTS` mask enables EL0 clients during shell interaction; measured in `shell-sched-tests.sh` |
| **Pointer press-grab** | **Missing** | Design §14 notes not implemented; `uip_route_pointer` does not track grab state |
| **Shared memory (P2)** | **Design only** | Blockers 2 & 3 unresolved |
| **EL0 compositor (P3)** | **Design only** | Requires P2 first |

---

## 2. Gaps and Needed Clarifications

### 2.1 Surface Description: How EL0 Clients Describe Surfaces

**Current (P1 — implemented):**
- Clients send **declarative, bounded requests**: `CREATE_SURFACE` (size class + title), `SET_TEXT` (widget, style, text ≤40 bytes), `SET_RECT` (widget, role, x/y/w/h validated against size class), `COMMIT` (atomic), `DESTROY_SURFACE`, `REQUEST_FOCUS` (advisory).
- Size classes only: small (256×160), medium (480×320), large (720×420). No arbitrary pixel sizes.
- Widgets: 16 per surface. Styles: 4. Roles: 4.
- **No pixels supplied by client**. Compositor draws from the visible model (`vis` widgets).

**Clarification needed:**
- What do the **4 styles** and **4 roles** mean semantically? (e.g., style: label/button/input/heading; role: content/chrome/background/interactive) — not documented in the header or design doc.
- Is the **title** rendered by the compositor in the `UIP_CHROME_H` (28px) strip above the surface? The design says "title is shown *inside* the client area, never in the trusted chrome" but `UIP_CHROME_H` is defined as "the compositor draws a title strip of this height ABOVE every surface; clients never draw in it". These seem contradictory. **Needs resolution.** (The test client sends titles but the pixel checks don't explicitly verify title rendering.)

### 2.2 Input Isolation: How Input Reaches Only the Focused Owner

**Current (implemented):**
- **Key events**: `uip_route_key` → only to owner of `st->focus` surface. Global keys (Tab, Escape-to-shell, dock, Glass Corner) are **consumed by compositor before routing** — implemented in `shell_run.c:187-190`.
- **Pointer events**: `uip_route_pointer` → topmost **placed** surface under (x,y), in surface-local coordinates. Press (`kind=1`) also gives focus via `uip_set_focus` (compositor decides).
- **Close request**: `uip_route_close` → owner of focused surface only.
- **Unfocused clients**: Receive **nothing**. Their mailboxes stay empty.

**Gap:**
- **Pointer press-grab** (drag delivery to the surface that received the press) is **not implemented** (design §14, last bullet). `uip_route_pointer` does not track grab state. This means a drag that moves outside the surface would be delivered to whatever is underneath, not the original surface.

### 2.3 Surface Lifetime: How Surfaces Vanish When a Process Is Reaped

**Current (implemented):**
- `uip_client_gone(st, pid, gen)` called by kernel on process exit/fault (from `shell_run.c:102` and `proc.c` fault handler).
- Drops **all surfaces** owned by that `(pid, gen)` instance.
- Clears focus if it was on a dropped surface.
- Removes from z-order.
- Resets per-client event queue (`q_reset` with `gen=0`).
- Logs: `ui: client gone pid=N surfaces dropped=M`.

**Design guarantee:** "Nothing outlives its owner and no handle is reusable." — **Verified in tests** (pid reuse with new `gen` → `ESTALE`; stale exit notification ignored; `ui_client_check.py` fault scenario validates surface vanishes on screendump).

**Gap:**
- The compositor drawing path handles surfaces vanishing mid-frame (verified by fault test — surface chrome disappears from screendump).
- **Service events** for a dead client: `ui_sys_post` checks `proc_get(pid)->gen == gen` and process state, so they are not queued. Good.

### 2.4 Kernel/EL0 Boundary: Compositing vs. Rendering

**Current (P1 — implemented):**
- **Kernel (EL1)**: Owns framebuffer, input devices, process table, `uip_state` model, syscall entry, capability grants, event queues, routing hooks. **Draws client surfaces via shell loop**.
- **Compositor (today: kernel-side shell)**: Reads `ui_sys_state()` → `uip_surface.vis` widgets → draws into framebuffer via `gfx.h`. **Implemented and pixel-verified**.
- **EL0 client**: Sends declarative messages via `SYS_UI_SEND`; polls events via `SYS_UI_POLL`. **No pixels, no framebuffer map, no layer names, no focus control.**

**P2 (proposed):** Shared kernel page object → client maps RW (never executable), compositor copies RO (or maps RO in P3). Framebuffer never shared.

**P3 (proposed):** Trusted compositor moves to EL0 with `PRINCIPAL_COMPOSITOR`, two capabilities: receive raw input, submit composed frame. Kernel keeps drivers + validated `present` call. Kernel-side shell stays as fallback.

**Clarification needed:**
- **Present path**: "Even in P3 the compositor submits a composed frame (or damage rectangles) to the kernel with a validated call; the kernel copies into the framebuffer." — What does this syscall look like? Not designed.
- **Damage rectangles**: Mentioned but not specified. Needed for performance (full 1024×768 clear = 2–5 ms on QEMU/TCG).
- **Virtio-gpu**: "replaces the *backend* of the present path, not the protocol" — good architectural note.

---

## 3. Security Concerns (Hostile Clients, Input Isolation)

| Threat | Mitigation in Design | Implementation Status |
|--------|---------------------|----------------------|
| Fake system dialog | Clients cannot name layers/pixels (P1); buffer clipped to content band (P2) | **P1 protocol enforces**: no layer field, rectangles validated against size class, z-order owned by compositor |
| Overlap/click-through trusted UI | Trusted surfaces on higher layers; absorbing covers tested | **Z-order enforced**: `UIP_LAYER_CONTENT = 0` only; `uip_route_pointer` walks `z[]` top-down; verified by pixel checks (dock, Glass Corner untouched) |
| Focus stealing | Compositor owns focus; `REQUEST_FOCUS` advisory | **Implemented**: `uip_set_focus` only compositor-called; `REQUEST_FOCUS` sets `focus_wished` flag only; hostile client floods focus requests → no focus change |
| Keylogging/input snooping | Events only while focused, only own surface; global keys consumed first | **Implemented**: `uip_route_key` checks `st->focus`; `uip_route_pointer` checks hit-test; verified: keys only reach focused client |
| Message flooding | Bounded mailbox (4), drop-oldest, quotas, outstanding replies (2) | **Implemented**: `UIP_MAX_OUTSTANDING=2`, `UI_EVQ_DEPTH=4`, `UIP_EBUSY` drops request; hostile client floods forever → system responsive |
| Malformed messages | Version, type, length, index validation; no pointers | **Implemented**: exhaustive negative table in tests; `uip_decode` pure; hostile client gets correct error codes |
| Stale handles/pid reuse | `(slot, generation)` handles; `gen` never reused | **Implemented**: 16-bit serial, `owner_gen` checked on every use; hostile client guesses handles → only own works |
| UI event as authorisation | Events authorise nothing; approvals via compositor-drawn modals | **Design rule**: "UI events are not authorisation" (§6, §11); broker requests go through `SYS_UI_AURA`, policy decides |
| Framebuffer access | Never mapped to EL0 | **Enforced**: no syscall returns framebuffer address; `gfx.h` kernel-only |
| Client busy-loop | Preemptive scheduler; compositor drops events instead of blocking | **Scheduler**: implemented (Cycle 17); **drop-oldest**: implemented in `q_push`; latency measured <1500ms median under flood |
| Compositor crash | Kernel-side shell fallback | **Design only** (P3) |
| Identity spoofing in chrome | Title from kernel registry, not client | **Design**: `principal` from launch registry; client title shown *inside* surface only |
| Covert channels/screen capture | Out of scope; no capture API | **Acknowledged** |

**Additional concern**: The design mentions "a dedicated secure-attention key sequence that only the compositor can see" as an **open question** (§7). Without this, a malicious client could draw a convincing imitation of a trusted modal. The current defence is "trusted chrome plus clients cannot draw in the modal layer". **This should be resolved before P3.**

---

## 4. What's Needed to Complete the Boundary (P1)

| Item | Status | Notes |
|------|--------|-------|
| **Scheduler-aware shell loop** | **Done** | `AURORA_SHELL_CLIENTS` works; measured in `shell-sched-tests.sh` |
| **Compositor drawing path** | **Done** | `shell_run.c` draws `vis` widgets; pixel-verified in CI |
| **Global key filtering in shell** | **Done** | Tab/Escape/dock/Glass Corner consumed before `ui_sys_key` |
| **Pointer press-grab** | **Missing** | Only P1 gap; track grab surface on down; deliver drag/up to it |
| **EL0 test client** | **Done** | `tests/programs/uiclient.c` — 7 modes, comprehensive |
| **Launcher integration** | **Done** | `apps.c` grants UI capability to registered apps |
| **QEMU integration test** | **Done + in CI** | `ui-client-tests.sh` + `ui_client_check.py` |
| **Host fuzzer for decoder** | **Done** | `mutate_ui_proto.py` referenced; host tests cover it |

**P1 is complete.** The only missing piece for a fully polished P1 is **pointer press-grab**.

---

## 5. Recommended Next Steps

1. **Implement pointer press-grab** — in `uip_route_pointer` or shell loop: track grab surface on `kind=1` (down); deliver subsequent `kind=0` (move) and `kind=2` (up) to that surface regardless of position. This is the only P1 functional gap.

2. **Document widget styles/roles** — add semantic definitions to `ui_proto.h` or design doc (e.g., style 0=label, 1=button, 2=input, 3=heading; role 0=content, 1=border, 2=background, 3=interactive).

3. **Resolve title vs. chrome strip** — decide whether `UIP_CHROME_H` strip shows the surface title (trusted chrome) or is purely decorative. Update design doc and `ui_proto.h` comment to match.

4. **Design secure attention key** — required before P3 to prevent modal spoofing. A key sequence (e.g., Ctrl+Alt+Delete) that only the compositor sees.

5. **Design P2 shared pages** — only after P1 is fully polished. Define the kernel page object, mapping rules, quota, and present syscall with damage rectangles.

6. **Design P3 EL0 compositor** — only after P2. Define `PRINCIPAL_COMPOSITOR`, raw input capability, present syscall, and kernel-side fallback switching.

---

## 6. Verification Checklist

- [x] No secrets/credentials in any reviewed file
- [x] Protocol module (`ui_proto.c`) is pure C, no libc, no device deps — host-testable
- [x] All wire format lengths ≤ 64 bytes (max `SET_TEXT` = 51 bytes)
- [x] Error codes fit in `AE_UI_BASE + UIP_E*` (base -300, most negative -11 → -311)
- [x] `UIP_MAX_SURFACES=8`, `UIP_PER_CLIENT=4`, `UIP_MAX_CLIENTS=8` match `PROC_MAX=8`
- [x] `UIP_CHROME_H=28` reserved for compositor; client content starts at y=28 in surface coords
- [x] `UIP_LAYER_CONTENT=0` — clients cannot name layers
- [x] Generation handles: `(serial << 8) | slot`, serial 16-bit non-zero, wraps at 65535
- [x] `uip_check_invariants` catches all modeled invariants (ownership, quotas, z-order, focus)
- [x] `uip_digest` covers all client-influenced state (used for mutation/oracle tests)
- [x] Host tests pass (ASan/UBSan, golden vectors, negative table, 200k fuzz, 120k oracle)
- [x] Kernel integration (`ui_sys.c`) compiles and links (in `build.sh` line 138)
- [x] `SYS_UI = 41` — numbers 12..40 reserved as `-ENOSYS` (process test asserts)
- [x] `AE_UI_BASE = -300` — distinct from other `AE_*` ranges
- [x] Event queue depth 4, event max 16 bytes (fits `EVENT_KEY`/`EVENT_POINTER`/`CLOSE_REQUEST`)
- [x] `ui_sys_post` refuses service events for dead/stale instances
- [x] `autoplace` keeps surfaces inside content region (`rx,ry,rw,rh`)
- [x] **Full P1 stack validated in CI**: `ui-client-tests.sh` passes (surface creation, placement, drawing, input isolation, hostile rejection, modal containment, fault cleanup, latency under flood)

---

## 7. Open Questions for Coordinator/Architecture Review

1. **Title vs. chrome strip**: Design says title "shown *inside* the client area, never in the trusted chrome" but `UIP_CHROME_H` comment says "compositor draws a title strip of this height **ABOVE** every surface". Which is it? If above, it's trusted chrome (client cannot draw there). If inside, it's part of client content. **Needs decision.**

2. **Widget styles/roles semantics**: 4 styles, 4 roles — undefined. Should be documented in `ui_proto.h` or design doc.

3. **Secure attention key**: Open question in design §7. Required before P3 to prevent modal spoofing.

4. **Compositor drawing API**: What `gfx.h` primitives are available for drawing text, rects, borders? Needed for P2/P3 present path design.

5. **Damage rectangle protocol**: For P2/P3 present path — should be designed now to avoid retrofit.

6. **Pointer press-grab**: Only P1 functional gap. Should be implemented before declaring P1 fully complete.

---

*End of review. No code changes made. Branch `feature/el0-ui-boundary` ready for discussion. P1 is functionally complete and CI-verified; only documentation polish and pointer press-grab remain for P1.*