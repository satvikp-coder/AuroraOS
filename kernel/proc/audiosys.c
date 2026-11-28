/*
 * kernel/proc/audiosys.c - SYS_AUDIO (43): bounded EL0 audio capture & offline TTS playback.
 * Enforces kernel-attested principal + process generation on every handle operation,
 * validates user pointers via copy_from_user/copy_to_user, and cleans up on process exit/fault.
 */
#include <audiosys.h>
#include <proc.h>
#include <vsnd.h>

static int64_t map_snd_err(int rc)
{
    if (rc == SND_OK) return AE_OK;
    if (rc == SND_EPERM) return AE_EPERM;
    if (rc == SND_ENODEV || rc == SND_ELEGACY) return AE_ENOSYS;
    if (rc == SND_ENOMEM) return AE_ENOMEM;
    return AE_EINVAL;
}

int64_t audio_syscall(struct proc *p, uint64_t op, uint64_t a1, uint64_t a2, uint64_t a3)
{
    if (!p) return AE_EINVAL;
    struct cap_owner owner = { p->principal, p->gen };

    switch (op) {
    case AUDIO_OP_PRESENT:
        return vsnd_present() ? 1 : 0;

    case AUDIO_OP_CAP_OPEN: {
        if (p->principal == PRINCIPAL_NONE) return AE_EPERM;
        struct cap_handle h = { 0, 0 };
        int rc = vsnd_capture_open(owner, (uint32_t)a1, &h);
        if (rc != SND_OK) return map_snd_err(rc);
        int urc = copy_to_user(a2, &h, sizeof(h));
        if (urc != AE_OK) {
            (void)vsnd_capture_release(h, owner);
            return urc;
        }
        return AE_OK;
    }

    case AUDIO_OP_CAP_PREPARE: {
        if (p->principal == PRINCIPAL_NONE) return AE_EPERM;
        struct cap_handle h = { (size_t)a1, a2 };
        return map_snd_err(vsnd_capture_prepare(h, owner, (uint32_t)a3));
    }

    case AUDIO_OP_CAP_START: {
        if (p->principal == PRINCIPAL_NONE) return AE_EPERM;
        struct cap_handle h = { (size_t)a1, a2 };
        return map_snd_err(vsnd_capture_start(h, owner));
    }

    case AUDIO_OP_CAP_STOP: {
        if (p->principal == PRINCIPAL_NONE) return AE_EPERM;
        struct cap_handle h = { (size_t)a1, a2 };
        return map_snd_err(vsnd_capture_stop(h, owner));
    }

    case AUDIO_OP_CAP_RELEASE: {
        if (p->principal == PRINCIPAL_NONE) return AE_EPERM;
        struct cap_handle h = { (size_t)a1, a2 };
        return map_snd_err(vsnd_capture_release(h, owner));
    }

    case AUDIO_OP_PLAY_TTS: {
        if (p->principal == PRINCIPAL_NONE) return AE_EPERM;
        if (a3 == 0 || a3 > 6u) return AE_EINVAL;
        char text[8];
        for (size_t i = 0; i < sizeof(text); ++i) text[i] = 0;
        int urc = copy_from_user(text, a2, (size_t)a3);
        if (urc != AE_OK) return urc;
        return map_snd_err(vsnd_play_tts((uint32_t)a1, text, (size_t)a3));
    }

    default:
        return AE_ENOSYS;
    }
}

void audio_proc_cleanup(struct proc *p)
{
    if (!p || p->principal == PRINCIPAL_NONE) return;
    (void)vsnd_revoke_audio(p->principal);
}
