# Visible Listening-State Microphone Indicator — Design (AUDIO-08)

Status: **Design documentation only.** No implementation. Describes a visible, always-on-when-listening microphone indicator owned by the compositor, following AuroraOS's UI architecture (`kernel/ui/shell_run.c`, `kernel/ui/ui_sys.c`).

---

## 1. Design goals

- **Always visible when the microphone is active.** No EL0 process can hide or suppress the indicator.
- **Compositor-owned.** Only the compositor (kernel UI) can show or hide the indicator. Clients cannot draw over or remove it.
- **Cannot be spoofed.** A malicious client cannot fake the indicator to trick the user into thinking the mic is off when it is on (or vice versa).
- **Tied to microphone permission.** The indicator is on if and only if at least one process holds an active `AUDIO_CAP_MIC` grant (AUDIO-07).

## 2. Visual design

- A small **microphone glyph** (or dot) rendered in the shell's chrome band (header/dock area), outside the client surface region.
- Color: **amber/red** when active, absent (not drawn) when inactive.
- Position: fixed in the compositor-controlled region, never inside a client surface.
- Animation (optional): subtle pulse while actively capturing, solid when armed but silent.

## 3. Compositor ownership (kernel UI)

The indicator is drawn by `shell_run.c` / `ui_sys.c` as part of the shell's own chrome:

```
shell_draw(&SH, s) {
    ... draw home, dock, header ...
    if (mic_active()) draw_mic_indicator(s);   /* compositor code only */
    ... draw client surfaces inside content band ...
}
```

`mic_active()` reads the compositor's microphone grant table. No syscall from EL0 can change this — only the compositor grants/revokes `AUDIO_CAP_MIC`.

## 4. Client surfaces cannot cover it

The compositor defines the client surface region (as in `shell_boot_draw`):

```c
ui_sys_init(40, 96 + UIP_CHROME_H, 944, 520 - UIP_CHROME_H);
/* client surfaces may occupy: inside the content band, clear of the header and dock */
```

The microphone indicator is drawn in the header/chrome band, **outside** the client surface region. Clients cannot draw over it because their surfaces are clipped to the content band.

## 5. State transitions

| Event | Indicator state | Action |
|---|---|---|
| No mic grants | Off | Not drawn |
| First `AUDIO_CAP_MIC` grant | On | Draw indicator, begin pulse |
| Last `AUDIO_CAP_MIC` revoked | Off | Remove indicator |
| User revokes via UI | Off | Revoke all grants, remove indicator |
| Process exits | Re-evaluate | Revoke that process's grant; if none remain, turn off |

## 6. Integration with existing UI

- Follows the same pattern as `shell_review_open()` / `shell_review_close()` — compositor-owned modal state that clients cannot bypass.
- Shares the shell's `SH.dirty` / `present()` redraw mechanism: when mic state changes, `SH.dirty = 1` triggers a repaint.
- Uses `console_puts("shell: mic ...")` lines for QEMU test observability (consistent with existing shell logging).

## 7. Security properties

- **No silent mic activation:** Every mic activation requires a compositor grant, which shows the indicator.
- **No indicator spoofing:** Clients cannot draw or remove the indicator; it is compositor chrome.
- **No grant without indicator:** `AUDIO_CAP_MIC` grant always implies indicator visible.
- **Revocation on exit:** `proc_destroy()` and shell `reap()` revoke mic grants, so a crashed or exited process cannot keep the mic logically active.
