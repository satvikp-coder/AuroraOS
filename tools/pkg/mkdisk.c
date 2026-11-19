/*
 * tools/pkg/mkdisk.c - host-side installer for the local QEMU development package.
 *
 *   mkdisk IMAGE SECTORS NAME=ELFFILE...
 *
 * Builds a fresh AuroraFS image with the REAL kernel filesystem code (kernel/fs/afs.c) and installs each ELF as an INSTALLER-owned, not-other-writable
 * executable. Every ELF is first checked with the REAL loader validator (kernel/proc/elf.c) and against the loader's file-size rule, the per-owner
 * quotas and the filesystem limits; nothing is loosened to make a program fit. After writing, each file is re-read through the filesystem, compared byte
 * for byte, and its owner and permissions are verified. Output is one line per file (name, size, CRC-32) used for the package manifest.
 * IMAGE is created here (a regular file, never a device). It never modifies an existing disk: normal boot uses the disk it is given, this tool only makes new ones.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <blk.h>
#include <elf.h>
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

#define DIE(...) do { fprintf(stderr, "mkdisk: " __VA_ARGS__); fputc('\n', stderr); return 1; } while (0)

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: mkdisk IMAGE SECTORS NAME=ELFFILE...\n"); return 2; }
    nsec = strtoull(argv[2], 0, 0);
    if (nsec < 64 || nsec > 65536) DIE("SECTORS must be 64..65536");
    if (argc - 3 > (int)FS_QUOTA_FILES) DIE("%d applications exceed the installer's quota of %u files", argc - 3, FS_QUOTA_FILES);
    static uint8_t bufs[FS_QUOTA_FILES][FS_MAX_FILE + 1]; size_t lens[FS_QUOTA_FILES]; const char *names[FS_QUOTA_FILES];
    uint32_t blocks = 0;
    for (int i = 3; i < argc; i++) {                                   /* validate EVERYTHING before touching the image */
        int k = i - 3; char *eq = strchr(argv[i], '=');
        if (!eq || eq == argv[i]) DIE("argument '%s' is not NAME=ELFFILE", argv[i]);
        *eq = 0; names[k] = argv[i];
        if (fs_validate_name(names[k]) != FS_OK) DIE("'%s' is not a valid AuroraFS name", names[k]);
        for (int j = 0; j < k; j++) if (strcmp(names[j], names[k]) == 0) DIE("duplicate name '%s'", names[k]);
        FILE *f = fopen(eq + 1, "rb"); if (!f) { perror(eq + 1); return 1; }
        lens[k] = fread(bufs[k], 1, sizeof bufs[k], f); fclose(f);
        if (lens[k] > ELF_MAX_FILE) DIE("%s is %zu bytes: larger than the loader's %u byte limit", names[k], lens[k], ELF_MAX_FILE);
        if (lens[k] < 64) DIE("%s is too small to be an executable", names[k]);
        struct elf_image im; int rc = elf_validate(bufs[k], lens[k], &im);
        if (rc != ELF_OK) DIE("%s is refused by the loader's ELF validator: %s", names[k], elf_strerror(rc));
        blocks += (uint32_t)((lens[k] + 511) / 512);
    }
    if (blocks > FS_QUOTA_BLOCKS) DIE("applications need %u blocks, the installer's quota is %u", blocks, FS_QUOTA_BLOCKS);
    img = fopen(argv[1], "wb+"); if (!img) { perror(argv[1]); return 1; }
    struct stat st; if (fstat(fileno(img), &st) || !S_ISREG(st.st_mode)) DIE("%s is not a regular file", argv[1]);
    static uint8_t zero[512]; for (uint64_t i = 0; i < nsec; i++) if (fwrite(zero, 512, 1, img) != 1) { perror("write"); return 1; }
    int rc = fs_format(0); if (rc == FS_OK) rc = fs_mount();
    if (rc != FS_OK) DIE("format/mount failed: %s", fs_strerror(rc));
    for (int k = 0; k < argc - 3; k++) {
        fs_id_t id;                                                    /* created owner-writable, filled, then sealed read-only for everyone (never writable by any non-kernel principal) */
        rc = fs_create(PRINCIPAL_INSTALLER, names[k], FS_PERM_DEFAULT, &id);
        if (rc == FS_OK) { int w = fs_write(PRINCIPAL_INSTALLER, id, 0, bufs[k], (uint32_t)lens[k]); rc = w == (int)lens[k] ? FS_OK : (w < 0 ? w : FS_EIO); }
        if (rc == FS_OK) rc = fs_set_perm(PRINCIPAL_INSTALLER, id, FS_PERM_OWNER_R | FS_PERM_OTHER_R);
        if (rc != FS_OK) DIE("%s: %s", names[k], fs_strerror(rc));
    }
    fs_unmount(); if (fflush(img)) DIE("flush failed");
    rc = fs_mount(); if (rc != FS_OK) DIE("the finished image does not mount: %s", fs_strerror(rc));   /* re-validate from the bytes on disk */
    for (int k = 0; k < argc - 3; k++) {
        fs_id_t id; struct fs_stat s; static uint8_t back[FS_MAX_FILE + 1];
        if (fs_lookup_owner(PRINCIPAL_INSTALLER, names[k], &id) != FS_OK || fs_stat(0, id, &s) != FS_OK) DIE("%s is missing after install", names[k]);
        if (s.owner != PRINCIPAL_INSTALLER || (s.perm & FS_PERM_OTHER_W) || s.size != lens[k]) DIE("%s: wrong owner, permission or size after install", names[k]);
        if (fs_read(0, id, 0, back, s.size) != (int)s.size || memcmp(back, bufs[k], s.size)) DIE("%s: read-back differs from the source", names[k]);
        printf("%s %u 0x%08x\n", names[k], s.size, fs_crc32(back, s.size));
    }
    uint32_t files, bl; if (fs_usage(PRINCIPAL_INSTALLER, &files, &bl) != FS_OK || files != (uint32_t)(argc - 3)) DIE("unexpected installer usage");
    fclose(img);
    return 0;
}
