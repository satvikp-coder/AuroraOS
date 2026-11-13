/*
 * kernel/settings.c — the native settings registry. Policy and reasons: kernel/include/settings.h and docs/kernel/settings.md.
 *
 * ON-DISK RECORD "settings.v1" (40 bytes, little-endian): "ASET" | version u32 (=1) | count u32 (=SETTINGS_COUNT) | count x value u32, positional by
 * setting id 1..count | CRC-32 of the preceding 36 bytes. Every value is re-validated against the schema on load: a record with a valid CRC but an
 * out-of-range value is corrupt.
 *
 * LOAD OUTCOMES. valid => RAM = file. missing => defaults (not an error: nothing was ever saved). corrupt (bad size, magic, CRC, count, a value out of
 * range, or the filesystem's own data check failing) => defaults; the next successful write REPLACES the broken file with a valid one. version != 1 =>
 * defaults for RAM, but the file is KEPT UNTOUCHED and writes are refused (SETTINGS_EVERSION): a newer kernel's data is never overwritten by an older one.
 *
 * WRITE PATH. validate -> authorise -> encode the whole new record -> fs_write (AuroraFS writes a whole file copy-on-write, so the file is the old
 * record or the new record, never a mix) -> ONLY THEN update the RAM copy. If the save fails or its outcome is uncertain (FS_EUNCERTAIN unmounts the
 * filesystem), the caller gets SETTINGS_ESTORE, the RAM value is unchanged and nothing claims the value was saved; after a remount settings_load()
 * shows what the disk really holds.
 */
#include <fs.h>
#include <launch.h>
#include <proc.h>
#include <settings.h>

#define FILE_NAME "settings.v1"
#define REC_SIZE  (12 + 4 * SETTINGS_COUNT + 4)

enum { T_BOOL, T_ENUM, T_RANGE };
struct def { uint32_t id; int type; uint32_t min, max, dflt, read_caps, write_caps; };
static const struct def DEFS[SETTINGS_COUNT] = {
    { SET_THEME,          T_ENUM,  0,  2,   0, CAP_SET_READ, CAP_SET_APPEARANCE },
    { SET_TEXT_SCALE,     T_RANGE, 75, 200, 100, CAP_SET_READ, CAP_SET_APPEARANCE },
    { SET_REDUCED_MOTION, T_BOOL,  0,  1,   0, CAP_SET_READ, CAP_SET_APPEARANCE },
    { SET_FOCUS_MODE,     T_ENUM,  0,  3,   0, CAP_SET_READ, CAP_SET_ATTENTION },
    { SET_AURA_MODE,      T_ENUM,  0,  2,   1, CAP_SET_READ, CAP_SET_SYSTEM },
    { SET_NOTIFICATIONS,  T_ENUM,  0,  2,   0, CAP_SET_READ, CAP_SET_ATTENTION },
};

static uint32_t cur[SETTINGS_COUNT];
static int status = SETLOAD_NOFS, loaded, store_err;

#define MAX_GRANTS 16
static struct { uint32_t principal, caps; } grants[MAX_GRANTS];

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static const struct def *find(uint32_t id) { return (id >= 1 && id <= SETTINGS_COUNT) ? &DEFS[id - 1] : 0; }
uint32_t settings_default(uint32_t id) { const struct def *d = find(id); return d ? d->dflt : 0; }
int settings_valid(uint32_t id, uint64_t v)
{
    const struct def *d = find(id);
    return d && v >= d->min && v <= d->max;                           /* compared as 64 bits: 2^32 + 1 is not "1" */
}
int settings_status(void) { return status; }
int settings_last_store_error(void) { return store_err; }

static void load_defaults(void) { for (int i = 0; i < SETTINGS_COUNT; i++) cur[i] = DEFS[i].dflt; }

static void encode(const uint32_t *v, uint8_t *out)
{
    out[0] = 'A'; out[1] = 'S'; out[2] = 'E'; out[3] = 'T';
    put32(out + 4, 1); put32(out + 8, SETTINGS_COUNT);
    for (int i = 0; i < SETTINGS_COUNT; i++) put32(out + 12 + 4 * i, v[i]);
    put32(out + 12 + 4 * SETTINGS_COUNT, fs_crc32(out, 12 + 4 * SETTINGS_COUNT));
}

int settings_load(void)
{
    load_defaults(); loaded = 0;
    if (!fs_mounted()) return status = SETLOAD_NOFS;
    fs_id_t id;
    int rc = fs_lookup_owner(PRINCIPAL_SETTINGS, FILE_NAME, &id);
    if (rc == FS_ENOENT) { loaded = 1; return status = SETLOAD_MISSING; }
    if (rc) return status = SETLOAD_NOFS;
    loaded = 1;                                                            /* a file exists (valid or not): a later write may replace it, unless it is a newer version */
    uint8_t b[REC_SIZE + 8];
    struct fs_stat st;
    if (fs_stat(0, id, &st) != FS_OK) return status = SETLOAD_CORRUPT;
    if (st.size > sizeof b) return status = SETLOAD_CORRUPT;
    int n = fs_read(0, id, 0, b, st.size);                                 /* the filesystem verifies its own data CRC first */
    if (n < 0 || (uint32_t)n != st.size) { loaded = 1; return status = SETLOAD_CORRUPT; }
    loaded = 1;
    if (st.size < 12 || !(b[0] == 'A' && b[1] == 'S' && b[2] == 'E' && b[3] == 'T')) return status = SETLOAD_CORRUPT;
    if (get32(b + 4) != 1) {
        /* A different version: a CRC we cannot interpret, so judge only the header. Keep the file; refuse to overwrite it. */
        return status = SETLOAD_VERSION;
    }
    if (st.size != REC_SIZE || get32(b + 8) != SETTINGS_COUNT) return status = SETLOAD_CORRUPT;
    if (get32(b + REC_SIZE - 4) != fs_crc32(b, REC_SIZE - 4)) return status = SETLOAD_CORRUPT;
    uint32_t tmp[SETTINGS_COUNT];
    for (int i = 0; i < SETTINGS_COUNT; i++) { tmp[i] = get32(b + 12 + 4 * i); if (!settings_valid(DEFS[i].id, tmp[i])) return status = SETLOAD_CORRUPT; }
    for (int i = 0; i < SETTINGS_COUNT; i++) cur[i] = tmp[i];
    return status = SETLOAD_OK;
}

/* ---- authorisation ---- */
int settings_grant(uint32_t principal, uint32_t caps)
{
    if (principal == 0 || principal == PRINCIPAL_NONE || principal == PRINCIPAL_SETTINGS || principal == PRINCIPAL_INSTALLER || (caps & ~CAP_SET_ALL) || caps == 0) return SETTINGS_EARG;
    for (int i = 0; i < MAX_GRANTS; i++) if (grants[i].caps && grants[i].principal == principal) { grants[i].caps = caps; return SETTINGS_OK; }
    for (int i = 0; i < MAX_GRANTS; i++) if (!grants[i].caps) { grants[i].principal = principal; grants[i].caps = caps; return SETTINGS_OK; }
    return SETTINGS_EARG;
}
void settings_revoke_all(void) { for (int i = 0; i < MAX_GRANTS; i++) grants[i].caps = 0; }
static uint32_t caps_of(uint32_t principal)
{
    if (principal == 0) return CAP_SET_ALL;                              /* the kernel */
    if (principal == PRINCIPAL_NONE) return 0;
    uint32_t c = CAP_SET_READ;                                           /* every real identity may read */
    for (int i = 0; i < MAX_GRANTS; i++) if (grants[i].caps && grants[i].principal == principal) c |= grants[i].caps;
    return c;
}

int settings_get(uint32_t principal, uint32_t id, uint32_t *value)
{
    const struct def *d = find(id);
    if (!d || !value) return d ? SETTINGS_EARG : SETTINGS_EID;
    if (!(caps_of(principal) & d->read_caps)) return SETTINGS_EPERM;
    *value = cur[id - 1];
    return SETTINGS_OK;
}

static int persist(const uint32_t *v)
{
    uint8_t rec[REC_SIZE]; encode(v, rec);
    fs_id_t id;
    int rc = fs_lookup_owner(PRINCIPAL_SETTINGS, FILE_NAME, &id);
    if (rc == FS_ENOENT) rc = fs_create(PRINCIPAL_SETTINGS, FILE_NAME, FS_PERM_OWNER_R | FS_PERM_OWNER_W, &id);   /* no permission for anybody else */
    if (rc == FS_OK) { int w = fs_write(PRINCIPAL_SETTINGS, id, 0, rec, REC_SIZE); rc = (w == (int)REC_SIZE) ? FS_OK : (w < 0 ? w : FS_EIO); }
    store_err = rc;
    return rc;
}

int settings_set(uint32_t principal, uint32_t id, uint64_t value)
{
    const struct def *d = find(id);
    if (!d) return SETTINGS_EID;
    if (!settings_valid(id, value)) return SETTINGS_EVALUE;
    if (!(caps_of(principal) & d->write_caps)) return SETTINGS_EPERM;
    if (status == SETLOAD_VERSION) return SETTINGS_EVERSION;             /* never overwrite a newer kernel's data */
    if (!loaded || !fs_mounted()) { store_err = FS_EMOUNT; return SETTINGS_ESTORE; }
    if (cur[id - 1] == (uint32_t)value && status == SETLOAD_OK) return SETTINGS_OK;   /* already the saved value: no write */
    uint32_t cand[SETTINGS_COUNT];
    for (int i = 0; i < SETTINGS_COUNT; i++) cand[i] = cur[i];
    cand[id - 1] = (uint32_t)value;
    if (persist(cand) != FS_OK) return SETTINGS_ESTORE;                  /* RAM unchanged: the value was NOT saved */
    cur[id - 1] = (uint32_t)value;
    status = SETLOAD_OK;                                                 /* a missing or corrupt file has just been replaced by a valid one */
    return SETTINGS_OK;
}

int settings_reset(uint32_t principal, uint32_t id) { return settings_set(principal, id, settings_default(id)); }

/* ---- system call: SYS_SETTINGS(op, id, value) ---- */
int64_t settings_syscall(struct proc *p, uint64_t op, uint64_t id, uint64_t value)
{
    uint32_t who = p->principal;
    if (who == 0 || who == PRINCIPAL_NONE) return AE_EPERM;
    int rc;
    uint32_t v = 0;
    switch (op) {
    case SETOP_GET:
        if (id > 0xFFFF) return AE_SET_BASE + SETTINGS_EID;
        rc = settings_get(who, (uint32_t)id, &v);
        return rc ? AE_SET_BASE + rc : (int64_t)v;
    case SETOP_SET:
        if (id > 0xFFFF) return AE_SET_BASE + SETTINGS_EID;
        rc = settings_set(who, (uint32_t)id, value);                    /* the full 64-bit value is validated: nothing is truncated to 32 bits */
        return rc ? AE_SET_BASE + rc : 0;
    case SETOP_RESET:
        if (id > 0xFFFF) return AE_SET_BASE + SETTINGS_EID;
        rc = settings_reset(who, (uint32_t)id);
        return rc ? AE_SET_BASE + rc : 0;
    }
    return AE_EINVAL;
}
