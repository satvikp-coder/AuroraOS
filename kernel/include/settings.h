#ifndef AURORA_SETTINGS_H
#define AURORA_SETTINGS_H
#include <stdint.h>

/*
 * Native settings registry (issue #6, milestone E). See docs/kernel/settings.md. This is NOT the Aurora Glass browser Settings prototype: it is a
 * kernel service on AuroraFS. These are stored PREFERENCES. Nothing here drives hardware (there is no display, audio or radio driver); the values are
 * what a future compositor or service would read.
 *
 * Typed: every setting has a type (boolean, enumeration or integer range), a default and a validator; a value that does not fit is refused, never
 * clamped or truncated. Authorised: reading and writing need capabilities that only KERNEL code can grant to an identity (settings_grant); an
 * identity that was granted nothing can read (CAP_SET_READ is the default for every real identity) but cannot write anything. Persistent: one
 * versioned, CRC-protected record in the file "settings.v1" owned by PRINCIPAL_SETTINGS, a principal no process can ever hold, with no permission for
 * anyone else, so a program can neither read nor modify the file through the filesystem (it does not even learn that it exists).
 */
#define PRINCIPAL_SETTINGS 0x7F000002u        /* owner of the settings file; never given to a process */

enum { SET_THEME = 1, SET_TEXT_SCALE, SET_REDUCED_MOTION, SET_FOCUS_MODE, SET_AURA_MODE, SET_NOTIFICATIONS };
#define SETTINGS_COUNT 6

/* theme: 0 auto, 1 light, 2 dark | text scale: percent 75..200 | reduced motion: 0/1 | focus mode: 0 off, 1 work, 2 rest, 3 sleep |
 * aura mode: 0 off, 1 suggest, 2 act with review | notifications: 0 all, 1 priority only, 2 none */

#define CAP_SET_READ       0x1u               /* read any setting */
#define CAP_SET_APPEARANCE 0x2u               /* write theme, text scale, reduced motion */
#define CAP_SET_ATTENTION  0x4u               /* write focus mode, notification preference */
#define CAP_SET_SYSTEM     0x8u               /* write system policy: aura mode */
#define CAP_SET_ALL        0xFu

enum {
    SETTINGS_OK = 0,
    SETTINGS_EID = -1,        /* no such setting */
    SETTINGS_EVALUE = -2,     /* the value is not valid for this setting's type or range */
    SETTINGS_EPERM = -3,      /* the identity lacks the capability */
    SETTINGS_ESTORE = -4,     /* the value could NOT be saved (nothing changed); settings_last_store_error() has the filesystem code */
    SETTINGS_EVERSION = -5,   /* the saved file is from a newer version: it is kept untouched and writes are refused */
    SETTINGS_EARG = -6
};
enum { SETLOAD_OK = 0, SETLOAD_MISSING = 1, SETLOAD_CORRUPT = 2, SETLOAD_VERSION = 3, SETLOAD_NOFS = 4 };

#define AE_SET_BASE (-200)       /* system-call result = AE_SET_BASE + SETTINGS_E*, e.g. -203 = not permitted */
enum { SETOP_GET = 1, SETOP_SET = 2, SETOP_RESET = 3 };

int  settings_load(void);        /* (re)read the saved record; returns SETLOAD_*. Missing, corrupt or newer-version => defaults in RAM */
int  settings_status(void);      /* SETLOAD_* of the last load */
int  settings_get(uint32_t principal, uint32_t id, uint32_t *value);
int  settings_set(uint32_t principal, uint32_t id, uint64_t value);     /* saved BEFORE it takes effect; SETTINGS_ESTORE if it could not be */
int  settings_reset(uint32_t principal, uint32_t id);                   /* back to the default, same rules as settings_set */
int  settings_grant(uint32_t principal, uint32_t caps);                 /* KERNEL code only: sets the extra capabilities of an identity; 0 is refused */
void settings_revoke_all(void);
int  settings_last_store_error(void);
uint32_t settings_default(uint32_t id);
int  settings_valid(uint32_t id, uint64_t value);                       /* 1 if the value fits the setting's type */

void settingstest_run(void);     /* AURORA_SET_TEST kernels only */
int  settingstest_summary(void);
#endif
