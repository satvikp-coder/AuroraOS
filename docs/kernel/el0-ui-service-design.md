# EL0 UI service and compositor boundary: design

Status: **design only. Nothing in this document is implemented.** Written against the code on `main` at `d3b820e` (QEMU `virt` only; nothing here says anything about real hardware). Every statement under "What exists today" was checked in the named file; everything else is a proposal and is marked as such. Related: [native-graphics-plan.md](native-graphics-plan.md), [userspace.md](userspace.md), [loader.md](loader.md), [settings.md](settings.md), `kernel/include/shell.h`.

## 1. Why this is needed, and what is not being proposed

The native shell works, but it is kernel code: screens, layout, input dispatch and drawing all run at EL1 inside the privileged kernel. That is acceptable as bring-up and as a **fallback** (see section 9), but a final AuroraOS must not require every screen and application to be privileged. This design describes how untrusted EL0 clients can show UI and receive input without ever receiving the framebuffer, another client's input, a protected dialog, or a say in what the user is shown as "trusted".

Not proposed: mapping the framebuffer into any EL0 process (a client **or** the compositor); a general "draw these pixels" syscall; letting a client choose its own identity, name, layer, focus or z-order; moving the working shell into EL0 in one step; deleting the kernel-side shell.

## 2. What exists today (verified in code)

| Fact | Where |
|---|---|
| EL0 processes have a private 39-bit address space: one RX code page, one RW data page, a guard page, a 2-page stack, at fixed virtual addresses; **at most `ASPACE_MAX_PAGES = 24` pages per process** including page tables | `kernel/include/proc.h` |
| At most `PROC_MAX = 8` processes; each has a `gen` (unique per instance, never reused) | `proc.h` |
| IPC: each process owns one mailbox, **4 messages of at most 64 bytes**, copied into kernel storage; a send needs an explicit kernel grant to that recipient *instance* (`proc_ipc_allow`, bound to `gen`); the sender id is filled in by the kernel; there is no call that names another process's queue | `proc.h`, `kernel/proc/ipc.c` |
| Syscalls: write, yield, getpid, exit, ticks, send, recv, fs, settings. **No** shared memory, no display, no input, no process-listing and no spawn call | `proc.h` |
| Identity (`principal`) is set by kernel code only; programs are launched only by kernel code from an installer-owned executable, with the principal taken from the kernel's registry entry | `launch.h`, `loader.md` |
| The framebuffer is one kernel-owned, physically contiguous region; it is never mapped into any EL0 address space | `kernel/include/gfx.h` |
| Raw input is read by the kernel (virtio keyboard/tablet, polled) into a bounded event queue | `kernel/drivers/virtio_input.c`, `kernel/input/input_core.c` |
| The shell is a kernel-side scene (`ui_scene`) plus focus engine (`ui_focus`); its rule is "a UI event is never permission for a privileged action" | `shell.h` |
| **The shell loop never returns and never runs the scheduler.** After "Boot complete" the kernel enters `shell_loop`, which polls input and sleeps in `wfi`. EL0 processes run only earlier, during the bounded boot self-tests and `init_run` | `kernel/kernel.c`, `kernel/ui/shell_run.c` |

## 3. True architectural blockers (an EL0 compositor cannot be built until these change)

1. **No concurrency between the shell and EL0. (Resolved in Cycle 17: the shell loop now hands READY processes bounded slices, see `native-graphics-plan.md` section 15.)** Originally the shell loop did not schedule processes, so no client could run while the user interacted.
2. **The 24-page address-space cap and the single code page.** A framebuffer-sized buffer is 768 pages (3 MiB at 1024x768 XRGB). An EL0 compositor holding its own back buffer, glyph data and scene cannot fit. Clients that want a pixel buffer cannot either.
3. **No shared-memory primitive.** All data crosses the boundary as 64-byte copies, at most 4 queued.
4. **No input-delivery or display path for EL0.** There is no syscall to receive input events or to present a frame.
5. **`PROC_MAX = 8`** bounds clients plus the compositor plus init.
6. **No scheduling priority.** A compute-bound client is preempted by the timer, which is good, but the compositor cannot be given latency preference.

These are why section 4 migrates in phases and why phase 1 deliberately needs none of them to be fixed first.

## 4. Roles and phased migration

### Roles

* **Kernel display and input drivers (EL1, always).** Own the framebuffer, ramfb (and a future virtio-gpu), the virtio-input devices, the timer and the process table. They validate every request that reaches them. They are the only code that touches the framebuffer or raw devices.
* **Trusted compositor (the `ui_scene`/`ui_focus`/shell logic, today EL1).** Owns global focus, z-order, layers, trusted modal surfaces, the window chrome, and the routing of input. Today this is the kernel-side shell. Later it may become one EL0 process with a dedicated principal (`PRINCIPAL_COMPOSITOR`, never given to any other process) and two capabilities: receive raw input, and submit a composed frame to the kernel for presentation. It never maps the framebuffer.
* **EL0 application clients (untrusted).** Describe their own surface and receive input addressed to it. They get no pointer to the framebuffer, the scene, the focus context, another client's surface or any raw input.

### Phases

| Phase | What changes | Needs from section 3 |
|---|---|---|
| **P0 (today)** | Kernel-side shell only. | none |
| **P1 declarative clients** | EL0 clients send **bounded, typed, declarative requests** (create a surface, set a label, set a rectangle, commit, destroy). The in-kernel compositor validates them and draws; the client never supplies pixels. Input flows back as typed events. | blocker 1 (the shell loop must run the scheduler) and a minimal request syscall; **not** shared memory and **not** a larger address space |
| **P2 pixel surfaces** | A kernel shared-page object lets a client render into its own buffer; the compositor copies from it (never executes it) and composes. | blockers 2 and 3 |
| **P3 EL0 compositor** | The trusted compositor moves out of the kernel into one privileged EL0 service. The kernel keeps the drivers and a validated "present" call. The kernel-side shell stays as the fallback. | blockers 2, 3, 4, 6 |

P1 is the first phase worth building: it moves *applications* out of the kernel and proves the protocol and the security properties while the compositor remains trusted kernel code. Declarative requests also keep the attack surface small: a client cannot express "draw over the trusted dialog" because it has no way to name pixels or layers.

## 5. Message protocol (proposed; fits the existing 64-byte IPC message)

Every message starts with a fixed header: `version` (1 byte), `type` (1), `seq` (2), `surface` (4: a handle, not a pointer), then up to 56 bytes of payload. All lengths and indices are validated against their documented maxima before use; an unknown version or type, a bad length, or a handle that does not belong to the sender is rejected with an error code and changes nothing.

Client to compositor:

| Type | Payload | Notes |
|---|---|---|
| `CREATE_SURFACE` | size class (small/medium/large; not arbitrary pixels), title text (at most 24 ASCII bytes) | The title is shown *inside* the client area, never in the trusted chrome |
| `SET_TEXT` | widget index (at most 15), text (at most 40 ASCII bytes), style (a small enum) | |
| `SET_RECT` | widget index, x, y, w, h in surface coordinates, role (a small enum) | clamped to the surface |
| `COMMIT` | none | atomic: nothing is shown until commit |
| `DESTROY_SURFACE` | none | |
| `REQUEST_FOCUS` | none | **advisory only** (section 7) |

Compositor to client (only about that client's own surfaces):

| Type | Payload |
|---|---|
| `EVENT_KEY` | evdev code, modifiers, repeat flag; delivered only while the surface has focus |
| `EVENT_POINTER` | surface-local x, y and button state; delivered only inside the surface, or to the surface that holds a press |
| `CLOSE_REQUEST` | none (the user asked to close; the client may clean up) |
| `STATUS` | the result of the last request (`OK`, `E2BIG`, `EQUOTA`, `ESTALE`, `EINVAL`) |

Replies and events use the same 64-byte mailbox. Because a mailbox holds only 4 messages, the protocol is request/response with a bounded outstanding window of one request per surface, and the compositor **drops the oldest pending event** (never blocks) when a client does not drain its mailbox: a slow or hostile client loses its own events and cannot stall the compositor or anyone else.

## 6. Identity and authority

* The compositor learns *who sent a message* from the kernel (`from` pid plus the process `gen` and `principal` recorded by the kernel), never from message contents.
* The application **name shown in the chrome comes from the kernel's launch registry**, not from the client. A client cannot label itself "Settings" or "AuroraOS" in the trusted area.
* A surface handle is `(slot, generation)`. A handle whose generation does not match, or whose owner is not the sender, is `ESTALE`. This is what makes pid reuse harmless: a new process in the same slot has a new `gen` and cannot touch a dead process's surfaces.
* Capabilities to create surfaces are granted by kernel code (as with `settings_grant` and `proc_ipc_allow`). No syscall grants, lists or revokes them.
* **UI events are not authorisation.** A click or key delivered to a client authorises nothing in the kernel, the registry, the filesystem or the Action Broker. This keeps the rule already stated in `shell.h`.

## 7. Focus, z-order and trusted surfaces

* **Focus is owned by the compositor.** Keyboard events go to exactly one surface. Tab, Escape-to-shell, the dock and the Glass Corner are handled by the compositor *before* any client sees a key; a client cannot consume, delay or observe them.
* A client cannot take focus. `REQUEST_FOCUS` is honoured only when the user's last input just activated that client's launcher entry, or by explicit launcher policy. Focus changes are never announced to the other clients, so focus cannot be used as a signal.
* **Z-order is owned by the compositor.** Layer numbers are the existing `ui_scene` layers. Client surfaces live **only in the content band (layer 0)**, below the dock, the Glass Corner, sheets, the scrim, modals and toasts. A client has no way to name a layer. Therefore a client cannot cover a trusted element, and a client's surface cannot be raised above one.
* **Trusted modals** (authorisation prompts for the Action Broker, a future Passport, system sheets) are drawn only by the compositor in the modal layer with chrome that no client can reproduce: the border colour and a reserved status strip are drawn from compositor-only tokens, the title comes from the kernel registry, and a client surface never overlaps the strip. While one is open, input to all clients is withheld (the existing modal containment rule). A client cannot open, close, answer or move a trusted modal.
* **Open question (not decided here):** a dedicated secure-attention key sequence that only the compositor can see, so a user can tell a real prompt from a client-drawn imitation. Until one exists, trusted chrome plus "clients cannot draw in the modal layer" is the whole defence against spoofing.

## 8. Buffers, copying, lifetime

* **P1 copies only.** All state crosses as 64-byte messages. There is no shared memory.
* **P2 shared surfaces (proposed primitive).** A kernel-owned page object, created by the compositor's policy for one client, mapped **RW in that client** and **never executable**, and read by the compositor only by copying out of it (the kernel maps it RO into the compositor's address space in P3, or copies on the compositor's behalf in P2). Per-client quota (for example 64 pages), freed when the client exits or faults, never reused while mapped anywhere. The framebuffer is **never** such an object.
* **Present path.** Even in P3 the compositor submits a composed frame (or damage rectangles) to the kernel with a validated call; the kernel copies into the framebuffer. Measured cost of a full 1024x768 clear on QEMU/TCG is about 2 to 5 ms (`native-graphics-performance.md`), so a kernel-side copy is affordable, and damage rectangles reduce it further. This is a proposal to be re-measured, not a promise.
* **Per-client limits (proposed):** 4 surfaces, 16 widgets per surface, text at most 40 bytes, one outstanding request per surface. Exceeding a limit returns `EQUOTA` and changes nothing.
* **Lifetime.** Surfaces are owned by `(pid, gen)`. When a process exits or faults the kernel calls a compositor hook in the same tick: its surfaces are destroyed, focus moves away if it was there, and queued events for it are dropped. Nothing outlives its owner and no handle is reusable.

## 9. Crash recovery and fallbacks

* **Client crash or fault:** its surfaces vanish; the compositor shows a trusted toast ("`<name from the registry>` stopped"); nothing else is affected. This is the same containment `userspace.md` already demonstrates for faults.
* **Compositor crash (P3):** the kernel detects an exited or faulted compositor, or a missing present within a bound, and switches to the kernel-side shell, which stays linked in every build. Clients are told their surfaces are gone. The user always has Home, the dock and Settings.
* **No display, or no input device:** unchanged from today: UART boot, read-only shell with "limited input". EL0 clients simply receive no events.
* **No display driver for a device:** ramfb remains the QEMU-only Tier 0 fallback; a future virtio-gpu replaces the *backend* of the present path, not the protocol.

## 10. Memory budgets

* Baseline target stays 256 MiB guest RAM; 128 MiB must keep booting.
* Kernel-side compositor scene today is about 9.6 KB (`ui_scene` measurement) plus the 3 MiB framebuffer.
* P1 adds per-client protocol state only: roughly 16 widgets x 72 bytes plus 16 text slots x 40 bytes per surface, at most 4 surfaces per client, at most 7 clients, so well under 40 KB in total.
* P2 buffers are quota-bound per client; a 3 MiB full-screen client buffer is **not** allowed on the 256 MiB target by default.

## 11. Threat model

| Threat | Mitigation in the design | How it will be tested |
|---|---|---|
| A client draws a fake system dialog | clients cannot name layers or pixels in P1; in P2 their buffer is clipped to the content band and cannot reach the modal layer or the status strip | EL0 client asks for out-of-band rectangles: clamped or refused; screenshot proves no ink outside its surface |
| A client overlaps or clicks through trusted UI | trusted surfaces are on higher layers; absorbing covers (already tested) | extend the existing hit-test suite with client surfaces under a modal |
| Focus stealing | only the compositor assigns focus; `REQUEST_FOCUS` advisory | client requests focus while another surface is focused: no change |
| Keylogging or input snooping | a client receives events only while focused, only about its surface; unfocused clients get nothing; global keys are consumed first | two clients, one focused: the other's mailbox stays empty |
| Message flooding | bounded mailbox, drop-oldest, per-client quotas, one outstanding request | flood test: compositor latency and other clients unaffected |
| Malformed messages | version, type, length and index validation; no pointers in messages | host fuzz of the decoder (like the existing `idx`/`input` fuzzers) plus an EL0 client sending garbage |
| Stale handles and pid reuse | `(slot, generation)` handles; `gen` never reused | exit a client, start another in the same slot, replay the old handle: `ESTALE` |
| UI event used as authorisation (confused deputy) | events authorise nothing; approvals only through compositor-drawn trusted modals | none of the client request types reaches the registry, filesystem or broker; reviewed in code and asserted by grep-style tests |
| Framebuffer access | never mapped to EL0, compositor included | unchanged mapping tests; no new syscall returns a framebuffer address |
| Client never yields or loops | preemptive scheduler (already tested); compositor drops events instead of blocking | busy client plus interactive input: shell latency measured |
| Compositor crash | kernel-side shell fallback | fault-injection test (P3) |
| Identity spoofing in chrome | the title comes from the kernel registry | client sends a title equal to "Settings": chrome still shows the registry name |
| Covert channels, screen capture | out of scope; no capture API exists | n/a |

## 12. Test plan by phase

* **P1 (first PR candidates):** (a) a pure, host-tested protocol module: decode, validate, per-client state, quotas, generation handles, with ASan/UBSan, a randomized fuzzer and mutation checks; no kernel change, no syscall. (b) After the shell loop can run the scheduler (blocker 1, its own PR): one EL0 test client in QEMU sends valid and invalid messages to the kernel-side compositor; assert the label appears in a screendump inside the client's surface only, invalid messages are rejected with the right codes, a client crash removes its surface and the kernel and shell keep running, and a client cannot draw over the dock or a modal.
* **P2/P3:** to be planned when blockers 2 to 4 have designs of their own; each needs its own security review.

## 13. Recommended next steps

1. Land the pure protocol module with host tests (section 12a). It is independent of every blocker and cheap to review.
2. Design and implement blocker 1 (a scheduler-aware shell loop) as its own kernel PR; measure input latency before and after.
3. Add the minimal request syscall and the P1 EL0 client test.
4. Only then decide on shared pages (blockers 2 and 3).

Nothing above authorises or starts any of these steps by itself.

## 14. Status of phase P1 (updated)

The **pure protocol module** is implemented: `kernel/include/ui_proto.h`, `kernel/ui/ui_proto.c`, tested on the host (`tests/kernel-host/ui_proto_test.c`, ASan/UBSan, plus `mutate_ui_proto.py`). It is linked into the kernel build but **nothing calls it yet**: no syscall, no scheduler integration, no client. Differences from the proposal above, made while implementing it:

* `SET_RECT` is **rejected** when it does not fit the surface (zero size, negative, overflow, beyond the size class), not clamped.
* Sizes are classes (small 256x160, medium 480x320, large 720x420), not arbitrary pixels; text is printable ASCII only; a title and each text are at most 24 and 40 bytes; 16 widgets per surface; at most 4 surfaces per client instance and 8 in total.
* Ownership is `(pid, generation, principal)`; a dead, stale or foreign handle all answer `ESTALE`, so a client cannot tell them apart. A new generation or principal in a pid slot is treated as a new instance and the previous instance's surfaces are dropped first.
* The 16-bit handle serial wraps after 65,535 reuses of one slot (never 0); ownership is still checked on every use.
* Replies: every handled request produces one `STATUS`; at most 2 may be unconsumed, after which further requests are dropped with no reply (`EBUSY`). The kernel must call `uip_reply_consumed` when the client receives a reply and `uip_client_gone` when an instance exits or faults.
* Events (`EVENT_KEY`, `EVENT_POINTER`, `CLOSE_REQUEST`) are built by `uip_route_*`, which return the single intended recipient. Pointer press-grab (delivering a drag to the surface that received the press) is not implemented.
* Not done: the syscall or endpoint that carries these messages, the compositor drawing the visible model, any EL0 client. (The scheduler-aware shell loop, blocker 1, is done: `native-graphics-plan.md` section 15.)

