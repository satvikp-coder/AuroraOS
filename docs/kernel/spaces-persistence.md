# Spaces Persistence — Current State and Implementation Plan

**Related to:** Issue #8 (Aurora Glass UI shell) — remaining work: "persistence of Spaces"
**Status:** Selected Space index is persisted; per-Space data (accent, pinned apps, recent objects) is NOT yet implemented.

---

## 1. Current State of Spaces Persistence

### What IS Implemented (✅)

| Feature | Location | Mechanism |
|---------|----------|-----------|
| 4 built-in Spaces | `kernel/ui/shell.c:5` | `SHELL_SPACE_NAMES[4] = {"Study", "Work", "Projects", "Personal"}` |
| Selected Space index (0–3) | `kernel/include/shell.h:98` | `struct shell { uint8_t space; }` |
| Space selection UI | `kernel/ui/shell.c:159–169` | `build_spaces()` / `spaces_scaled()` |
| Space shown on Home | `kernel/ui/shell.c:133, 384` | `sh->space_txt` shows current Space name |
| **Space persistence across reboot** | `kernel/ui/shell_run.c:291–294, 232–236` | Settings registry `SET_SPACE = 7` (7th setting) |

**How Space persistence works today:**
1. At boot, after `settings_load()` succeeds, the shell reads `SET_SPACE` via `store_get(SET_SPACE, &sp)` (shell_run.c:292)
2. If valid (0–3), `shell_select_space(&SH, sp)` is called (shell_run.c:292)
3. Each time the user selects a Space (`shell_select_space`), `log_state()` saves it via `store_set(SET_SPACE, SH.space)` (shell_run.c:233)
4. The settings registry writes the whole 7-setting record (44 bytes) to `settings.v1` on AuroraFS (settings.md:41–42)
5. Read-only disk / no disk / corrupt record → falls back to default Space 0 (Study), logged honestly (settings.md:89–93)

### What Is NOT Implemented (❌)

| Feature | Design Spec | Current State |
|---------|-------------|---------------|
| Per-Space accent color | Each Space has its own accent (Blueprint line 28) | Single global accent (`UI_COL_ACCENT`) |
| Per-Space pinned apps | Each Space holds pinned tools (DESIGN.md line 60) | No pinned apps concept in kernel |
| Per-Space recent objects | Each Space has recent objects (Blueprint line 28) | Single global `recent[8]` array (shell.h:121) |
| Per-Space notification prefs | Each Space has notification preferences (Blueprint line 28) | Single global `SET_NOTIFICATIONS` setting |
| User-created Spaces | "four built-in Spaces plus user Spaces" (native-compositor-mapping.md) | Only 4 fixed Spaces, hardcoded |
| Space isolation | "Each holds content, pinned tools and permissioned settings" (DESIGN.md:60) | No isolation — Space is just a label |

---

## 2. Architecture: Where Spaces Data Lives

### Settings Registry (Already Used)
- **File:** `settings.v1` on AuroraFS, owned by `PRINCIPAL_SETTINGS (0x7F000002)`
- **Record:** 44 bytes (7 × u32 values + header + CRC), version 1
- **Settings:** `SET_THEME(1)`, `SET_TEXT_SCALE(2)`, `SET_REDUCED_MOTION(3)`, `SET_FOCUS_MODE(4)`, `SET_AURA_MODE(5)`, `SET_NOTIFICATIONS(6)`, `SET_SPACE(7)`
- **Capabilities:** `CAP_SET_APPEARANCE` (theme, text scale, reduced motion), `CAP_SET_ATTENTION` (focus mode, notifications), `CAP_SET_SYSTEM` (aura mode)
- **Shell identity:** `PRINCIPAL_SHELL (0x7F000003)` granted `CAP_SET_APPEARANCE | CAP_SET_ATTENTION` (NOT system)

### AuroraFS (For Per-Space Data Files)
- **Interface:** `kernel/include/fs.h` — `fs_create`, `fs_lookup`, `fs_read`, `fs_write`, `fs_delete`, `fs_stat`
- **Quotas:** 8 files / 96 blocks per principal
- **File IDs:** `fs_id_t = (generation << 8) | inode` — stale IDs never name newer files
- **Principal model:** Kernel assigns principal at process creation; EL0 never supplies it

### Shell State (In-Memory)
```c
// kernel/include/shell.h
struct shell {
    uint8_t space;                    // Current Space index (0–3)
    const char *recent[SHELL_RECENT]; // Global recent items (not per-Space!)
    uint8_t nrecent;
    // ... no per-Space data fields
};
```

---

## 3. Integration Points

### A. Settings Registry (`settings.h`)
```c
// Get current Space (already works)
int rc = settings_get(PRINCIPAL_SHELL, SET_SPACE, &space);

// Set current Space (already works)
int rc = settings_set(PRINCIPAL_SHELL, SET_SPACE, new_space);

// For new per-Space settings, would need:
// - New SET_* IDs (e.g., SET_SPACE_ACCENT_0..3, SET_SPACE_PINNED_0..3, etc.)
// - Or a single blob setting per Space (more flexible)
```

### B. AuroraFS (`fs.h`) — For Per-Space Data Files
```c
// Create a per-Space data file (owned by PRINCIPAL_SHELL)
fs_id_t id;
int rc = fs_create(PRINCIPAL_SHELL, "space.0.data", FS_PERM_DEFAULT, &id);

// Write Space data (e.g., JSON or binary blob)
struct space_data { uint32_t accent; uint32_t pinned[8]; char recent[8][32]; };
rc = fs_write(PRINCIPAL_SHELL, id, 0, &data, sizeof(data));

// Read Space data
rc = fs_read(PRINCIPAL_SHELL, id, 0, &data, sizeof(data));

// Lookup by name (for loading on Space switch)
rc = fs_lookup(PRINCIPAL_SHELL, "space.0.data", &id);
```

**Principal choice:** Use `PRINCIPAL_SHELL (0x7F000003)` — same as settings, already granted appearance+attention caps. The shell is a kernel component, not an EL0 process.

---

## 4. Implementation Plan: Completing Spaces Persistence

### Phase 1: Per-Space Data Structure (Kernel)

Add to `kernel/include/shell.h`:
```c
#define SHELL_SPACES 4
#define SHELL_PINNED_MAX 8
#define SHELL_RECENT_PER_SPACE 8

struct shell_space_data {
    uint32_t accent;                    // Accent color (ARGB)
    uint32_t pinned[SHELL_PINNED_MAX];  // App IDs (from launcher registry)
    uint8_t npinned;
    char recent[SHELL_RECENT_PER_SPACE][32]; // Recent item keys per Space
    uint8_t nrecent;
    uint32_t notification_pref;         // 0=all, 1=priority, 2=none (per-Space override)
    uint8_t has_custom_data;            // Whether this Space has saved custom data
};

// In struct shell:
struct shell_space_data spaces[SHELL_SPACES];
uint8_t space_data_loaded[SHELL_SPACES]; // Track which Space's data is loaded
```

### Phase 2: AuroraFS Storage for Per-Space Data

**File naming:** `space.<index>.data` (e.g., `space.0.data`, `space.1.data`)

**Write path (on Space change or data modification):**
```c
// In shell_run.c or shell.c when Space data changes
static int space_data_save(struct shell *sh, uint32_t space_idx)
{
    if (space_idx >= SHELL_SPACES) return -1;
    fs_id_t id;
    int rc = fs_lookup(PRINCIPAL_SHELL, space_data_filename(space_idx), &id);
    if (rc == FS_ENOENT) {
        rc = fs_create(PRINCIPAL_SHELL, space_data_filename(space_idx), FS_PERM_DEFAULT, &id);
    }
    if (rc != FS_OK) return rc;
    return fs_write(PRINCIPAL_SHELL, id, 0, &sh->spaces[space_idx], sizeof(struct shell_space_data));
}
```

**Read path (on boot or Space switch):**
```c
static int space_data_load(struct shell *sh, uint32_t space_idx)
{
    if (space_idx >= SHELL_SPACES) return -1;
    fs_id_t id;
    int rc = fs_lookup(PRINCIPAL_SHELL, space_data_filename(space_idx), &id);
    if (rc != FS_OK) return rc; // File doesn't exist → use defaults
    rc = fs_read(PRINCIPAL_SHELL, id, 0, &sh->spaces[space_idx], sizeof(struct shell_space_data));
    if (rc == (int)sizeof(struct shell_space_data)) {
        sh->space_data_loaded[space_idx] = 1;
        return 0;
    }
    return -1;
}
```

**Integration with existing Space selection (shell_run.c):**
```c
// In log_state() where space_store saves SET_SPACE:
if (SH.space != *space) {
    *space = SH.space;
    // ... existing SET_SPACE save ...
    // NEW: Save per-Space data for the NEWLY SELECTED Space
    space_data_save(&SH, SH.space);
    // NEW: Load per-Space data for the NEWLY SELECTED Space
    space_data_load(&SH, SH.space);
    // Apply loaded data (accent, pinned, recent) to shell state
    shell_apply_space_data(&SH, SH.space);
}
```

### Phase 3: Default Per-Space Data (Boot)

In `shell_boot_draw()` (shell_run.c:203–225), after `shell_init()`:
```c
// Load all Space data files at boot (once FS is mounted)
for (uint32_t i = 0; i < SHELL_SPACES; i++) {
    space_data_load(&SH, i);
}
// Apply data for the restored Space (from SET_SPACE)
shell_apply_space_data(&SH, SH.space);
```

**Default accents (matching DESIGN.md / Blueprint):**
| Space | Default Accent (ARGB) | Hue |
|-------|----------------------|-----|
| Study (0) | `0xFF3478F6` | Blue |
| Work (1) | `0xFF00A859` | Green |
| Projects (2) | `0xFFE8701A` | Orange |
| Personal (3) | `0xFFD01B5A` | Pink |

### Phase 4: UI Integration

**Accent color:** Use `sh->spaces[sh->space].accent` instead of `UI_COL_ACCENT` in `PAL_STD` / palette selection.

**Pinned apps:** Add "Pinned" section to Home screen (shell.c `build_home()` / `home_scaled()`) showing `sh->spaces[sh->space].pinned` apps.

**Recent objects:** Replace global `sh->recent` with `sh->spaces[sh->space].recent` in Index search ranking (`index_model.c` tie-break).

**Notification preference:** Per-Space override of `SET_NOTIFICATIONS` — shell checks Space data first, falls back to global setting.

---

## 5. What Survives Reboot vs. What Doesn't

| Data | Survives Reboot? | Mechanism |
|------|------------------|-----------|
| Selected Space index (0–3) | ✅ Yes | Settings registry `SET_SPACE` |
| Per-Space accent color | ✅ Yes (planned) | AuroraFS `space.N.data` |
| Per-Space pinned apps | ✅ Yes (planned) | AuroraFS `space.N.data` |
| Per-Space recent objects | ✅ Yes (planned) | AuroraFS `space.N.data` |
| Per-Space notification pref | ✅ Yes (planned) | AuroraFS `space.N.data` |
| Running processes | ❌ No | Never persisted; Pages shows live table |
| Process memory / state | ❌ No | No checkpoint/restore |
| Unsaved app data | ❌ No | Apps must save via AuroraFS themselves |
| Shell session values (contrast) | ❌ No | Session-only, no registry setting |
| Global recent (current) | ❌ No | In-memory only; will be per-Space |

---

## 6. Testing Strategy (QEMU Only)

### New Test: `tests/qemu/spaces-persistence-tests.sh`

1. **Select Space → Reboot → Verify restored**
   - Select "Work" (Space 1), reboot, verify Home shows "Work"
   - Already covered by existing `spaces-tests.sh`

2. **Per-Space accent persistence**
   - Change Space 1 accent (via future UI), reboot, verify accent restored

3. **Per-Space pinned apps persistence**
   - Pin "Calculator" to Space 2, reboot, verify pinned on Home in Space 2

4. **Per-Space recent objects**
   - Open "Notes" in Space 3, reboot, verify "Notes" appears in Index recent for Space 3 only

5. **Isolation: recent objects don't leak across Spaces**
   - Open "Clock" in Space 0, switch to Space 1, verify "Clock" not in Space 1 recent

6. **Read-only disk / no disk**
   - Verify defaults used, no crash, honest logging

7. **Corrupt Space data file**
   - Corrupt `space.1.data`, verify defaults for Space 1, other Spaces unaffected

8. **Quota exhaustion**
   - Fill PRINCIPAL_SHELL quota, verify Space data save fails gracefully

---

## 7. Files to Modify

| File | Purpose |
|------|---------|
| `kernel/include/shell.h` | Add `shell_space_data` struct, per-Space fields in `shell` |
| `kernel/ui/shell.c` | Use per-Space accent, pinned, recent; add `shell_apply_space_data()` |
| `kernel/ui/shell_run.c` | Load/save per-Space data via AuroraFS; integrate with Space selection |
| `kernel/include/fs.h` | (No change — use existing interface) |
| `kernel/include/settings.h` | (No change — SET_SPACE already exists) |
| `tests/qemu/spaces-persistence-tests.sh` | New test script |

---

## 8. Open Questions

1. **User-created Spaces:** The design mentions "four built-in Spaces plus user Spaces". Should we:
   - Keep 4 fixed built-in + dynamic user Spaces (stored in AuroraFS directory)?
   - Or expand fixed list to a configurable array?

2. **Space data format:** Binary struct (current plan) vs. JSON (extensible, human-readable)?
   - Binary is simpler, smaller, faster; JSON survives schema changes better.
   - Recommendation: Binary with version field for now; migrate to JSON if schema grows.

3. **Accent color picker:** No color picker UI exists yet. How does user change accent?
   - Could be a Settings row per Space (appears when Space selected)
   - Or a long-press on Space card → color palette

4. **Pinned app management:** How does user pin/unpin?
   - Long-press app on Home → "Pin to current Space"
   - Or dedicated "Pinned" section with edit mode

5. **Notification preference per Space:** Is this a shell concern or a future notification service concern?
   - For now: store in Space data; future service reads it.

---

## 9. Dependencies

- **AuroraFS must be mounted** (fs_mounted() == 1) for per-Space data to persist
- **Settings registry must load OK** (SET_SPACE restoration works without FS, but per-Space data needs FS)
- **PRINCIPAL_SHELL must have quota** (8 files / 96 blocks) — 4 Space data files = 4 files, ~1 block each
- **No new capabilities needed** — PRINCIPAL_SHELL already has appearance+attention

---

## 10. Rollout Order

1. **Data structure + AuroraFS read/write** (kernel only, no UI)
2. **Boot load + Space switch save/load** (integrate with existing `log_state()`)
3. **Accent color applied to palette** (visual change)
4. **Pinned apps on Home** (UI addition)
5. **Per-Space recent in Index** (ranking tie-break)
6. **Per-Space notification pref** (when notification service exists)
7. **User-created Spaces** (future, after core persistence works)

---

*Document created as part of Issue #8 Spaces persistence work. All development targets QEMU ARM64 `virt` only. Never flash or modify Samsung Galaxy F04.*