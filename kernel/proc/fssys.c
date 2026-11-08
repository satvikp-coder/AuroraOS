/*
 * kernel/proc/fssys.c — the file-access system call (SYS_FS = 8), milestone C of issue #6.
 *
 * IDENTITY. The filesystem principal of a caller is struct proc::principal, set ONLY by kernel code (proc_set_principal) when a
 * process is created; no system call can read, change or choose it, and EL0 never passes one. It is a durable APPLICATION identity (the
 * number stored in file owner fields on disk), deliberately NOT the pid, the ASID or the process generation: those are reused or restart
 * from 1 on every boot, so a file owner derived from them would pass to the next occupant of a pid or to a different program after a
 * reboot. A process that has no principal (PRINCIPAL_NONE) or the reserved kernel value 0 gets AE_EPERM for every file operation.
 * Whoever creates a process decides its principal; milestone D's loader will take it from the owner of the executable's file.
 *
 * ABI. x8 = SYS_FS, x0 = operation, x1..x4 = arguments, result in x0. Results >= 0 are values; negative results are AE_* codes for generic
 * problems (bad pointer AE_EFAULT, bad argument AE_EINVAL, too large AE_E2BIG, no principal AE_EPERM) or AE_FS_BASE + FS_E* for filesystem
 * errors (e.g. FS_EACCES -7 => -107). Operations:
 *   1 CREATE   (name_ptr, name_len, perm)           -> file id
 *   2 LOOKUP   (name_ptr, name_len)                  -> file id (files the caller may not touch look absent)
 *   3 READ     (id, offset, buf, len<=4096)          -> bytes read
 *   4 WRITE    (id, offset, buf, len<=4096)          -> bytes written
 *   5 STAT     (id, struct user_fs_stat *)           -> 0
 *   6 LIST     (struct user_fs_stat *, max<=16)      -> entries written (only files the caller may read)
 *   7 DELETE   (id)                                  -> 0
 *   8 SET_PERM (id, perm)                            -> 0
 * There is no format, mount, raw-block or kernel-metadata operation. Offsets and lengths are range-checked before any use; names are copied
 * to a kernel buffer, must not contain NUL and are validated by the filesystem; user buffers are only touched by copy_from_user / copy_to_user.
 * File ids are not secrets: every operation checks the permission bits against the kernel-supplied principal.
 */
#include <fs.h>
#include <proc.h>

static uint8_t iobuf[SYS_FS_IO_MAX];

static int64_t fsret(int rc) { return rc >= 0 ? rc : AE_FS_BASE + rc; }

static int get_name(char *out, uint64_t uptr, uint64_t len)
{
    if (len == 0 || len > FS_NAME_MAX) return AE_EINVAL;
    int rc = copy_from_user(out, uptr, (size_t)len);
    if (rc != AE_OK) return rc;
    for (uint64_t i = 0; i < len; i++) if (out[i] == 0) return AE_EINVAL;          /* no silent truncation at an embedded NUL */
    out[len] = 0;
    return AE_OK;
}

static void to_user_stat(struct user_fs_stat *u, const struct fs_stat *s)
{
    u->id = s->id; u->size = s->size; u->owner = s->owner; u->perm = s->perm; u->reserved = 0;
    for (int i = 0; i < 32; i++) u->name[i] = s->name[i];
}

int64_t fs_syscall(struct proc *p, uint64_t op, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
    uint32_t who = p->principal;
    if (who == 0 || who == PRINCIPAL_NONE) return AE_EPERM;
    if (op < FSOP_CREATE || op > FSOP_SET_PERM) return AE_EINVAL;
    if (!fs_mounted()) return AE_FS_BASE + FS_EMOUNT;

    char name[FS_NAME_MAX + 1];
    int rc;
    fs_id_t id;

    switch (op) {
    case FSOP_CREATE:
        if (a3 > FS_PERM_MASK) return AE_EINVAL;
        rc = get_name(name, a1, a2); if (rc != AE_OK) return rc;
        rc = fs_create(who, name, (uint32_t)a3, &id);
        return rc ? fsret(rc) : (int64_t)id;
    case FSOP_LOOKUP:
        rc = get_name(name, a1, a2); if (rc != AE_OK) return rc;
        rc = fs_lookup(who, name, &id);
        return rc ? fsret(rc) : (int64_t)id;
    case FSOP_READ: {
        if (a3 == 0 && a4 == 0) return 0;
        if (a4 > SYS_FS_IO_MAX) return AE_E2BIG;
        if (a2 > FS_MAX_FILE) return AE_FS_BASE + FS_EARG;
        if (a4 && !user_writable(a3, (size_t)a4)) return AE_EFAULT;                 /* refuse before reading the file */
        rc = fs_read(who, (fs_id_t)a1, (uint32_t)a2, iobuf, (uint32_t)a4);
        if (rc <= 0) return fsret(rc);
        int cr = copy_to_user(a3, iobuf, (size_t)rc); if (cr != AE_OK) return cr;
        return rc;
    }
    case FSOP_WRITE: {
        if (a4 > SYS_FS_IO_MAX) return AE_E2BIG;
        if (a2 > FS_MAX_FILE) return AE_FS_BASE + FS_EARG;
        if (a4) { int cr = copy_from_user(iobuf, a3, (size_t)a4); if (cr != AE_OK) return cr; }
        rc = fs_write(who, (fs_id_t)a1, (uint32_t)a2, iobuf, (uint32_t)a4);
        return fsret(rc);
    }
    case FSOP_STAT: {
        struct fs_stat st; struct user_fs_stat u;
        if (!user_writable(a2, sizeof u)) return AE_EFAULT;
        rc = fs_stat(who, (fs_id_t)a1, &st); if (rc) return fsret(rc);
        to_user_stat(&u, &st);
        return copy_to_user(a2, &u, sizeof u);
    }
    case FSOP_LIST: {
        struct fs_stat st[SYS_FS_LIST_MAX]; struct user_fs_stat u[SYS_FS_LIST_MAX];
        if (a2 == 0 || a2 > SYS_FS_LIST_MAX) return AE_EINVAL;
        if (!user_writable(a1, (size_t)a2 * sizeof u[0])) return AE_EFAULT;
        rc = fs_list(who, st, (int)a2); if (rc < 0) return fsret(rc);
        for (int i = 0; i < rc; i++) to_user_stat(&u[i], &st[i]);
        if (rc) { int cr = copy_to_user(a1, u, (size_t)rc * sizeof u[0]); if (cr != AE_OK) return cr; }
        return rc;
    }
    case FSOP_DELETE:
        return fsret(fs_delete(who, (fs_id_t)a1));
    case FSOP_SET_PERM:
        if (a2 > FS_PERM_MASK) return AE_EINVAL;
        return fsret(fs_set_perm(who, (fs_id_t)a1, (uint32_t)a2));
    }
    return AE_EINVAL;
}
