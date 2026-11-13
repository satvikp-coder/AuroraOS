/*
 * tests/tools/mkafs.c — host-side provisioning tool for QEMU tests: builds a fresh DISPOSABLE AuroraFS disk image from host files.
 *
 *   mkafs IMAGE SECTORS [OWNER PERM NAME HOSTFILE]...
 *
 * It links the REAL kernel/fs/afs.c (the same code the kernel mounts with) over a plain file, so the image is exactly what the kernel's
 * fs_create / fs_write produce: valid CRCs, generations, the lot. OWNER is a number, or "installer" for PRINCIPAL_INSTALLER. The IMAGE file is
 * created (truncated) here and is never a device: the tool refuses a path that is not a regular file it just created.
 * Why this exists: the loader tests must prove that machine code comes from the DISK. Provisioning from the host means the executables are not in
 * the kernel binary at all (the test script also greps the kernel for their marker strings).
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <blk.h>
#include <fs.h>
#include <launch.h>

static FILE *img; static uint64_t nsec;
int blk_present(void) { return 1; }
uint64_t blk_sectors(void) { return nsec; }
int blk_readonly(void) { return 0; }
int blk_error_definite(int rc) { (void)rc; return 1; }
int blk_read(uint64_t s, void *b, uint32_t n) { if (s + n > nsec) return BLK_ERANGE; if (fseek(img, (long)(s * 512), SEEK_SET) || fread(b, 512, n, img) != n) return BLK_EIO; return BLK_OK; }
int blk_write(uint64_t s, const void *b, uint32_t n) { if (s + n > nsec) return BLK_ERANGE; if (fseek(img, (long)(s * 512), SEEK_SET) || fwrite(b, 512, n, img) != n) return BLK_EIO; return BLK_OK; }
int blk_flush(void) { return fflush(img) ? BLK_EIO : BLK_OK; }

int main(int argc, char **argv)
{
    if (argc < 3 || (argc - 3) % 4) { fprintf(stderr, "usage: mkafs IMAGE SECTORS [OWNER PERM NAME HOSTFILE]...\n"); return 2; }
    nsec = strtoull(argv[2], 0, 0);
    if (nsec < 64 || nsec > 65536) { fprintf(stderr, "mkafs: SECTORS must be 64..65536\n"); return 2; }
    img = fopen(argv[1], "wb+");
    if (!img) { perror(argv[1]); return 1; }
    struct stat st; if (fstat(fileno(img), &st) || !S_ISREG(st.st_mode)) { fprintf(stderr, "mkafs: %s is not a regular file\n", argv[1]); return 1; }
    static uint8_t zero[512];
    for (uint64_t i = 0; i < nsec; i++) if (fwrite(zero, 512, 1, img) != 1) { perror("write"); return 1; }
    int rc = fs_format(0);
    if (rc == FS_OK) rc = fs_mount();
    if (rc != FS_OK) { fprintf(stderr, "mkafs: format/mount failed: %s\n", fs_strerror(rc)); return 1; }
    for (int a = 3; a + 3 < argc + 0; a += 4) {
        uint32_t owner = strcmp(argv[a], "installer") == 0 ? PRINCIPAL_INSTALLER : (uint32_t)strtoul(argv[a], 0, 0);
        uint32_t perm = (uint32_t)strtoul(argv[a + 1], 0, 0);
        FILE *f = fopen(argv[a + 3], "rb");
        if (!f) { perror(argv[a + 3]); return 1; }
        static uint8_t buf[FS_MAX_FILE + 1];
        size_t n = fread(buf, 1, sizeof buf, f); fclose(f);
        if (n > FS_MAX_FILE) { fprintf(stderr, "mkafs: %s is larger than %d bytes\n", argv[a + 3], FS_MAX_FILE); return 1; }
        fs_id_t id;
        rc = fs_create(owner, argv[a + 2], perm, &id);
        if (rc == FS_OK && n) rc = fs_write(owner, id, 0, buf, (uint32_t)n) == (int)n ? FS_OK : FS_EIO;
        if (rc != FS_OK) { fprintf(stderr, "mkafs: %s: %s\n", argv[a + 2], fs_strerror(rc)); return 1; }
        printf("mkafs: %-14s owner=0x%x perm=0x%x %zu bytes\n", argv[a + 2], owner, perm, n);
    }
    fclose(img);
    return 0;
}
