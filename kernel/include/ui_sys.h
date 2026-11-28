/*
 * kernel/include/ui_sys.h - the kernel side of the EL0 UI protocol (phase P1, docs/kernel/el0-ui-service-design.md): one narrow system call, the
 * compositor's surface model, per-client event queues, and the hooks the shell calls. The protocol itself (decoding, validation, ownership,
 * atomic commit, routing decisions) is the pure module kernel/ui/ui_proto.c; this file only connects it to processes.
 *
 * SYS_UI (41), x0 = operation:
 *   SYS_UI_SEND (1): x1 = user buffer holding one protocol message, x2 = its length (<= 64), x3 = user buffer for the STATUS reply, x4 = its capacity.
 *                    Returns 0 on success, or AE_UI_BASE + UIP_E* (a protocol error), or an AE_* system-call error (EFAULT, E2BIG, EPERM).
 *                    The reply (11 bytes) is copied to x3 when it fits; replies never queue, so a client cannot build a backlog.
 *   SYS_UI_POLL (2): x1 = user buffer, x2 = its capacity (>= 14). Returns the length of the next event for THIS process (EVENT_KEY, EVENT_POINTER or
 *                    CLOSE_REQUEST) or AE_EAGAIN when there is none. A process can only ever read its own queue.
 * The caller's pid, generation and principal come from the kernel's process table, never from the message. The right to create surfaces is a
 * capability granted by KERNEL code to a principal (ui_sys_grant): no system call grants, lists or revokes it, and nobody holds it by default.
 * The framebuffer, the scene array and the focus context are never reachable from a process: it sends bounded descriptions and receives bounded
 * events, nothing else.
 */
#ifndef AURORA_UI_SYS_H
#define AURORA_UI_SYS_H
#include <proc.h>
#include <ui_proto.h>

#define SYS_UI 41                     /* numbers 12..40 are deliberately still -ENOSYS (a process test asserts it; 10 is SYS_MEM, 11 SYS_MODEL) */
#define SYS_UI_SEND 1
#define SYS_UI_POLL 2
/* (operation 3, SYS_UI_AURA, is declared in broker_kernel.h: a command for the Action Broker; the policy decides and only the shell executes) */
#define AE_UI_BASE (-300)             /* system-call result = AE_UI_BASE + UIP_E*, e.g. -306 = no such surface for you */
#define UI_EVQ_DEPTH 4

void ui_sys_init(int16_t region_x, int16_t region_y, int16_t region_w, int16_t region_h);     /* the area client surfaces may occupy (kernel code, at boot) */
int  ui_sys_grant(uint32_t principal);        /* kernel code only: 0, or -1 for the kernel, none, the installer, the settings owner, or a full table */
int64_t ui_syscall(struct proc *p, uint64_t op, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4);
int  ui_sys_post(int pid, uint64_t gen, const uint8_t *ev, uint32_t n);   /* kernel code: queue a service event (<= 16 bytes) for that instance ONLY if there is room: 1 queued, 0 refused (queue full, bad pid, gen not live). Never drops another event. */
void ui_sys_client_gone(int pid, uint64_t gen);                  /* the process instance exited or faulted: its surfaces and queued events go */
const struct uip_state *ui_sys_state(void);                      /* read-only view for the compositor's drawing */

/* Input routing (called by the shell loop, kernel code). Each returns 1 when the event was delivered to / consumed by a client surface. */
int  ui_sys_pointer(int x, int y, uint8_t kind /* 0 move, 1 down, 2 up */, uint8_t buttons);   /* a press on a surface also gives it focus */
int  ui_sys_key(uint16_t code, uint16_t mods, uint8_t down, uint8_t repeat);                   /* to the owner of the FOCUSED surface only */
int  ui_sys_has_focus(void);
int ui_sys_focus_owner(int pid, uint64_t gen);                         /* trusted compositor only, returns 1 if focused */
void ui_sys_focus_clear(void);                                    /* focus returns to the shell */
#endif
