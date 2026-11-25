# Native EL0 Application Implementation Pattern (AuroraOS)

This document describes the standard pattern for implementing native EL0 applications in AuroraOS, using the **Clock** app as the reference implementation.

## Overview

Native EL0 applications in AuroraOS are:
- **Unprivileged EL0 programs** that run under a kernel-chosen principal (identity)
- **Launched exclusively by the kernel** via the trusted launcher (`launch_register` / `launch_app`)
- **Installed on AuroraFS** as installer-owned, CRC-verified, read-only ELF executables
- **Confined by the UI protocol** (surface quota, kernel placement, focus rules) — they cannot trap Esc/Tab
- **One instance per app** — a second launch while running is refused

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                         KERNEL                                  │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────────────┐  │
│  │ apps.c       │  │ launch.c     │  │ shell.c (launcher)   │  │
│  │ (registry)   │──│ (loader)     │──│ (Index → apps_launch)│  │
│  └──────────────┘  └──────────────┘  └──────────────────────┘  │
│         │                │                     │                │
│         ▼                ▼                     ▼                │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │  AuroraFS: installer-owned *.elf (CRC-verified, RO)    │   │
│  └─────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼ launch_app()
┌─────────────────────────────────────────────────────────────────┐
│                         EL0 PROCESS                             │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────────────┐  │
│  │ SYS_UI       │  │ SYS_TIMER    │  │ SYS_FS               │  │
│  │ (surfaces)   │  │ (timers)     │  │ (private files)      │  │
│  └──────────────┘  └──────────────┘  └──────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
```

## Step-by-Step Implementation Pattern

### 1. Define the App in the Kernel Registry

**File:** `kernel/include/apps.h`
```c
enum { APP_NONE = 0, APP_CALCULATOR = 1, APP_NOTES = 2, APP_FILES = 3, APP_CLOCK = 4, APP_COUNT };
```

**File:** `kernel/proc/apps.c`
```c
static const struct appdef DEFS[] = {
    { APP_CALCULATOR, "calculator", "Calculator", "calc.elf", 601 },
    { APP_NOTES, "notes", "Notes", "notes.elf", 602 },
    { APP_FILES, "files", "Files", "files.elf", 603 },
    { APP_CLOCK, "clock", "Clock", "clock.elf", 604 },  // ← New app
};
```

Each app gets:
- **Stable APP_* ID** (fixed in kernel code)
- **Key** (Index search term, e.g., "clock")
- **Label** (display name, e.g., "Clock")
- **Executable filename** (must match installed `.elf` on AuroraFS)
- **Principal** (kernel-assigned identity, e.g., 604 — must be `principal_assignable`)

### 2. Grant Capabilities at Boot

**File:** `kernel/proc/apps.c` — `apps_init()`
```c
void apps_init(void)
{
    for (unsigned i = 0; i < sizeof DEFS / sizeof DEFS[0]; i++) {
        const struct appdef *d = &DEFS[i];
        int rc = launch_register(d->key, d->file, d->principal, 0);
        registered[d->id] = (rc == LAUNCH_OK);
        int g = ui_sys_grant(d->principal);           // UI capability (all apps)
        int t = 0;
        if (d->principal == 604) t = timersys_grant(d->principal); // Timer capability (Clock only)
        // ... logging ...
    }
}
```

**Capabilities are granted by kernel code only:**
- `ui_sys_grant(principal)` — UI surface protocol (required for all apps)
- `timersys_grant(principal)` — SYS_TIMER system call (Clock app)
- `settings_grant(principal, CAP_*)` — Settings registry (Settings app, future)
- No system call can grant capabilities; EL0 programs cannot request them.

### 3. Register in Package Build

**File:** `tools/pkg/apps.list`
```
calc.elf tests/programs/calc.c
notes.elf tests/programs/notes.c
files.elf tests/programs/files.c
clock.elf tests/programs/clock.c   # ← New app
```

The packager (`scripts/package-qemu.sh`):
- Builds each `.c` with `aarch64-linux-gnu-gcc` (freestanding, no libc)
- Links against `tests/programs/user.ld` (EL0 entry point at `_start`)
- Validates executable names match `apps.c` registry both ways
- Installs on AuroraFS as installer-owned, read-only, CRC-verified

### 4. Add Index Entry for Shell Launch

**File:** `kernel/ui/shell.c` — `ITEMS[]` and `DEST[]`
```c
static const struct idx_item ITEMS[] = {
    // ...
    { "clock", "Clock", { "timer", "countdown", "uptime" }, 1 },  // is_app = 1
    // ...
};
static const struct dest DEST[] = {
    // ...
    { D_APP, 4 },  // ← APP_CLOCK = 4, not D_SCREEN
    // ...
};
```

- `D_APP` with arg = `APP_CLOCK` routes launch through `apps_launch(4)`
- `is_app = 1` in `ITEMS` marks it as availability-dependent (shown "(not available yet)" if not installed)
- Search keywords ("timer", "countdown", "uptime") enable Index discovery

### 5. Implement the EL0 Application

**File:** `tests/programs/clock.c`

#### 5.1 System Call Wrappers
```c
#define SYS_UI 41
#define SYS_TIMER 42
#define TIMER_START 1
#define TIMER_CANCEL 2
#define TIMER_QUERY 3
#define TIMER_RELEASE 4
#define TIMER_NOW 5

static i64 tm(i64 op, i64 a, i64 b) { return sc(SYS_TIMER, op, a, b, 0, 0); }
static i64 ui_send(const void *m, u64 len, void *r, u64 rcap) { return sc(SYS_UI, 1, (i64)m, (i64)len, (i64)r, (i64)rcap); }
static i64 ui_poll(void *buf, u64 cap) { return sc(SYS_UI, 2, (i64)buf, (i64)cap, 0, 0); }
```

#### 5.2 UI Surface Protocol
Create a surface (type 1 = CREATE), add rectangles (type 3 = RECT), add text (type 2 = TEXT), commit (type 4 = COMMIT):
```c
static void hdr(u8 *b, u8 type, u16 seq, u32 handle) { b[0] = 1; b[1] = type; put16(b + 2, seq); put32(b + 4, handle); }

static i64 send_rect(u8 w, u8 role, int x, int y, int ww, int hh) { /* ... */ }
static i64 send_text(u8 w, u8 style, const char *t) { /* ... */ }
static i64 commit(void) { /* ... */ }
```

#### 5.3 Application Logic
```c
// State
static int timer_id = -1;
static uint8_t timer_state = 0;  // 0=READY, 1=RUNNING, 2=DONE, 3=CANCELLED, 4=ERROR
static uint8_t timer_preset = 0;

// Update display from timer service
static void clk_update_live(void) {
    i64 now = tm(TIMER_NOW, 0, 0);  // monotonic ms
    // Query running timer for remaining time
    if (timer_state == 1) {
        u8 st = 0; u64 rem = 0;
        if (query(timer_id, &st, &rem) == 0) {
            if (st == 2) timer_state = 2;  // completed
            else if (st == 3) timer_state = 3;  // cancelled
        }
    }
    // Format and send text to UI widgets
    send_text(1, 0, uptime_str);
    send_text(2, 1, remaining_str);
    send_text(3, 0, status_str);
    commit();
}

// Handle keys
static int on_key(u32 code, u32 mods) {
    switch (code) {
    case 2: timer_preset = 0; refresh(); return 1;  // '1' key
    case 3: timer_preset = 1; refresh(); return 1;  // '2' key
    case 4: timer_preset = 2; refresh(); return 1;  // '3' key
    case 31:  // 'S' key - Start
        if (timer_state == 0 || timer_state >= 2) {
            i64 id = tm(TIMER_START, PRESET_MS[timer_preset], 0);
            if (id >= 0) { timer_id = id; timer_state = 1; }
            else timer_state = 4;
            refresh();
        }
        return 1;
    case 46:  // 'C' key - Cancel
        if (timer_state == 1) {
            if (tm(TIMER_CANCEL, timer_id, 0) == 0) {
                tm(TIMER_RELEASE, timer_id, 0);
                timer_state = 3; timer_id = -1;
            } else timer_state = 4;
            refresh();
        }
        return 1;
    case 16: return 2;  // 'Q' key - Quit
    }
    return 0;
}

// Event loop
void __attribute__((section(".text._start"))) _start(u64 mode) {
    // 1. Create surface
    hdr(b, 1, ++useq, 0); b[8] = 2; b[9] = n; /* size class 2 = large */
    ui_send(b, 10 + n, r, sizeof r);
    handle = get32(r + 4);

    // 2. Set up UI rectangles
    send_rect(0, 0, 8, 8, 700, 24);   send_text(0, 1, "Clock");      // Title
    send_rect(1, 2, 8, 36, 700, 20);  send_text(1, 0, uptime);       // Uptime
    send_rect(2, 1, 8, 60, 700, 28);  send_text(2, 1, timer_val);    // Timer value
    send_rect(3, 0, 8, 92, 700, 20);  send_text(3, 0, status);       // Status line
    send_rect(4, 2, 8, 120, 700, 20); send_text(4, 0, "1/2/3 S C Q"); // Help
    commit();

    // 3. Event loop
    for (;;) {
        u8 ev[16]; i64 got = ui_poll(ev, sizeof ev);
        if (got <= 0) { yield(); continue; }
        if (ev[1] == 0x84) quit(0);                    // CLOSE_REQUEST
        if (ev[1] == 0x85) {                           // TIMER event
            u32 id = ev[8]; u8 st = ev[9];
            if ((int)id == timer_id && st == 2) timer_state = 2;
            else if ((int)id == timer_id && st == 3) timer_state = 3;
            refresh(); continue;
        }
        if (ev[1] == 0x82 && ev[12]) {                 // Key press
            u32 code = (u32)ev[8] | ((u32)ev[9] << 8);
            int a = on_key(code, mods);
            if (a == 2) quit(0);
            if (a) refresh();
        }
    }
}
```

### 6. Key Design Principles

| Principle | Implementation |
|-----------|----------------|
| **No libc** | Freestanding C, inline `svc #0` syscalls, manual string handling |
| **Kernel-chosen identity** | Principal from `apps.c` registry (604 for Clock) |
| **Capability-based** | `ui_sys_grant`, `timersys_grant` called by kernel at boot |
| **One instance** | `apps_running()` checks (pid, generation); second launch refused |
| **Honest availability** | `apps_available()` checks filesystem mounted + installer-owned executable exists |
| **Clean shutdown** | `apps_gone()` reaps on exit/fault; `ui_sys_client_gone`, `timersys_owner_gone`, `ledger_drop_instance` |
| **UI protocol compliance** | Surface quota, kernel placement, Esc/Tab never delivered, CLOSE_REQUEST handled |
| **Testability** | Every state change prints `"clock ..."` line via `SYS_WRITE` (kernel tags `[user pN]`) |

### 7. Adding a New App (Checklist)

For a new app (e.g., **Settings** as native EL0):

1. [ ] Add `APP_SETTINGS = 5` to `kernel/include/apps.h`
2. [ ] Add entry to `DEFS[]` in `kernel/proc/apps.c` with principal (e.g., 605)
3. [ ] Grant required capabilities in `apps_init()`:
   - `ui_sys_grant(605)` — always
   - `settings_grant(605, CAP_SET_APPEARANCE | CAP_SET_ATTENTION | CAP_SET_SYSTEM)` — for settings access
4. [ ] Add to `tools/pkg/apps.list`: `settings.elf tests/programs/settings.c`
5. [ ] Add Index entry in `kernel/ui/shell.c`:
   - `ITEMS`: `{ "settings", "Settings", {...}, 1 }`
   - `DEST`: `{ D_APP, 5 }`
6. [ ] Implement `tests/programs/settings.c` following the pattern:
   - `SYS_UI` for surfaces
   - `SYS_FS` for reading/writing settings registry (via `settings_get/set` syscalls)
   - Settings registry syscalls use the app's principal (kernel-enforced)
7. [ ] Add host tests (`tests/kernel-host/`) and QEMU tests (`tests/qemu/`)

## Trusted Launcher Guarantees

The launcher (`kernel/proc/launch.c`) enforces:
- **Executable owner**: Must be `PRINCIPAL_INSTALLER` (0x7F000001) — `fs_lookup_owner()` check
- **Permissions**: Not writable by others (`st.perm & FS_PERM_OTHER_W` rejected)
- **Size**: 64 bytes .. 24 KiB (`ELF_MAX_FILE`)
- **Integrity**: CRC-verified read via `fs_read()` (corrupt = `FS_ECORRUPT`)
- **Format**: Valid ELF, supported architecture (`proc_create_elf()`)
- **Identity**: Process runs as the registry's `principal`, never derived from file or caller

An app **cannot** replace its own executable (no syscall writes installer-owned files) or shadow another's (exact owner lookup).

## Evidence & Testing

| Layer | Clock App |
|-------|-----------|
| **Host unit** | Timer logic vs. `timersvc.c` oracle (200k random sequences) |
| **Host integration** | `shell_clock_test.c` (fake service, geometry, focus, toasts, scale/contrast) |
| **QEMU** | `tests/qemu/clock-tests.sh` (real kernel, real timer service, key injection, screendump) |
| **Packaging** | `scripts/package-qemu.sh` validates registry ↔ installed names match both ways |

---

*This pattern ensures every native app is a bounded, auditable, kernel-launched EL0 program with honest availability and clean lifecycle management.*