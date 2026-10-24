/* syscall(): the raw x86-64 syscalls Wine issues through libc's syscall()
 * (futex, gettid, tgkill, tkill, arch_prctl, utimensat). Anything else is
 * ENOSYS. Guest PE code executing a raw `syscall` instruction is a different
 * thing: on a Windows host that instruction enters the real NT kernel, so the
 * host never sees it (see NOTES-x64.md, "syscall thunks"). */
#ifndef UWPH_SYSCALL_H
#define UWPH_SYSCALL_H
#ifdef __cplusplus
extern "C" {
#endif

UWPH_API long uwph_syscall(long nr, ...);

#ifdef __cplusplus
}
#endif
#endif
