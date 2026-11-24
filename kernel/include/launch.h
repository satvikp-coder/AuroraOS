#ifndef AURORA_LAUNCH_H
#define AURORA_LAUNCH_H
#include <stdint.h>

/*
 * Trusted program launch (issue #6, milestone D). See docs/kernel/loader.md.
 *
 * WHO MAY LAUNCH. Only kernel code can call launch_app(). There is no system call that starts a program, names an executable, or chooses
 * an identity, so no EL0 program can launch anything (a future "start this app" request would be a separate, authorised kernel service).
 *
 * WHICH EXECUTABLE. The kernel's application REGISTRY (launch_register) maps an application name to (executable file name, identity). The
 * executable is looked up by EXACT owner PRINCIPAL_INSTALLER (fs_lookup_owner): a file with the same name owned by anybody else is never
 * considered, so an application cannot shadow its own or another's executable. Only kernel code can create or modify files owned by
 * PRINCIPAL_INSTALLER (no system call ever passes that principal), so an application cannot replace an installed executable either.
 * The file must not be writable by others, must pass the AuroraFS integrity check (CRC) and then the ELF validation. A passing checksum only
 * means the bytes are what was stored; it says nothing about the program being safe, so the program runs unprivileged at EL0 regardless.
 *
 * WHICH IDENTITY. The process runs as the principal in the REGISTRY ENTRY. It is never derived from the file's owner, from the caller or from
 * the executable's contents. Consequently running a program whose file is readable by others (or even launching the same file under a second
 * registry entry) grants nothing: the process has exactly the files of the identity it was registered with, and no others.
 */
#define PRINCIPAL_INSTALLER 0x7F000001u      /* owner of installed executables; never given to a process */

enum {
    LAUNCH_OK = 0,
    LAUNCH_ENOAPP = -1,     /* no such application in the registry */
    LAUNCH_ENOEXE = -2,     /* the installer-owned executable does not exist (detail = FS_E*) */
    LAUNCH_EUNSAFE = -3,    /* the executable is writable by others, or has an impossible size */
    LAUNCH_EREAD = -4,      /* the filesystem could not read it, e.g. FS_ECORRUPT (detail = FS_E*) */
    LAUNCH_EEXE = -5,       /* not a supported executable (detail = ELF_E*) */
    LAUNCH_ENOMEM = -6,     /* no process slot or no memory; nothing is left allocated */
    LAUNCH_EFS = -7,        /* the filesystem is not mounted */
    LAUNCH_EREG = -8        /* launch_register refused its arguments */
};

#define LAUNCH_MAX_APPS 16

int  launch_register(const char *app, const char *file, uint32_t principal, uint64_t arg);   /* kernel code only; arg is passed to the program in x0; LAUNCH_OK or LAUNCH_EREG */
void launch_reset(void);                                                         /* forget every registration (tests) */
int  launch_app(const char *app, int *pid, int *detail);                         /* LAUNCH_OK and *pid, or LAUNCH_E* (*detail = FS_E* / ELF_E*) */
const char *launch_strerror(int rc);

/* init: after the boot self-checks, if the filesystem is mounted and holds an INSTALLER-owned "init.elf", the kernel starts it as the first user process
 * under the fixed application identity PRINCIPAL_INIT and lets it run (bounded) before the boot finishes. No init.elf => nothing happens and nothing is
 * printed. There is no system call that starts programs, so init cannot start others; today it is the first and only boot-time user program. */
#define PRINCIPAL_INIT 1u
void init_run(void);

void loadtest_run(void);        /* AURORA_LOAD_TEST kernels only */
int  loadtest_summary(void);
#endif
