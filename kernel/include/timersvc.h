/*
 * kernel/include/timersvc.h - the monotonic timer SERVICE core (issue #9, Clock/Timer): pure, bounded, host-testable. It is a table of countdown timers
 * measured against a caller-supplied MONOTONIC millisecond counter (the kernel's timer count, never a frame count and never a wall-clock time).
 *
 * Correctness under delayed scheduling: a timer stores an ABSOLUTE deadline (start + duration, saturating). It is complete as soon as the counter has
 * reached the deadline, however late the caller polls, so a starved or sleeping process still sees the right remaining time and the right completion;
 * nothing accumulates ticks or deltas. A counter that goes backwards is treated as "no time passed" and counted as an anomaly, never as a negative duration.
 *
 * Ownership: a timer belongs to the kernel-attested (pid, generation) that started it. Only that instance can cancel it or read its events; a client that
 * exits has its timers dropped (tsvc_owner_gone). The table is bounded (TS_MAX) and durations are limited (1 second .. 24 hours): a request outside
 * that is refused, never clamped.
 *
 * What it is not (yet): it is not wired to a system call, an EL0 application or Aura. There is no notification, sound or persistence; a timer does
 * not survive a restart. "Completed" is a state the owner can read, nothing more. Aura keeps saying "no timer service" until this is wired and tested in QEMU.
 */
#ifndef AURORA_TIMERSVC_H
#define AURORA_TIMERSVC_H
#include <stdint.h>

#define TS_MAX 4
#define TS_PER_OWNER 2                  /* timers one owner instance may hold (running, done or cancelled but not yet released): one owner cannot take every slot */
#define TS_MIN_MS 1000ULL
#define TS_MAX_MS (24ULL * 60 * 60 * 1000)

enum { TS_FREE = 0, TS_RUNNING, TS_DONE, TS_CANCELLED };
enum { TS_OK = 0, TS_E_ARG = -1, TS_E_FULL = -2, TS_E_NOENT = -3, TS_E_OWNER = -4, TS_E_STATE = -5, TS_E_QUOTA = -6 };

struct tsvc_timer { uint8_t state; uint32_t pid; uint64_t gen, start_ms, duration_ms, deadline_ms; };
struct tsvc { struct tsvc_timer t[TS_MAX]; uint64_t last_now; uint32_t backwards; uint8_t seen; };

void tsvc_init(struct tsvc *s);
/* Start a countdown of duration_ms for the requester. Returns the timer id (0..TS_MAX-1) or a TS_E_* error. */
int  tsvc_start(struct tsvc *s, uint64_t now_ms, uint64_t duration_ms, uint32_t pid, uint64_t gen);
int  tsvc_cancel(struct tsvc *s, uint32_t id, uint32_t pid, uint64_t gen);                      /* only the owner instance, only while RUNNING */
/* Advance the table to now_ms: every running timer whose deadline has been reached becomes DONE (once). Returns how many completed on this call. */
int  tsvc_poll(struct tsvc *s, uint64_t now_ms);
/* Owner-checked reads. Return TS_OK and fill the outputs, or an error. remaining is 0 for a DONE or CANCELLED timer. */
int  tsvc_query(struct tsvc *s, uint64_t now_ms, uint32_t id, uint32_t pid, uint64_t gen, uint8_t *state, uint64_t *remaining_ms);
void tsvc_owner_gone(struct tsvc *s, uint32_t pid, uint64_t gen);                                 /* drop every timer of that instance */
int  tsvc_release(struct tsvc *s, uint32_t id, uint32_t pid, uint64_t gen);                      /* free a DONE or CANCELLED timer's slot (owner only) */
#endif
