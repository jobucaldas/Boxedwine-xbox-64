/*
 * Signals. Wine's ntdll.so installs SA_SIGINFO handlers for SIGSEGV, SIGILL,
 * SIGBUS, SIGFPE, SIGTRAP (guest exceptions), SIGUSR1 (wineserver asks a
 * thread to suspend / report its context), SIGQUIT (terminate thread),
 * SIGINT/SIGABRT, SIGSYS (raw guest `syscall` via syscall user dispatch) and
 * ignores SIGPIPE (server.c). Handlers read and modify a Linux ucontext.
 *
 * Synchronous signals: a host exception hook receives the Windows
 * EXCEPTION_RECORD + CONTEXT, first offers it to the W^X emulation, then
 * converts it (uwph_exception_to_siginfo + uwph_context_to_ucontext), runs the
 * registered handler on the faulting thread, and converts the edited
 * ucontext back before resuming. Which hook is usable in UWP is a hardware
 * question: AddVectoredExceptionHandler is documented "desktop apps only";
 * probe.h reports what the console actually exports.
 *
 * Asynchronous signals (SIGUSR1/SIGQUIT sent by tgkill from wineserver):
 * queued per thread and delivered at the next host entry (syscall dispatcher,
 * blocking wait) — the cooperative path. Preempting a thread spinning in
 * guest code needs SuspendThread + Get/SetThreadContext; SetThreadContext is
 * documented "desktop apps only", so that path is optional/probed.
 */
#ifndef UWPH_SIGNAL_H
#define UWPH_SIGNAL_H
#ifdef __cplusplus
extern "C" {
#endif

UWPH_API int uwph_sigaction(int sig, const struct uwph_sigaction *act, struct uwph_sigaction *old);
UWPH_API void *uwph_signal(int sig, void *handler);
UWPH_API int uwph_sigemptyset(uwph_sigset_t *s);
UWPH_API int uwph_sigaddset(uwph_sigset_t *s, int sig);
UWPH_API int uwph_sigismember(const uwph_sigset_t *s, int sig);
UWPH_API int uwph_pthread_sigmask(int how, const uwph_sigset_t *set, uwph_sigset_t *old);
UWPH_API int uwph_sigprocmask(int how, const uwph_sigset_t *set, uwph_sigset_t *old);
UWPH_API int uwph_sigaltstack(const uwph_stack_t *ss, uwph_stack_t *old);
UWPH_API int uwph_kill(uwph_pid_t pid, int sig);
UWPH_API unsigned int uwph_alarm(unsigned int seconds);

/* ---- Windows exception <-> Linux signal translation (portable) ---- */
/* Windows exception codes, defined here so the mapping compiles everywhere. */
#define UWPH_EXC_ACCESS_VIOLATION      0xC0000005u
#define UWPH_EXC_IN_PAGE_ERROR         0xC0000006u
#define UWPH_EXC_ILLEGAL_INSTRUCTION   0xC000001Du
#define UWPH_EXC_INT_DIVIDE_BY_ZERO    0xC0000094u
#define UWPH_EXC_INT_OVERFLOW          0xC0000095u
#define UWPH_EXC_PRIV_INSTRUCTION      0xC0000096u
#define UWPH_EXC_STACK_OVERFLOW        0xC00000FDu
#define UWPH_EXC_DATATYPE_MISALIGNMENT 0x80000002u
#define UWPH_EXC_BREAKPOINT            0x80000003u
#define UWPH_EXC_SINGLE_STEP           0x80000004u
#define UWPH_EXC_FLT_DIVIDE_BY_ZERO    0xC000008Eu
#define UWPH_EXC_FLT_INVALID_OPERATION 0xC0000090u
#define UWPH_EXC_FLT_MULTIPLE_TRAPS    0xC00002B5u  /* STATUS_FLOAT_MULTIPLE_TRAPS (SSE) */

struct uwph_exception {          /* the parts of EXCEPTION_RECORD we use */
    uint32_t code;
    uint64_t address;
    uint32_t nparams;
    uint64_t info[4];            /* AV: info[0] = 0 read / 1 write / 8 exec, info[1] = address */
};

/* Fills signo/si_code/si_addr and the trap/err words Wine's handlers read
 * (TRAP_sig, ERROR_sig). Returns the signal number, 0 if unmapped. */
int uwph_exception_to_siginfo(const struct uwph_exception *e, uwph_siginfo_t *si,
                              int64_t *trapno, int64_t *err);

/* Register-file conversion between Windows x64 CONTEXT and Linux ucontext.
 * `ctx` is a CONTEXT* on the Windows build; on the stub build a struct with
 * the same layout (src/internal.h). FXSAVE area copied as-is. */
void uwph_context_to_ucontext(const void *ctx, uwph_ucontext_t *uc);
void uwph_ucontext_to_context(const uwph_ucontext_t *uc, void *ctx);

/* Entry from the host exception hook. Returns 1 if a handler (or W^X) took
 * it and execution resumes from the (possibly edited) ctx. */
int uwph_dispatch_exception(const struct uwph_exception *e, void *ctx);

/* Deliver queued asynchronous signals for the current thread (called on
 * every host entry). */
void uwph_deliver_pending_signals(void *ctx);

#ifdef __cplusplus
}
#endif
#endif
