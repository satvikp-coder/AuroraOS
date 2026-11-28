/*
 * kernel/include/audiosys.h - SYS_AUDIO (43): Passport-controlled EL0 audio capture & TTS playback.
 */
#ifndef AURORA_AUDIOSYS_H
#define AURORA_AUDIOSYS_H

#include <stdint.h>

#define SYS_AUDIO 43

enum {
    AUDIO_OP_PRESENT     = 0,  /* returns vsnd_present() (0 or 1) */
    AUDIO_OP_CAP_OPEN    = 1,  /* x1 = stream_id, x2 = user struct cap_handle *out */
    AUDIO_OP_CAP_PREPARE = 2,  /* x1 = handle.slot, x2 = handle.serial, x3 = buffer_count */
    AUDIO_OP_CAP_START   = 3,  /* x1 = handle.slot, x2 = handle.serial */
    AUDIO_OP_CAP_STOP    = 4,  /* x1 = handle.slot, x2 = handle.serial */
    AUDIO_OP_CAP_RELEASE = 5,  /* x1 = handle.slot, x2 = handle.serial */
    AUDIO_OP_PLAY_TTS    = 6   /* x1 = stream_id, x2 = user const char *text, x3 = len (1..6) */
};

struct proc;
int64_t audio_syscall(struct proc *p, uint64_t op, uint64_t a1, uint64_t a2, uint64_t a3);
void    audio_proc_cleanup(struct proc *p);

#endif /* AURORA_AUDIOSYS_H */
