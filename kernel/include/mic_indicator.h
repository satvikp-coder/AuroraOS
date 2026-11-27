/*
 * kernel/include/mic_indicator.h - Compositor-owned microphone listening indicator (AUDIO-05).
 *
 * Pure, freestanding, no allocation, no libc, no globals.
 * Only MIC_ACTOR_COMPOSITOR may activate, pause, or clear the live indicator; unprivileged
 * client actors are refused and logged in `denied_spoofs`. Binds to (principal, pid, gen) so
 * process exit or permission revocation clears the indicator fail-closed.
 */
#ifndef AURORA_MIC_INDICATOR_H
#define AURORA_MIC_INDICATOR_H

#include <stdint.h>

enum mic_ind_state {
    MIC_IND_OFF = 0,
    MIC_IND_LISTENING,
    MIC_IND_PAUSED,
    MIC_IND_REVOKED
};

enum mic_actor_kind {
    MIC_ACTOR_CLIENT = 0,
    MIC_ACTOR_COMPOSITOR = 1
};

enum mic_ind_err {
    MIC_OK = 0,
    MIC_E_ARG = -1,
    MIC_E_ACTOR = -2,
    MIC_E_STATE = -3,
    MIC_E_BUSY = -4
};

struct mic_indicator {
    uint8_t state;
    uint32_t owner_principal;
    uint32_t owner_pid;
    uint64_t owner_gen;
    uint64_t frames_notified;
    uint32_t denied_spoofs;
};

static inline void mic_ind_init(struct mic_indicator *m)
{
    if (!m) return;
    m->state = MIC_IND_OFF;
    m->owner_principal = 0;
    m->owner_pid = 0;
    m->owner_gen = 0;
    m->frames_notified = 0;
    m->denied_spoofs = 0;
}

static inline int mic_ind_begin(struct mic_indicator *m,
                                uint8_t actor,
                                uint32_t principal,
                                uint32_t pid,
                                uint64_t gen)
{
    if (!m || principal == 0 || principal == 0xFFFFFFFFu || pid == 0 || gen == 0) return MIC_E_ARG;
    if (actor != MIC_ACTOR_COMPOSITOR) {
        m->denied_spoofs++;
        return MIC_E_ACTOR;
    }
    if (m->state == MIC_IND_LISTENING || m->state == MIC_IND_PAUSED) return MIC_E_BUSY;
    m->state = MIC_IND_LISTENING;
    m->owner_principal = principal;
    m->owner_pid = pid;
    m->owner_gen = gen;
    return MIC_OK;
}

static inline int mic_ind_pause(struct mic_indicator *m, uint8_t actor)
{
    if (!m) return MIC_E_ARG;
    if (actor != MIC_ACTOR_COMPOSITOR) {
        m->denied_spoofs++;
        return MIC_E_ACTOR;
    }
    if (m->state != MIC_IND_LISTENING) return MIC_E_STATE;
    m->state = MIC_IND_PAUSED;
    return MIC_OK;
}

static inline int mic_ind_resume(struct mic_indicator *m, uint8_t actor)
{
    if (!m) return MIC_E_ARG;
    if (actor != MIC_ACTOR_COMPOSITOR) {
        m->denied_spoofs++;
        return MIC_E_ACTOR;
    }
    if (m->state != MIC_IND_PAUSED) return MIC_E_STATE;
    m->state = MIC_IND_LISTENING;
    return MIC_OK;
}

static inline int mic_ind_notify_frame(struct mic_indicator *m, uint8_t actor)
{
    if (!m) return MIC_E_ARG;
    if (actor != MIC_ACTOR_COMPOSITOR) {
        m->denied_spoofs++;
        return MIC_E_ACTOR;
    }
    if (m->state != MIC_IND_LISTENING) return MIC_E_STATE;
    if (m->frames_notified < UINT64_MAX) m->frames_notified++;
    return MIC_OK;
}

static inline int mic_ind_stop(struct mic_indicator *m, uint8_t actor)
{
    if (!m) return MIC_E_ARG;
    if (actor != MIC_ACTOR_COMPOSITOR) {
        m->denied_spoofs++;
        return MIC_E_ACTOR;
    }
    m->state = MIC_IND_OFF;
    m->owner_principal = 0;
    m->owner_pid = 0;
    m->owner_gen = 0;
    return MIC_OK;
}

/* Process exit or PID/generation replacement: clears indicator if it matches the live owner. */
static inline int mic_ind_owner_gone(struct mic_indicator *m, uint32_t pid, uint64_t gen)
{
    if (!m || pid == 0 || gen == 0) return 0;
    if (m->state == MIC_IND_OFF) return 0;
    if (m->owner_pid == pid && m->owner_gen == gen) {
        m->state = MIC_IND_OFF;
        m->owner_principal = 0;
        m->owner_pid = 0;
        m->owner_gen = 0;
        return 1;
    }
    return 0;
}

/* Permission revocation for a principal: marks REVOKED and clears owner if matching. */
static inline int mic_ind_revoke_principal(struct mic_indicator *m, uint8_t actor, uint32_t principal)
{
    if (!m || principal == 0) return MIC_E_ARG;
    if (actor != MIC_ACTOR_COMPOSITOR) {
        m->denied_spoofs++;
        return MIC_E_ACTOR;
    }
    if (m->state != MIC_IND_OFF && m->owner_principal == principal) {
        m->state = MIC_IND_REVOKED;
        m->owner_principal = 0;
        m->owner_pid = 0;
        m->owner_gen = 0;
        return 1;
    }
    return 0;
}

static inline const char *mic_ind_label(const struct mic_indicator *m)
{
    if (!m) return "";
    switch (m->state) {
    case MIC_IND_LISTENING: return "Mic: Listening";
    case MIC_IND_PAUSED:    return "Mic: Paused";
    case MIC_IND_REVOKED:   return "Mic: Revoked";
    default:                return "";
    }
}

#endif /* AURORA_MIC_INDICATOR_H */
