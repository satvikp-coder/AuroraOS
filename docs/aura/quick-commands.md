# Aura Quick Commands: the deterministic parser and the typed intent (issue #10 foundation)

The native Aura screen sends validated commands to the Action Broker. It supports navigation, opening installed apps, timers with explicit units, and supported settings changes with trusted review. Issue #10 tracks the remaining command families. The sections below also preserve the earlier parser and Cycle 18 milestones.

Code: `kernel/include/aura_cmd.h`, `kernel/ui/aura_cmd.c`. Tests: `tests/kernel-host/aura_cmd_test.c` (ASan/UBSan), `tests/kernel-host/mutate_aura_cmd.py` (mutation check).

From the native Aura screen, `open Trail` or `show activity` opens the
read-only Trail view through the Action Broker. This command is reserved for
the trusted shell. Application navigation rights do not grant access to Trail,
and an active trusted review keeps control of the screen.

## What it is, and what it is not

A pure function from at most 64 bytes of ASCII to one `struct aura_intent` made only of enumerations and small validated numbers. It has no state, no clock, no allocation and no libc. It does **not** authorise anything, does not execute anything, and never reports an unsupported request as done. The `preview` text is built from constants and validated numbers; it never contains the user's words, so a hostile string cannot reach a review screen through it.

A parsed intent is **data** that a future broker must check: who is asking (a kernel-assigned identity, never a field in the intent), the risk class, the capability, and the documented trusted approval. `AS_ACTIONABLE` means only "this is an S0 navigation that policy allows without a review"; running it is still the broker's decision.

## The intent (schema version 1)

| Field | Meaning |
|---|---|
| `version` | 1 |
| `kind` | `open_screen`, `select_space`, `set_setting`, `unsupported`, `destructive`, `clarify`, `unknown`, `refused` |
| `status` | `actionable` (S0 navigation), `needs_approval` (S1 or higher), `unsupported`, `clarify`, `rejected` |
| `reason` | an enumerated code, never free text: `empty`, `too_long`, `bad_encoding`, `injection`, `multiple`, `ambiguous`, `missing_arg`, `range`, `not_understood`, `no_timer`, `no_hardware`, `no_notes`, `no_messaging`, `no_pages`, `needs_broker` |
| `risk` | 0 to 3 = S0 to S3 (DESIGN.md): the class the action *would* have |
| `target` | a screen (Home, Spaces, Index, Settings, Pages), a built-in Space 0 to 3, or a registry setting id (1 theme, 2 text scale) |
| `arg_kind`, `arg` | for a setting: an absolute value (theme 0 auto, 1 light, 2 dark; text size 75 to 200) or a relative step (-1, +1) |
| `preview` | at most 47 ASCII bytes from a fixed vocabulary |

There is **no** field for a syscall number, a path, an address, a principal, a shell command or a free-form string. That absence is the point.

## What it recognises

| Command (examples) | Result | Risk |
|---|---|---|
| open / go to / show / launch / take me to / switch to / navigate to / view + home, spaces, index (search, launcher), settings (preferences, options) | `open_screen`, actionable | S0 |
| the same for Pages (processes) | actionable **only if the platform reports a Pages screen**; otherwise `unsupported` (`no_pages`) | S0 |
| switch to / select / use / open + study, work, projects, personal (with or without "space") | `select_space`, actionable (the choice is in memory only, no isolation) | S0 |
| set theme to dark/light/auto; dark mode; turn on light mode | `set_setting` theme, **needs approval** | S1 |
| set text size to 75..200; make text bigger / smaller | `set_setting` text scale, **needs approval**; out of range is `rejected` (`range`) | S1 |
| timers, alarms, stopwatch, reminders | `unsupported` (`no_timer`) | S0 |
| brightness, volume, Wi-Fi, Bluetooth, battery | `unsupported` (`no_hardware`) | S1 |
| notes, memo | `unsupported` (`no_notes`) | S0 |
| send, message, email, call, share, post | `unsupported` (`no_messaging`) | S3 |
| delete, remove, erase, wipe, format, uninstall, install, download | `destructive`, **rejected** (`needs_broker`) | S3 |

Politeness words around the command ("please", "can you", "hey Aura", "thanks") are ignored. Matching is case-insensitive and whitespace is collapsed.

## Strictness (why it is safe in front of a broker)

* The **whole** input must be one command. Leftover words make it `unknown`; nothing is partially executed.
* One command at a time: "and", "then", "also", ";" and "&" make it `clarify` (`multiple`). A request that also contains a destructive word is refused as destructive first, so "open settings and delete everything" is never half-run.
* Text that tries to instruct the assistant ("ignore previous instructions", "you are now root", "system prompt", "act as", "sudo", "admin", "password"), names an address (`0x...`), or contains a path or shell metacharacter (`/ \ | $ ` < >`) is **refused as a whole**, even if it also contains a valid command.
* Ambiguous prefixes ("open s") and missing arguments ("theme", "open", "text size") are `clarify`. Unknown targets are `unknown`.
* Limits: 64 bytes, 12 words of at most 16 characters; printable ASCII only. Longer or non-ASCII input is **rejected, never truncated or transliterated**. A number with more than 3 digits is out of range, so a wrapped 32-bit value can never become a valid text size (tested: 4294967396).

## The validator

`aura_validate()` shares no parsing code with the parser. It checks version, enums, target and argument ranges, the exact risk of each kind, that `status`, `kind` and `reason` agree, the platform capability (an `open_screen` Pages intent is invalid without a Pages screen), and that the preview equals the canonical text. It must be run on **every** intent before it is acted on, including intents that did not come from the parser: the future model will produce intents too, and its output is untrusted exactly like typed input. In the tests it rejects every hand-made bad intent (a setting change marked actionable, a deletion marked approvable, text size 201, a wrong preview, an unterminated or non-ASCII preview, ...).

## Evidence

* 169 hand-written expectations (exact, synonyms, case, whitespace, politeness, ambiguity, missing arguments, unsupported, destructive, hostile, prompt-injection-style, conflicting, unknown, boundary values).
* Limits and encodings: 64 vs 65 bytes, 16 vs 17-character words, 12 vs 13 words, UTF-8, tab, newline, DEL, an embedded NUL, NULL input.
* 1000 repetitions interleaved with other commands are byte-identical; different spellings of one command give identical intents.
* 300,000 random word salads and byte strings: every output validates independently; an **exact-size heap input with no terminator** (so a read past the length is an ASan error); outputs are deterministic; no accepted text size is outside 75..200; only S0 navigation is ever actionable.
* Mutation check (`mutate_aura_cmd.py`): 36 mutants, 35 caught. The first run left 7 survivors; 5 were real test gaps (each injection detector was only tested together with another, a 32-bit wrap of a long number, a 65-byte input rejected for another reason, a validator range check masked by a preview mismatch) and now have tests; one was dead code (removed); one remains as a documented equivalent mutant (the generic "risk > 3" guard in the validator is unreachable because every kind checks its exact risk).
* One real defect found while testing: the destructive-request preview was 54 characters and was silently truncated to the 47-character buffer; the independent validator rejected the parser's own output. The text is now shorter.

## What is not done (and blocks closing #10)

* No Action Broker, no Passport, no Trail, no trusted approval or hold-to-confirm screen, no receipt, no undo. S1 intents say they need approval; nothing can grant it yet.
* No executor: nothing turns an `actionable` intent into a navigation. No native preview screen.
* No timer, notes, messaging, brightness or Notes services exist, so those commands are reported unsupported. The `timer`, `notes`, `messaging` and `hardware` fields of `struct aura_caps` are reserved: schema version 1 has no intent for those domains, so they are always `unsupported` whatever the platform reports.
* The grammar is English only, ASCII only, and covers a deliberately small set of commands. It is not fuzzy and does not correct typos (a typo is `unknown`).
* No model: the vocabulary and the intent are the contract a future Qwen3-0.6B proposer must fit, and its proposals must pass `aura_validate()` and the broker like any other input.

## Native Aura screen and S0 execution (Cycle 18)

The parser is no longer data-only. The native **Aura** screen (Home tile, Index, dock Back history) takes a typed command (printable ASCII, 64 bytes), shows the parser's constant preview live, and on Enter or Run sends it through the Action Broker as the trusted shell: `aura_parse` -> `aura_validate` -> `bp_decide` -> the narrow S0 executor -> a **verified** result. What works: open home / spaces / index / settings / pages and switch to the study, work, projects or personal Space. Everything else answers honestly and changes nothing: timers, brightness, Notes, messaging ("unsupported"), deletion/install ("Not done: needs broker and trusted approval"), prompt injection, unclear or multi-command input, and S1 settings requests ("Needs approval, and the trusted review is not available yet"; the text size is never changed). A done command is consumed; a refused one stays in the field so it can be corrected. No language model is involved. EL0 clients can ask the broker through `SYS_UI` op 3 only if kernel code granted them the navigate right; their request is queued and executed by the shell only if the requester is still the same live instance. Trusted approval, Passport, Trail, hold-to-confirm and every S1+ effect remain unavailable.
