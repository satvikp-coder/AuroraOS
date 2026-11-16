# Native shell UI specification for the next screens (Cycle 18 design pass)

Status: **a design, nothing here is implemented.** Tier 0 Clear, 1024x768, QEMU only. Written by a read-only design review of `kernel/ui/shell.c` (`pal`, `make_geo`, `settings_flow`, `pages_flow`, `aura_flow`, `build_about`), the font ladder and `docs/agents/CYCLE18_PLANS.md`. Contrast ratios were computed by hand (the method reproduces the documented 4.07:1): **confirm them with `ratio()` in `tests/kernel-host/shell_scale_test.c` before relying on them.** The timer, review, hold and ledger code exist only as plans, so Trail record and grant fields here are assumptions.

## 0. Verified layout facts
* Font advance / line height (px): 12 = 7/17, 14 = 8/19, 16 = 10/22, 18 = 11/24, 24 = 14/33; the 2x magnified sizes double both.
* `sl()` label height = lines x lh + 4 (text inset 8). `sb()` button height = max(min, lines x lh + 16).
* `make_geo` at 100%: header 0..64, rail 64..68, `top` 84, `bottom` 648, dock y 664..744. At 200%: header 82, `top` 102, `bottom` 636, dock y 652, `btn_h` 60. High contrast: `top` is 2 px lower.
* New flow screens use `card_s(40, top, 944)`, content x 56, inner width 912, and the `opt` 1-then-0 retry used by `settings_flow`.

## 1. Palette fix (standard mode)
lin = ((v/255 + 0.055)/1.055)^2.4; L = 0.2126R + 0.7152G + 0.0722B; ratio = (Lhi + 0.05)/(Llo + 0.05).

| Token | Old | New | Reason |
|---|---|---|---|
| accent | 3478F6 | **2A68E8** | white/accent 4.07:1 -> **4.95:1** (needs 4.5) |
| aura (status rail, never text) | 00AEB8 | **00838C** | rail vs surface 2.71 -> 4.54, vs canvas 2.43 -> 4.07 (needs 3) |
| ctl (new field: border of ENABLED controls) | (border C9D4E3, 1.50:1) | **6B7C93** | 4.26 vs surface, 3.82 vs canvas (needs 3); cards, dock panel and client bodies keep `border` |
| secure (new field, review strip only) | n/a | **FFB020** | ink on it 8.83:1; needs a 2 px ink border (1.83:1 against the white dialog) |

Unchanged: ink 142136 (16.15 on surface), muted 52627A (6.20 / 5.55 on surface / canvas), focus 175CD3 (5.99 / 5.36), scrim 525A66 (surface/scrim 6.97), canvas EEF3F8, hover, pressed, `border_dis`. High contrast keeps every value; `ctl` = 000000; the secure strip is black with white text; the toast is black with a 2 px white border. This is native-only: the browser prototype tokens (`DESIGN.md` section 3, `aurora-glass/`) are a separate owner decision.

Tests that assert the old colours and must be updated with the change: `tests/kernel-host/shell_test.c` (0x003478F6, 0x0000AEB8 literals), `shell_scale_test.c` (asserts worst standard contrast in [4.0, 4.5): becomes >= 4.5), `tests/qemu/home_check.py` / `scale_check.py` / `ui_check.py` colour constants (scale_check must add 6B7C93 and FFB020 to its palette set or its every-pixel check fails), `kernel/include/ui_scene.h` `UI_COL_ACCENT`, `kernel/ui/shell.c` `COL_AURA`, and the "Known finding" paragraph in `docs/kernel/native-graphics-plan.md`.

## 2. Common rules
* No screen reader exists: each screen has one live text line that changes, and every change also logs `shell: say <text>` for QEMU tests. State is never colour alone.
* All sizes come from a role through `pick_font`; labels wrap; buttons are at least 48 px high; optional lines are dropped at `opt = 0`; nothing is clipped at 75-200%.
* Entry: Index items "Clock" and "Trail"; Home tiles stay untouched (the 100% Home layout is pixel-tested). New ids: `SCR_CLOCK`, `SCR_TRAIL`; SIDs 21-22 review, 250-255 Clock, 260-262 Trail, 270 hold.

## 2a. Clock / Timer (100% rectangles x,y,w,h)
Card 40,84,944,423. "Clock" 56,96,912,37 (24 bold); optional "Time since boot. No wall clock exists." 56,135 (14 muted); "Up h:mm:ss" 56,162,912,28 (18 bold); optional "Timer" 56,198; value "m:ss" 56,223,912,37 (24 bold); live state line 56,262,912,26 (16); presets "1 min" "5 min" "25 min" at x 56/362/668, y 296, 298x48; "Start" 56,352,452,48 and "Cancel" 516,352,452,48; optional "Stops if AuroraOS restarts. No sound." 56,408; "Home" 56,435,200,48.

| State | Value | Live line | Enabled |
|---|---|---|---|
| READY (default 5 min, chosen preset active) | 5:00 | Timer ready. Press Start. | presets, Start |
| RUNNING | remaining | Timer running. | Cancel only |
| DONE | 0:00 | Timer finished. (+ toast) | presets, Start |
| CANCELLED | preset | Timer cancelled. Nothing is running. | presets, Start |
| No service | -:-- | Timer not available: no timer service. | none |
| Quota / failed read-back | preset | Timer not started: no free timer. | presets, Start |

Focus order: 1 min, 5 min, 25 min, Start, Cancel, Home, dock, Held. A disabled focused control hands focus to the next valid one (Start -> Cancel on start; Cancel -> Start on cancel/done). Redraw only when the shown second changes. At 200% the three optional lines are dropped (bottom 594 <= 636).

## 2b. Trusted review modal
Scrim on `UI_LAYER_SCRIM`, dialog on `UI_LAYER_MODAL`, `ui_modal_push`; Cancel is added before Approve so default focus is Cancel. Dialog 192,84,640,281 (radius 20, 2 px focus border). Strip "AuroraOS secure review" 208,100,608,40 (secure fill, 2 px ink border, 16 bold ink). "Requested by: <kernel name>" 208,152; intent preview 208,177 (18 bold); "<Setting>: <current> -> <new>" 208,207 (16); "Reversible. Recorded in Trail." 208,235 (14 muted); live state 208,266 (14 bold); Cancel (SID 21) 208,301,296,48; Approve (SID 22) 520,301,296,48. Theme adds " Stored only: no dark theme."

| State | Live line | Approve | Left button |
|---|---|---|---|
| OPEN | Press Ctrl+Alt+Delete to enable Approve. | disabled, "Approve (locked)" | Cancel |
| ARMED (chord >= 600 ms after open; lasts 10 s) | Approve is enabled for 10 seconds. | enabled | Cancel |
| Arm lapsed | Approve timed out. Press Ctrl+Alt+Delete again. | disabled | Cancel |
| Expired (60 s) | Review expired. Nothing changed. | disabled | Close |
| Requester gone | Requester exited. Nothing changed. | disabled | Close |

Arming never moves focus (the user must Tab to Approve and press Enter or click). Approve keeps the normal button look, not accent, so both choices weigh the same. Esc declines in every state; a click on the scrim does nothing. The chord is a non-repeat `KEY_DELETE` key-down with `MOD_CTRL|MOD_ALT`, handled before clients. With no keyboard the modal never opens (Aura says "Not available: review needs a keyboard."). Width scales to clamp(640 x pct/100, 640, 976), centred, y = `top`; no line is dropped, lines wrap. In high contrast authenticity rests on the fixed string, the scrim and the chord, not on colour.

## 2c. Trail (read-only)
"Trail" 56,96 (24 bold); optional "Read-only. Checksums catch accidents, not tampering."; live health line (16 bold); rows at y = 194 + 23 i (14); "Home". Rows are constants and numbers only: "Changes: N. Denials: N.", up to 8 records newest first ("#41 Text size 100% -> 125%. Done." / ". Started.", "#39 Granted: <name> may use timers.", "#38 Revoked: ...", "#37 System started.", "#N Unknown record kind K."), "Saved permissions: N" and one line per durable grant. If rows do not fit, a Settings-style window with "Show earlier" (SID 260), "Show more" (SID 261) and "Showing a-b of n". Health strings: "Trail: recording to disk." / "Trail: off. No disk, nothing is saved." / "Trail: read-only disk. Changes refused." / "Trail: file unreadable. Changes refused." / "Trail: rollback found. Changes refused." / "Trail: not available." Empty: "No records yet." / "Saved permissions: none." No control other than scroll and Home; no syscall path.

## 2d. Hold-to-confirm (demo; `env.hold_input` stays 0)
Button "Hold to confirm" (SID 270) x0,y0,400,48; four segments at x0 + 102k, y0 + 56, 94x12 (empty = canvas fill with ctl border, filled = ink); live status at y0 + 72. Segment k fills at 375 x (k+1) ms; at 1500 ms, still held, it confirms exactly once (a later release does nothing). Status: "Hold for 1.5 seconds to confirm." / "Holding: n of 4" / "Confirmed." / "Released early. Nothing happened." / "Hold to confirm is not available." Keyboard: a non-repeat Enter or Space key-down on the focused control starts it; cancels on that key's key-up, any other key, Tab/focus change, Esc or a modal change. Pointer: press inside starts; release or leaving the rectangle cancels and re-entering does not resume. A release processed before the poll that would complete the hold cancels (fail closed). Note: `ui_key` activates on key-down and `ui_focus` ignores key-up, so intercept this control before `ui_key`, as `index_input` does.

## 2e. Toast
"Timer finished", 16 bold white on ink, 2 px surface border, radius 12, `UI_LABEL`, visible only (never focusable); drawn over the Horizon status slot (100%: 182,10,208,48; 200%: 272,11,384,60, clear of the uptime label); stays inside the header so it never overlaps dock, content, sheet or the review dialog; shown 4000 ms on the monotonic counter, one slot, a new notice restarts it.

## 3. Acceptance checklist (turn into host tests and QEMU pixel checks)
Every screen runs the `shell_scale_test.c` audit over 75..200% and both palettes: inside 1024x768 and the rail..dock band, wrap-oracle fit, focus targets >= 48x40, no overlap, text >= 4.5:1 (standard) and 7:1 (high contrast).
* Palette: `ratio()` white/accent >= 4.5, aura and ctl >= 3 on surface and canvas, ink/secure >= 4.5; enabled button border == `ctl`, card border == `border`; QEMU px(500,65) == 00838C.
* Clock: the rectangles above; Tab order 250..253 then 255 in READY (Cancel skipped), only 254 then 255 in RUNNING; Start moves focus to Cancel; no-service state has no enabled control but Home; 200% bottom <= 636.
* Review: focus id 21 on open; Approve not focusable in OPEN and Enter never approves from default focus; a chord before 600 ms is a no-op; after arming focus is unchanged; 10 s later Approve is disabled; Esc declines everywhere; the secure colour is used only by `build_review` (QEMU: zero FFB020 pixels with the review closed; the chord never reaches a client log).
* Trail: 14 rows fit, 17 rows scroll; each health string; unknown kind row; QEMU: Enter/Space on rows changes nothing.
* Hold: confirms at 1500 ms and never at 1499; release, other key, Tab, Esc, pointer exit and modal open each cancel with zero confirmations; 1000 repeats never confirm; at most one confirmation per hold; filled segments == floor(elapsed/375).
* Toast: rectangle inside the header and disjoint from uptime, Held, dock, dialog and sheet at every scale; not focusable; gone at 4000 ms.

## 4. Pre-existing findings to decide on
* At 100% high contrast the static header draws a 4 px rail while `make_geo` assumes 6, leaving a 2 px gap above flow screens.
* `make_geo` computes a dock width of 502 where the static dock is 500 (harmless).
* `COL_DISABLED_FILL` in `shell.c` is defined but unused.
