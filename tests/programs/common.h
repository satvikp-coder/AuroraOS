/* tests/programs/common.h — a tiny freestanding runtime for the loader test programs. They run at EL0 and use only SYS_WRITE / SYS_YIELD /
 * SYS_EXIT / SYS_FS; nothing here is linked into the kernel. */
#ifndef LOADER_TEST_COMMON_H
#define LOADER_TEST_COMMON_H

typedef unsigned long u64; typedef unsigned int u32; typedef long i64;
#define SYS_WRITE 1
#define SYS_YIELD 2
#define SYS_EXIT  4
#define SYS_FS    8
#define EFS_ENOENT (-102)         /* AE_FS_BASE + FS_ENOENT */

static inline i64 sc(i64 nr, i64 a0, i64 a1, i64 a2, i64 a3, i64 a4)
{
    register i64 x8 __asm__("x8") = nr, x0 __asm__("x0") = a0, x1 __asm__("x1") = a1, x2 __asm__("x2") = a2, x3 __asm__("x3") = a3, x4 __asm__("x4") = a4;
    __asm__ volatile ("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4) : "memory");
    return x0;
}
#define UNUSED __attribute__((unused))
static UNUSED u64 slen(const char *s) { u64 n = 0; while (s[n]) n++; return n; }
static UNUSED void say(const char *s) { sc(SYS_WRITE, (i64)s, (i64)slen(s), 0, 0, 0); }
static UNUSED void sayn(const char *s, u64 v)           /* "<s><decimal v>" */
{
    char b[96]; u64 n = 0; while (s[n] && n < 60) { b[n] = s[n]; n++; }
    char d[24]; int k = 0; if (!v) d[k++] = '0'; while (v) { d[k++] = (char)('0' + v % 10); v /= 10; }
    while (k) b[n++] = d[--k];
    b[n] = 0; say(b);
}
static UNUSED void __attribute__((noreturn)) quit(int code) { for (;;) sc(SYS_EXIT, code, 0, 0, 0, 0); }
static UNUSED void yield(void) { sc(SYS_YIELD, 0, 0, 0, 0, 0); }

static UNUSED i64 fs_create(const char *name, u64 perm) { return sc(SYS_FS, 1, (i64)name, (i64)slen(name), (i64)perm, 0); }
static UNUSED i64 fs_lookup(const char *name) { return sc(SYS_FS, 2, (i64)name, (i64)slen(name), 0, 0); }
static UNUSED i64 fs_read(i64 id, u64 off, void *buf, u64 len) { return sc(SYS_FS, 3, id, (i64)off, (i64)buf, (i64)len); }
static UNUSED i64 fs_write(i64 id, u64 off, const void *buf, u64 len) { return sc(SYS_FS, 4, id, (i64)off, (i64)buf, (i64)len); }

/* The persistent record both applications keep: a tag and a boot counter. */
struct rec { char tag[8]; u32 count; u32 pad; };

/* Open (or create) "<file>", bump its counter and write it back. Returns the new count, or a negative step number on failure. */
static UNUSED i64 bump(const char *file, const char *tag)
{
    struct rec r; i64 id = fs_lookup(file), n;
    if (id == EFS_ENOENT) {
        id = fs_create(file, 3);                      /* owner read+write only: private to this identity */
        if (id < 0) return -2;
        for (int i = 0; i < 8; i++) r.tag[i] = tag[i];
        r.count = 0; r.pad = 0;
    } else {
        if (id < 0) return -3;
        n = fs_read(id, 0, &r, sizeof r);
        if (n != (i64)sizeof r) return -4;
        for (int i = 0; i < 8; i++) if (r.tag[i] != tag[i]) return -5;
    }
    r.count++;
    n = fs_write(id, 0, &r, sizeof r);
    if (n != (i64)sizeof r) return -6;
    return r.count;
}
#endif
