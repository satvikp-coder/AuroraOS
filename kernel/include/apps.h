/* kernel/include/apps.h - the native application registry (Cycle 21). A BOUNDED, kernel-owned table of the applications the shell may start on a person's request.
 * Policy:
 *  - Only kernel code launches. There is still no system call that starts a program, names an executable or chooses an identity, and an EL0 client cannot ask the shell to launch.
 *  - Stable application ids (APP_*), fixed in kernel code. The executable file, its identity (principal) and the UI capability come from the table, never from the person, a caller or the file.
 *  - Launch goes through the existing trusted loader (launch_register/launch_app): the executable must be INSTALLER-owned, not writable by others, CRC-verified and a valid ELF. No second loader.
 *  - One running instance per application. The instance is identified by (pid, generation): a reused pid never matches. When the process exits or faults the shell reaps it, its surfaces,
 *    queued events, timers and Passport grants are dropped (ui_sys_client_gone, timersys_owner_gone, ledger_drop_instance), and apps_gone() forgets it so the next launch is a fresh instance.
 *  - Availability is honest: an application is available only if the filesystem is mounted and its installer-owned executable exists. */
#ifndef AURORA_APPS_H
#define AURORA_APPS_H
#include <stdint.h>

enum { APP_NONE = 0, APP_CALCULATOR = 1, APP_NOTES = 2, APP_FILES = 3, APP_COUNT };
enum { APPS_OK = 0, APPS_EUNKNOWN = -1, APPS_EBUSY = -2, APPS_ENOTINSTALLED = -3, APPS_EFAILED = -4 };

void apps_init(void);                                   /* kernel code, once at boot: register every application and grant its UI capability */
int  apps_available(uint32_t app);                      /* 1 only if it could be launched now */
int  apps_launch(uint32_t app);                         /* APPS_OK, or APPS_E*; never starts a second instance */
void apps_gone(int pid, uint64_t gen);                  /* the process instance was reaped */
int  apps_running(uint32_t app);                        /* pid of the live instance, or 0 */
const char *apps_describe(int rc);                      /* a short sentence for the person */
const char *apps_label(uint32_t app);
#endif
