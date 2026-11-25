# Intent Schema Validation Flow

## Overview

This document describes the end-to-end validation flow for Aura Quick Commands intents, from parser input through broker policy decisions to execution-time verification. The flow ensures that no untrusted intent can cause an action without passing three independent validation layers.

---

## 1. `aura_validate()` — Independent Structural Validation

`aura_validate()` (`kernel/ui/aura_cmd.c:393`) is a pure function that validates a `struct aura_intent` **after** parsing (or from any source, including model output). It shares **no code** with the parser (`aura_parse`) and performs the following checks:

| Check | Description |
|---|---|
| **Version** | `x->version == AURA_INTENT_VERSION` (must be 1) |
| **Enum bounds** | `kind < AK_COUNT`, `status < AS_COUNT`, `reason < AR_COUNT`, `arg_kind <= AA_REL`, `risk <= 3` |
| **Preview integrity** | Preview must be NUL-terminated within `AURA_PREVIEW_MAX` (48) bytes; all bytes must be printable ASCII (0x20–0x7E) |
| **Status/kind/reason agreement** | Each kind/status combination has exact reason expectations (e.g., `AK_SET_SETTING` with `AS_REJECTED` requires `AR_RANGE` for out-of-range text size) |
| **Target/argument ranges** | - `AK_OPEN_SCREEN`: target in [0,5], `arg_kind == AA_NONE`, `arg == 0`; Pages target requires `caps->pages` <br> - `AK_SELECT_SPACE`: target in [0,3], `arg_kind == AA_NONE`, `arg == 0` <br> - `AK_SET_SETTING`: theme target requires `arg_kind == AA_ABS` and `arg in [0,2]`; text scale requires `arg in [75,200]` or `AA_REL ±1` <br> - `AK_TIMER`: requires `caps->timer`; arg range [1, 86400] seconds |
| **Risk/class consistency** | `AK_OPEN_SCREEN`/`AK_SELECT_SPACE` → risk 0; `AK_SET_SETTING` → risk 1; `AK_DESTRUCTIVE` → risk 3; `AK_UNSUPPORTED` → risk matches reason; etc. |
| **Canonical preview** | Preview text must match a kind-specific constant string (e.g., `AK_OPEN_SCREEN` target 0 → `"Open Home"`, `AK_SET_SETTING` theme auto → `"Change theme to Auto"`, text size 100 → `"Set text size to 100%"`) |

**Returns:** `1` if the intent is well-formed and consistent, `0` otherwise.

**Key security property:** This validator can be called on **any** intent, not just those from the parser. The future model proposer must produce intents that pass `aura_validate()`, just as typed input must.

---

## 2. `bp_decide()` — Broker Policy Re-validation

The Action Broker (`kernel/ui/broker_policy.c:45`) re-validates every intent before making a decision. At the top of `bp_decide()`:

```c
if (req->intent.version != AURA_INTENT_VERSION || !aura_validate(&req->intent, env->caps)) {
    res(out, BP_DENY, BPR_BAD_INTENT, 255, 0);
    return;
}
```

After validation, the broker:

1. **Looks up the requester** — verifies PID, process generation, principal, and rights from the kernel's tables
2. **Checks replay freshness** — via the nonce-based replay table (`replay_take`) to prevent reuse
3. **Makes a decision** based on `intent.status`:
   - `AS_ACTIONABLE` — only S0 navigation (`risk == 0` and `kind` is open/select) is allowed; requires `BP_RIGHT_NAVIGATE` and `nav_service`
   - `AS_NEEDS_APPROVAL` — S1 settings changes require `BP_RIGHT_SETTINGS`, a `review_path`, and a `trail`; decision is `BP_REVIEW`
   - `AS_UNSUPPORTED` — understood but no native service; decision is `BP_UNAVAILABLE`
   - `AS_REJECTED` — invalid or out-of-range; for `AK_DESTRUCTIVE` requires passport, trail, review_path, and hold_input → decision `BP_HOLD`; otherwise `BP_DENY`

**Security property:** The broker never trusts a field the validator did not check. The risk class is re-derived from the kind and cross-checked against the status.

---

## 3. `rv_verify()` — Execution-Time Re-validation (Digest Binding)

`rv_verify()` (`kernel/ui/review.c:106`) is the execution-time guard for S1 approved changes. It sits between the trusted review screen and the actual effect:

1. **State check:** Review must be in `RV_APPROVED` state with a matching ticket
2. **Intent re-validation:** Calls `s1_ok(now_q, caps)` which internally calls `aura_validate()` to confirm the intent is still a valid S1 settings change
3. **Digest recomputation & compare:** Computes `rv_digest(now_q)` — an FNV-1a 64-bit hash over **every field**:
   - All 48 preview bytes (including trailing bytes after NUL)
   - Intent fields: version, kind, status, reason, risk, target, arg_kind, arg
   - Requester: pid (4B), gen (8B), principal (4B), nonce (8B)
   - Prior/next values (4B each)
   - Compares against the stored digest from when the review was opened
4. **Outcome:**
   - **Digest matches** → retires ticket to `RV_DONE` (the approval is consumed; the effect may run)
   - **Digest mismatch** → retires to `RV_DECLINED` (a changed intent, different requester, or stale generation — nothing runs)
   - **Second call** → `RV_E_STATE` (the approval has already been consumed)

**Security property:** The approval is cryptographically bound to the exact intent byte-for-byte at review time. Any modification to the intent between review and execution causes `rv_verify` to refuse. This prevents a "confused deputy" or "substitution attack" where the intent is altered after the user approves it.

---

## 4. Full Validation Flow

```
parser → aura_parse() → aura_validate() → broker policy (bp_decide()) → 
  if AS_ACTIONABLE: immediate execution (S0 navigation)
  if AS_NEEDS_APPROVAL: trusted review → user approves → rv_verify() at execution → execution
```

### Detailed Sequence

| Step | Function | Purpose |
|---|---|---|
| **1** | `aura_parse()` | Tokenizes and parses ASCII command into `struct aura_intent`. Sets initial `kind`, `status`, `reason`, `risk`, `target`, `arg_kind`, `arg`, and `preview`. Rejects injection, destructive, unsupported, and multi-command input. |
| **2** | `aura_validate()` | Independent structural validation (see Section 1). Runs on **every** intent before any action. |
| **3** | `bp_decide()` | Broker policy re-validates via `aura_validate()`, checks requester identity, replay, and risk/rights. For S0 → allows immediate navigation. For S1 → routes to trusted review. For destructive → requires passport/trail/hold. |
| **4** | (S1 only) Trusted review screen | User sees the `preview` text (constants only, no user text) and presses the secure-attention gesture. |
| **5** | `rv_open()` | Opens a review ticket, computes `rv_digest()`, stores it, state → `RV_OPEN`. |
| **6** | `rv_attention()` / `rv_approve()` | User arms then approves the review. State → `RV_APPROVED`, ticket consumed. |
| **7** | `rv_verify()` | **At execution time**: re-validates intent via `aura_validate()`, recomputes digest, compares to stored digest. If match → `RV_DONE` (effect runs). If mismatch → `RV_DECLINED` (effect blocked, ticket retired). |
| **8** | Execution | The approved intent effect runs (navigation, setting change, etc.). |

---

## 5. Security Properties

| Property | Guarantee |
|---|---|
| **Intent cannot be modified between validation and execution** | `rv_verify()` recomputes the digest from values read **immediately** before execution. Any byte change (intent field, preview, requester pid/gen/principal/nonce, prior/next values) causes `RV_E_CHANGED`. |
| **Schema-validated before any action** | Both `aura_validate()` (parser path) and `aura_validate()` re-check (broker path, `rv_verify` path) must pass before any effect is allowed. |
| **No model runtime yet** | Validation is structural (syntax, enum bounds, range checks, canonical previews), not semantic. The parser and validator do not understand the "meaning" of commands beyond the fixed grammar; they only ensure the intent conforms to the schema. A future Qwen3-0.6B proposer must produce intents that pass `aura_validate()` and the broker, just like typed input. |
| **Limits enforced** | 64-byte input limit, 48-byte preview, printable ASCII only, argument ranges (75–200 for text size, 1–86400 for timer), no syscall/path/address fields in the intent format. |

---

## 6. Limitations

- **No semantic understanding:** The parser and validator know the grammar (verbs, screens, settings, timers, destructive words, inject word patterns) but do not understand the real-world effect of commands. They only ensure the intent structure is valid.
- **No Action Broker or Passport yet:** `bp_decide()` references `env->passport`, `env->trail`, `env->review_path`, `env->hold_input`, `env->nav_service`, `env->timer_service` which are currently NULL/absent. S1+ effects cannot actually run.
- **No model integration:** The system has no language model runtime. The `aura_validate()` function is the contract that any future model proposer must satisfy.
- **English-only ASCII grammar:** The parser accepts only printable ASCII, case-insensitive command matching, no fuzzy matching or typo correction.

---

## 7. QEMU Test Strategy

### `aura_cmd_test.c` Coverage

Test the parser and validator exhaustively:

- **169 hand-written expectations**: exact matches, synonyms, case, whitespace, politeness, ambiguity, missing arguments, unsupported, destructive, hostile, prompt-injection-style, conflicting, unknown, boundary values
- **Limits and encodings**: 64 vs 65 bytes, 16 vs 17-character words, 12 vs 13 words, UTF-8, tab, newline, DEL, embedded NUL, NULL input
- **1000 repetitions**: byte-identical outputs for same spellings; different spellings give identical intents
- **300,000 random word salads and byte strings**: every output validates independently; exact-size heap input with no terminator (ASan error check); no accepted text size outside 75–200; only S0 navigation ever actionable
- **Mutation check** (`mutate_aura_cmd.py`): 36 mutants, 35 caught. Documented edge cases where mutants survived due to test gaps

### `broker_policy_test.c` Coverage

Test broker decision logic:

- Valid intent → `BP_ALLOW_NOW` for S0 navigation with rights
- Valid S1 intent → `BP_REVIEW`
- Invalid intent → `BP_DENY` with `BPR_BAD_INTENT`
- Bad requester → `BP_DENY` with `BPR_BAD_REQUESTER` / `BPR_STALE_REQUESTER`
- Replay prevention → nonce-based rejection
- Destructive intent → `BP_HOLD` (requires passport/trail/review_path/hold_input)
- Unsupported intent → `BP_UNAVAILABLE`
- Timer intent with rights check (`BP_SHELL_PID`, `BP_RIGHT_TIMER`)