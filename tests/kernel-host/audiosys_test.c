#include <audiosys.h>
#include <proc.h>
#include <vsnd.h>
#include <stdio.h>

static int open_calls, prepare_calls, write_ok, copy_ok, release_calls;
static int open_result;

int user_writable(uint64_t va, size_t n) { return va == 8 && n == sizeof(struct cap_handle) && write_ok; }
int copy_to_user(uint64_t va, const void *src, size_t n)
{
    (void)src;
    return user_writable(va, n) && copy_ok ? AE_OK : AE_EFAULT;
}
int copy_from_user(void *dst, uint64_t va, size_t n) { (void)dst; (void)va; (void)n; return AE_EFAULT; }
int vsnd_present(void) { return 1; }
int vsnd_play_tts(uint32_t stream, const char *text, size_t n) { (void)stream; (void)text; (void)n; return SND_OK; }
int vsnd_revoke_audio(uint32_t principal) { (void)principal; return 0; }
int vsnd_capture_open(struct cap_owner owner, uint32_t stream, struct cap_handle *handle)
{
    (void)owner; (void)stream; (void)handle;
    open_calls++;
    return open_result;
}
int vsnd_capture_prepare(struct cap_handle h, struct cap_owner owner, uint32_t count)
{
    (void)h; (void)owner; (void)count;
    prepare_calls++;
    return CAP_OK;
}
int vsnd_capture_start(struct cap_handle h, struct cap_owner owner) { (void)h; (void)owner; return CAP_E_OWNER; }
int vsnd_capture_stop(struct cap_handle h, struct cap_owner owner) { (void)h; (void)owner; return CAP_E_STATE; }
int vsnd_capture_release(struct cap_handle h, struct cap_owner owner)
{
    (void)h; (void)owner;
    release_calls++;
    return CAP_OK;
}

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "failed at line %d\n", __LINE__); return 1; } } while (0)

int main(void)
{
    static struct proc p;
    p.principal = 602;
    p.gen = 1;
    CHECK(audio_syscall(&p, AUDIO_OP_CAP_OPEN, 0, 8, 0) == AE_EFAULT && open_calls == 0);
    write_ok = 1;
    CHECK(audio_syscall(&p, AUDIO_OP_CAP_OPEN, UINT64_C(1) << 32, 8, 0) == AE_EINVAL && open_calls == 0);
    CHECK(audio_syscall(&p, AUDIO_OP_CAP_OPEN, 0, 8, 0) == AE_EFAULT && release_calls == 1);
    copy_ok = 1;
    CHECK(audio_syscall(&p, AUDIO_OP_CAP_OPEN, 0, 8, 0) == AE_OK);
    open_result = CAP_E_CAP;
    CHECK(audio_syscall(&p, AUDIO_OP_CAP_OPEN, 0, 8, 0) == AE_EPERM);
    open_result = CAP_E_FULL;
    CHECK(audio_syscall(&p, AUDIO_OP_CAP_OPEN, 0, 8, 0) == AE_EAGAIN);
    CHECK(audio_syscall(&p, AUDIO_OP_CAP_PREPARE, 0, 1, UINT64_C(1) << 32) == AE_EINVAL && prepare_calls == 0);
    CHECK(audio_syscall(&p, AUDIO_OP_CAP_START, 0, 1, 0) == AE_EPERM);
    CHECK(audio_syscall(&p, AUDIO_OP_CAP_STOP, 0, 1, 0) == AE_EINVAL);
    CHECK(audio_syscall(&p, AUDIO_OP_PLAY_TTS, UINT64_C(1) << 32, 8, 1) == AE_EINVAL);
    puts("audio_syscall: 10 passed, 0 failed");
    return 0;
}
