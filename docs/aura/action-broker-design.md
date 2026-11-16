# Native Action Broker: architecture (issue #10 and #11 dependency)

Status: **the pure policy function (`kernel/ui/broker_policy.c`, `bp_decide`) is implemented and host-tested (Cycle 18); everything else here is still design.** The policy executes nothing; no executor, review screen, Passport or Trail exists. It describes the smallest secure native service that can take the typed intents of `quick-commands.md` and turn them into effects, and it says plainly which parts cannot exist until native **Passport** and **Trail** (issue #11) do. Where a dependency is missing the answer is "unavailable", never a weaker improvised substitute. Written against `main` at `6c76294`. QEMU only; nothing here says anything about real hardware.

## 1. Principles (these come from DESIGN.md and the existing code, not from this document)

1. **A proposer is not an executor.** The parser, and later Qwen3-0.6B, only produce `struct aura_intent` values. Neither can run anything, hold a capability, or choose an identity. Text from a model is untrusted data exactly like typed input; `aura_validate()` re-checks every intent before the broker sees it.
2. **A UI click is not authorisation.** Pressing a button, answering a prompt or being focused authorises nothing by itself (`shell.h`). Only the broker's decision, made from kernel-assigned identity, capability and risk class, plus the documented trusted approval, permits an effect.
3. **Fail closed.** A missing service, an unknown intent, an unsupported domain, a failed check, a crash or a timeout all mean "nothing happened". Nothing is ever reported as done that was not.
4. **Least authority.** The broker holds only the rights it needs for the actions it supports. It does not hold filesystem authority over other principals, process control, or the framebuffer.
5. **Risk classes are fixed by DESIGN.md:** S0 simple view/open (one safe step may run immediately), S1 reversible settings (plan preview), S2 file modifications (explicit review), S3 delete/send/install/share (tactile hold confirmation, never voice-only).

## 2. What exists today and what does not (verified in the code)

| Exists | Where |
|---|---|
| Typed intent, deterministic parser, independent validator | `kernel/ui/aura_cmd.c`, `quick-commands.md` |
| Kernel-assigned process identity (`pid`, `gen`, `principal`); principals set by kernel code only | `proc.h`, `launch.h` |
| A typed settings registry with capabilities (`CAP_SET_APPEARANCE`, `_ATTENTION`, `_SYSTEM`) and persistence | `kernel/settings.c`, `settings.md` |
| A compositor that owns focus and modal layers, and withholds input from clients while a modal is open | `kernel/ui/shell*.c`, `native-graphics-plan.md` sections 15, 16 |
| A bounded UI protocol and a syscall for EL0 clients (`SYS_UI`) | `ui_proto.c`, `ui_sys.c` |

| Does **not** exist | Consequence |
|---|---|
| Passport (permissions, access log, per-action approval) | no durable record of what a principal may do; no approval ledger |
| Trail (change receipts, version history, recovery) | no receipts, so **no honest Undo**; no durable result log |
| A trusted approval screen with a non-spoofable input path (secure attention) | an S1+ review would be a client-imitable prompt |
| Tactile hold-to-confirm input | S3 cannot be confirmed as DESIGN.md requires |
| A broker service, or a syscall to reach one | nothing receives intents |
| Timer, Notes, messaging, brightness services | those intents are unsupported (the parser already says so) |

## 3. Components

```
 text / voice / model ──► [proposer]  aura_parse()  ──►  struct aura_intent        (untrusted data)
                                                           │  aura_validate()  (independent re-check)
 requester (EL0 app or the shell) ── kernel-attested (pid, gen, principal) ──┐
                                                           ▼                  ▼
                                   [policy]  decide(intent, requester, caps, risk)  ──► DENY | ALLOW_NOW | REVIEW | HOLD | UNAVAILABLE
                                                           │
                          S0 navigation ◄─ ALLOW_NOW       │ REVIEW / HOLD
                                                           ▼
                              [trusted review]  compositor-drawn modal, kernel-named requester, preview from constants
                                                           │ approved by the user, on the trusted path only
                                                           ▼
                                   [executor]  one handler per intent kind, each with its own narrow authority
                                                           │
                                   [recorder]  result + receipt  ──► Passport/Trail (BLOCKED, see section 7)
```

* **Policy** is a pure function: `(validated intent, requester identity, platform capabilities) -> decision`. It is host-testable like the parser. The decision table: unsupported or unknown -> `DENY`/`UNAVAILABLE`; `AS_ACTIONABLE` S0 navigation -> `ALLOW_NOW` *only if the requester holds the UI-navigation right*; S1 settings -> `REVIEW`; S2 -> `REVIEW` (explicit); S3 and install -> `HOLD`; any S2/S3 while Passport or Trail is missing -> `UNAVAILABLE`.
* **Requester identity** comes from the kernel (the same `(pid, gen, principal)` the UI syscall uses), never from the intent. An intent has no identity field and must not gain one.
* **Trusted review** is the compositor's modal layer: the compositor draws it, the requester's name comes from the kernel's launch registry, the preview text comes from the intent's fixed vocabulary, and no client surface can overlap or answer it. Open question carried from the EL0 design: a secure-attention sequence so the user can tell it from an imitation; until it exists the review is only as trustworthy as "clients cannot draw in the modal layer".
* **Executor** handlers are the only code that causes effects, and each calls an existing, already-authorising interface as a principal that holds exactly the matching right. For example a theme change calls the settings registry as the shell principal that holds `CAP_SET_APPEARANCE` and **not** `CAP_SET_SYSTEM`, so the registry itself refuses Aura mode however the intent was produced.
* **Recorder** writes the outcome. Without Trail there is no durable receipt (section 7).

## 4. Supported actions and their path

| Intent | Decision | Executor | Today |
|---|---|---|---|
| open Home/Spaces/Index/Settings, select a Space | `ALLOW_NOW` (S0) if the requester may navigate | shell navigation (in memory) | could be built first: no Passport needed for in-memory navigation, but it must still check the requester |
| open Pages | `ALLOW_NOW` only when a Pages screen exists; else unsupported | shell navigation | Pages is not built |
| theme, text size | `REVIEW` (S1), then the registry write as the shell principal | settings registry | the registry exists; the review screen and the approval path do not |
| delete, format, install, send, share | `HOLD` (S3) | none | **unavailable**: needs hold-to-confirm, Passport and Trail |
| timer, notes, brightness | unsupported | none | no such services |

## 5. Failure, partial failure and Undo

* Every request ends in exactly one of: `done`, `denied` (policy), `declined` (the user said no or the review timed out), `failed` (the executor returned an error, nothing changed), `unavailable` (a dependency is missing). The broker never reports `done` unless the executor's own return value said so.
* A multi-step plan is **not** supported in the first version: one intent, one effect. That removes partial failure by construction; a later plan format must define per-step results and an all-or-nothing or explicit-compensation rule before it exists.
* **Undo** is offered only where a real reversible operation and its prior value were recorded: a settings change can be undone by writing back the recorded previous value, but only a Trail receipt makes that survive a restart and prove what happened. Until Trail exists the broker offers no Undo and says so; it does not keep an in-memory "undo" that vanishes on reboot and looks like a receipt.

## 6. Threat model and tests

| Threat | Mitigation | Test |
|---|---|---|
| A model or text injects an action | the intent type is a closed enum set; the validator re-checks it; policy decides on identity and risk, not on text | host: fuzz `parse -> validate -> decide`; assert no path reaches an effect without `ALLOW_NOW` or an approval |
| A client forges identity or a higher risk class | identity from the kernel; risk comes from the validator's table, not from the intent's claim | host: an intent with the wrong risk is invalid; QEMU: an EL0 client cannot name another principal |
| A client spoofs the review | compositor-only modal layer; kernel-named requester; no client drawing there | existing UI-client isolation suite plus a review-specific case |
| Approval by accident or by a script | S3 needs tactile hold on the trusted path; automation cannot satisfy it | blocked until the trusted input path exists |
| Confused deputy: the broker acts with more authority than the requester has | each handler runs as a principal holding only the matching right; the registry and filesystem still enforce their own checks | QEMU: a requester without the right gets `denied`, an Aura-mode change is refused by the registry |
| Replay of an old approval | an approval is bound to the exact intent bytes and a one-time nonce, consumed on use | host |
| Flooding | bounded queue, one outstanding request per requester, drop with a status | host and QEMU |
| Crash mid-action | the executor is atomic per intent or reports `failed`; nothing is half-applied | fault-injection test per handler |

## 7. Blocked on issue #11, and what is built first

Blocked: **Passport** (the permission ledger and approval record), **Trail** (receipts and recovery), the **secure-attention / trusted input path**, **hold-to-confirm** input, and any S2/S3 execution. Those stay `UNAVAILABLE`. There is no weaker stand-in: no "approve" button that merely sets a flag, and no in-memory receipt.

Safe, independent steps, in order, each its own PR:

1. **Pure policy engine** `decide(intent, requester, caps)` with host tests, a fuzzer and mutation checks (no kernel change).
2. **S0 navigation executor** for the shell, behind a requester check (no Passport needed because nothing durable or privileged changes), with a QEMU test that an intent from a requester without the right is denied.
3. A **review screen** in the compositor for S1, built only after the trusted-input question is decided; until then S1 intents stay `UNAVAILABLE`.
4. Passport and Trail (issue #11), then S1 execution, receipts and Undo.
5. Only after hold-to-confirm exists: S3.

Nothing in this document authorises or starts any of these steps by itself, and none of it changes the status of issue #10 or #11.
