/*
 * kernel/include/capture_lifecycle.h - Freestanding audio capture session lifecycle state machine (AUDIO-02).
 *
 * Pure, header-only, no allocation, no libc, no globals: tracks up to CAP_MAX_SESSIONS (4) concurrent
 * capture sessions, enforcing AUDIO_CAP_RECORD, principal ownership, state transitions, bounded byte
 * counters, and fail-closed revocation from any state.
 */
#ifndef AURORA_CAPTURE_LIFECYCLE_H
#define AURORA_CAPTURE_LIFECYCLE_H

#include <stddef.h>
#include <stdint.h>
#include <virtio_snd.h>  /* AUDIO_CAP_RECORD */

#define CAP_MAX_SESSIONS 4u
#define CAP_MAX_BYTES    (16u * 1024u * 1024u) /* 16 MiB per session ceiling; prevents counter wrap */
#define CAP_MAX_BUFFERS  64u

enum cap_state {
    CAP_IDLE = 0,
    CAP_AUTHORIZED,
    CAP_PREPARED,
    CAP_RUNNING,
    CAP_STOPPED,
    CAP_REVOKED
};

enum cap_err {
    CAP_OK = 0,
    CAP_E_ARG = -1,
    CAP_E_STATE = -2,
    CAP_E_OWNER = -3,
    CAP_E_CAP = -4,
    CAP_E_FULL = -5,
    CAP_E_BOUNDS = -6
};

struct cap_session {
    uint8_t state;
    uint32_t principal;
    uint32_t stream_id;
    uint32_t caps;
    uint32_t buffer_count;
    uint64_t bytes_captured;
};

struct cap_table {
    struct cap_session sessions[CAP_MAX_SESSIONS];
};

static inline void cap_table_init(struct cap_table *t)
{
    if (!t) return;
    for (size_t i = 0; i < CAP_MAX_SESSIONS; i++) {
        t->sessions[i].state = CAP_IDLE;
        t->sessions[i].principal = 0;
        t->sessions[i].stream_id = 0;
        t->sessions[i].caps = 0;
        t->sessions[i].buffer_count = 0;
        t->sessions[i].bytes_captured = 0;
    }
}

static inline void cap_session_init(struct cap_session *s)
{
    if (!s) return;
    s->state = CAP_IDLE;
    s->principal = 0;
    s->stream_id = 0;
    s->caps = 0;
    s->buffer_count = 0;
    s->bytes_captured = 0;
}

/* IDLE -> AUTHORIZED: requires non-zero principal and AUDIO_CAP_RECORD. */
static inline int cap_authorize(struct cap_session *s, uint32_t principal, uint32_t caps, uint32_t stream_id)
{
    if (!s || principal == 0 || principal == 0xFFFFFFFFu) return CAP_E_ARG;
    if (s->state != CAP_IDLE) return CAP_E_STATE;
    if (!(caps & AUDIO_CAP_RECORD)) return CAP_E_CAP;
    s->state = CAP_AUTHORIZED;
    s->principal = principal;
    s->stream_id = stream_id;
    s->caps = caps;
    s->buffer_count = 0;
    s->bytes_captured = 0;
    return CAP_OK;
}

/* AUTHORIZED -> PREPARED: tracks bounded buffer count. */
static inline int cap_prepare(struct cap_session *s, uint32_t principal, uint32_t buffer_count)
{
    if (!s || principal == 0) return CAP_E_ARG;
    if (s->state == CAP_REVOKED) return CAP_E_STATE;
    if (s->state != CAP_AUTHORIZED) return CAP_E_STATE;
    if (s->principal != principal) return CAP_E_OWNER;
    if (buffer_count == 0 || buffer_count > CAP_MAX_BUFFERS) return CAP_E_BOUNDS;
    s->buffer_count = buffer_count;
    s->state = CAP_PREPARED;
    return CAP_OK;
}

/* PREPARED -> RUNNING or STOPPED -> RUNNING. */
static inline int cap_start(struct cap_session *s, uint32_t principal)
{
    if (!s || principal == 0) return CAP_E_ARG;
    if (s->state != CAP_PREPARED && s->state != CAP_STOPPED) return CAP_E_STATE;
    if (s->principal != principal) return CAP_E_OWNER;
    if (!(s->caps & AUDIO_CAP_RECORD)) return CAP_E_CAP;
    s->state = CAP_RUNNING;
    return CAP_OK;
}

/* Record captured bytes while RUNNING; refuses wrap/overflow past CAP_MAX_BYTES. */
static inline int cap_record_bytes(struct cap_session *s, uint32_t principal, uint32_t bytes)
{
    if (!s || principal == 0 || bytes == 0) return CAP_E_ARG;
    if (s->state != CAP_RUNNING) return CAP_E_STATE;
    if (s->principal != principal) return CAP_E_OWNER;
    if ((uint64_t)bytes > CAP_MAX_BYTES || s->bytes_captured > (uint64_t)CAP_MAX_BYTES - (uint64_t)bytes)
        return CAP_E_BOUNDS;
    s->bytes_captured += (uint64_t)bytes;
    return CAP_OK;
}

/* RUNNING -> STOPPED: stops capture, keeps buffer tracking. */
static inline int cap_stop(struct cap_session *s, uint32_t principal)
{
    if (!s || principal == 0) return CAP_E_ARG;
    if (s->state != CAP_RUNNING) return CAP_E_STATE;
    if (s->principal != principal) return CAP_E_OWNER;
    s->state = CAP_STOPPED;
    return CAP_OK;
}

/* PREPARED / STOPPED / REVOKED -> IDLE: releases buffers and allows reuse. */
static inline int cap_release(struct cap_session *s, uint32_t principal)
{
    if (!s || principal == 0) return CAP_E_ARG;
    if (s->state != CAP_PREPARED && s->state != CAP_STOPPED && s->state != CAP_REVOKED) return CAP_E_STATE;
    if (s->principal != principal) return CAP_E_OWNER;
    cap_session_init(s);
    return CAP_OK;
}

/* Any state -> CAP_REVOKED: forced cleanup on permission revocation or exit. */
static inline int cap_revoke(struct cap_session *s)
{
    if (!s) return CAP_E_ARG;
    s->caps = 0;
    s->buffer_count = 0;
    s->state = CAP_REVOKED;
    return CAP_OK;
}

/* Table helpers for concurrent sessions (up to CAP_MAX_SESSIONS). */
static inline int cap_table_open(struct cap_table *t, uint32_t principal, uint32_t caps, uint32_t stream_id, size_t *out_slot)
{
    if (!t || !out_slot) return CAP_E_ARG;
    for (size_t i = 0; i < CAP_MAX_SESSIONS; i++) {
        if (t->sessions[i].state == CAP_IDLE) {
            int rc = cap_authorize(&t->sessions[i], principal, caps, stream_id);
            if (rc == CAP_OK) *out_slot = i;
            return rc;
        }
    }
    return CAP_E_FULL;
}

static inline int cap_table_revoke_principal(struct cap_table *t, uint32_t principal)
{
    if (!t || principal == 0) return 0;
    int n = 0;
    for (size_t i = 0; i < CAP_MAX_SESSIONS; i++) {
        struct cap_session *session = &t->sessions[i];
        if (session->state != CAP_IDLE && session->state != CAP_REVOKED &&
            session->principal == principal) {
            cap_revoke(session);
            n++;
        }
    }
    return n;
}

#endif
