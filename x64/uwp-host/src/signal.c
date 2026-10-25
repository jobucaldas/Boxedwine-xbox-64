/*
 * Signal emulation: sigaction table, masks, Windows exception -> Linux
 * signal translation, CONTEXT <-> ucontext, and the host exception hook.
 */
#include "internal.h"

static struct uwph_sigaction actions[UWPH_NSIG];
static uwph_lock_t sig_lock = UWPH_LOCK_INIT;

/* ---- sigset ---- */
UWPH_API int uwph_sigemptyset(uwph_sigset_t *s) { memset(s, 0, sizeof(*s)); return 0; }
UWPH_API int uwph_sigaddset(uwph_sigset_t *s, int sig)
{
    uwph_trace_hit(UWPH_TRACE_SIGADDSET);
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    UWPH_NATIVE(int, "sigaddset", (uwph_sigset_t *, int));
    UWPH_NATIVE_RETURN(fn(s, sig));
#endif

    if (sig <= 0 || sig >= UWPH_NSIG) return UWPH_FAIL(UWPH_EINVAL);
    s->val[(sig - 1) / 64] |= (uint64_t)1 << ((sig - 1) % 64);
    return 0;
}
UWPH_API int uwph_sigismember(const uwph_sigset_t *s, int sig)
{
    if (sig <= 0 || sig >= UWPH_NSIG) return UWPH_FAIL(UWPH_EINVAL);
    return (int)((s->val[(sig - 1) / 64] >> ((sig - 1) % 64)) & 1);
}

UWPH_API int uwph_sigaction(int sig, const struct uwph_sigaction *act, struct uwph_sigaction *old)
{
    if (sig <= 0 || sig >= UWPH_NSIG || ((sig == UWPH_SIGKILL || sig == UWPH_SIGSTOP) && act))
        return UWPH_FAIL(UWPH_EINVAL);
    uwph_lock(&sig_lock);
    if (old) *old = actions[sig];
    if (act) actions[sig] = *act;
    uwph_unlock(&sig_lock);
    return 0;
}

UWPH_API void *uwph_signal(int sig, void *handler)
{
    struct uwph_sigaction act, old;
    memset(&act, 0, sizeof(act));
    act.u.handler = (uwph_sighandler_t)handler;
    act.sa_flags = UWPH_SA_RESTART;
    if (uwph_sigaction(sig, &act, &old) < 0) return (void *)-1;   /* SIG_ERR */
    return (void *)old.u.handler;
}

UWPH_API int uwph_pthread_sigmask(int how, const uwph_sigset_t *set, uwph_sigset_t *old)
{
    uwph_trace_hit(UWPH_TRACE_PTHREAD_SIGMASK);
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    UWPH_NATIVE(int, "pthread_sigmask", (int, const uwph_sigset_t *, uwph_sigset_t *));
    UWPH_NATIVE_RETURN(fn(how, set, old));
#endif

    struct uwph_thread *t = uwph_thread_current();
    if (old) *old = t->sigmask;
    if (!set) return 0;
    for (int i = 0; i < 16; i++) {
        switch (how) {
        case UWPH_SIG_BLOCK:   t->sigmask.val[i] |= set->val[i]; break;
        case UWPH_SIG_UNBLOCK: t->sigmask.val[i] &= ~set->val[i]; break;
        case UWPH_SIG_SETMASK: t->sigmask.val[i] = set->val[i]; break;
        default: return UWPH_EINVAL;   /* pthread_* returns the error */
        }
    }
    uwph_deliver_pending_signals(NULL);
    return 0;
}

UWPH_API int uwph_sigprocmask(int how, const uwph_sigset_t *set, uwph_sigset_t *old)
{
    int r = uwph_pthread_sigmask(how, set, old);
    return r ? UWPH_FAIL(r) : 0;
}

UWPH_API int uwph_sigaltstack(const uwph_stack_t *ss, uwph_stack_t *old)
{
    struct uwph_thread *t = uwph_thread_current();
    if (old) *old = t->altstack;
    if (ss) t->altstack = *ss;
    return 0;
}

UWPH_API int uwph_kill(uwph_pid_t pid, int sig)
{
    if (pid != uwph_getpid() && pid != 0) return UWPH_FAIL(UWPH_ESRCH);   /* single process */
    return uwph_tgkill(uwph_getpid(), uwph_gettid(), sig);
}

UWPH_API unsigned int uwph_alarm(unsigned int seconds) { (void)seconds; return 0; }

/* ---- exception -> signal ---- */
int uwph_exception_to_siginfo(const struct uwph_exception *e, uwph_siginfo_t *si,
                              int64_t *trapno, int64_t *err)
{
    memset(si, 0, sizeof(*si));
    *err = 0;
    si->u.addr = (void *)(uintptr_t)e->address;
    switch (e->code) {
    case UWPH_EXC_ACCESS_VIOLATION:
        si->si_signo = UWPH_SIGSEGV;
        si->si_code = UWPH_SEGV_ACCERR;
        si->u.addr = (void *)(uintptr_t)(e->nparams >= 2 ? e->info[1] : e->address);
        *trapno = UWPH_TRAP_PAGEFLT;
        /* page-fault error code: Wine recovers ExceptionInformation[0] as (err >> 1) & 9 */
        *err = (int64_t)(((e->info[0] & 1) << 1) | ((e->info[0] & 8) << 1)) | 4 /* user */;
        break;
    case UWPH_EXC_STACK_OVERFLOW:
        si->si_signo = UWPH_SIGSEGV;
        si->si_code = UWPH_SEGV_ACCERR;
        si->u.addr = (void *)(uintptr_t)(e->nparams >= 2 ? e->info[1] : e->address);
        *trapno = UWPH_TRAP_PAGEFLT;
        *err = 2 | 4;
        break;
    case UWPH_EXC_PRIV_INSTRUCTION:
        si->si_signo = UWPH_SIGSEGV;   /* #GP: Wine emulates or raises PRIV_INSTRUCTION */
        si->si_code = 0x80;            /* SI_KERNEL */
        si->u.addr = NULL;
        *trapno = UWPH_TRAP_PROTFLT;
        break;
    case UWPH_EXC_INT_OVERFLOW:
        si->si_signo = UWPH_SIGSEGV;
        si->si_code = 0x80;
        *trapno = 4;                   /* TRAP_x86_OFLOW */
        break;
    case UWPH_EXC_ILLEGAL_INSTRUCTION:
        si->si_signo = UWPH_SIGILL;
        si->si_code = UWPH_ILL_ILLOPC;
        *trapno = UWPH_TRAP_PRIVINFLT;
        break;
    case UWPH_EXC_INT_DIVIDE_BY_ZERO:
        si->si_signo = UWPH_SIGFPE;
        si->si_code = UWPH_FPE_INTDIV;
        *trapno = UWPH_TRAP_DIVIDE;
        break;
    case UWPH_EXC_FLT_DIVIDE_BY_ZERO:
    case UWPH_EXC_FLT_INVALID_OPERATION:
        si->si_signo = UWPH_SIGFPE;
        si->si_code = UWPH_FPE_FLTDIV;
        *trapno = UWPH_TRAP_ARITHTRAP;
        break;
    case UWPH_EXC_FLT_MULTIPLE_TRAPS:
        si->si_signo = UWPH_SIGFPE;
        si->si_code = UWPH_FPE_FLTDIV;
        *trapno = UWPH_TRAP_XMMFLT;
        break;
    case UWPH_EXC_BREAKPOINT:
        si->si_signo = UWPH_SIGTRAP;
        si->si_code = UWPH_TRAP_BRKPT;
        *trapno = UWPH_TRAP_BPTFLT;
        break;
    case UWPH_EXC_SINGLE_STEP:
        si->si_signo = UWPH_SIGTRAP;
        si->si_code = UWPH_TRAP_TRACE;
        *trapno = UWPH_TRAP_DEBUG;
        break;
    case UWPH_EXC_DATATYPE_MISALIGNMENT:
        si->si_signo = UWPH_SIGBUS;
        si->si_code = UWPH_BUS_ADRALN;
        *trapno = UWPH_TRAP_ALIGNFLT;
        break;
    case UWPH_EXC_IN_PAGE_ERROR:
        si->si_signo = UWPH_SIGBUS;
        si->si_code = 2;               /* BUS_ADRERR */
        si->u.addr = (void *)(uintptr_t)(e->nparams >= 2 ? e->info[1] : e->address);
        *trapno = UWPH_TRAP_PAGEFLT;
        break;
    default:
        return 0;   /* C++ exceptions, debug-print, thread-name...: not signals */
    }
    return si->si_signo;
}

/* ---- CONTEXT <-> ucontext ---- */
void uwph_context_to_ucontext(const void *vctx, uwph_ucontext_t *uc)
{
    const uwph_win_context *c = vctx;
    int64_t *g = uc->uc_mcontext.gregs;
    memset(uc, 0, sizeof(*uc));
    g[UWPH_REG_R8] = (int64_t)c->R8;   g[UWPH_REG_R9] = (int64_t)c->R9;
    g[UWPH_REG_R10] = (int64_t)c->R10; g[UWPH_REG_R11] = (int64_t)c->R11;
    g[UWPH_REG_R12] = (int64_t)c->R12; g[UWPH_REG_R13] = (int64_t)c->R13;
    g[UWPH_REG_R14] = (int64_t)c->R14; g[UWPH_REG_R15] = (int64_t)c->R15;
    g[UWPH_REG_RDI] = (int64_t)c->Rdi; g[UWPH_REG_RSI] = (int64_t)c->Rsi;
    g[UWPH_REG_RBP] = (int64_t)c->Rbp; g[UWPH_REG_RBX] = (int64_t)c->Rbx;
    g[UWPH_REG_RDX] = (int64_t)c->Rdx; g[UWPH_REG_RAX] = (int64_t)c->Rax;
    g[UWPH_REG_RCX] = (int64_t)c->Rcx; g[UWPH_REG_RSP] = (int64_t)c->Rsp;
    g[UWPH_REG_RIP] = (int64_t)c->Rip;
    g[UWPH_REG_EFL] = c->EFlags;
    g[UWPH_REG_CSGSFS] = (int64_t)((uint64_t)c->SegCs | (uint64_t)c->SegGs << 16 |
                                   (uint64_t)c->SegFs << 32 | (uint64_t)c->SegSs << 48);
    memcpy(&uc->__fpregs_mem, c->FltSave, sizeof(uc->__fpregs_mem));
    uc->uc_mcontext.fpregs = &uc->__fpregs_mem;
    uc->uc_sigmask = uwph_thread_current()->sigmask;
}

void uwph_ucontext_to_context(const uwph_ucontext_t *uc, void *vctx)
{
    uwph_win_context *c = vctx;
    const int64_t *g = uc->uc_mcontext.gregs;
    c->R8 = (uint64_t)g[UWPH_REG_R8];   c->R9 = (uint64_t)g[UWPH_REG_R9];
    c->R10 = (uint64_t)g[UWPH_REG_R10]; c->R11 = (uint64_t)g[UWPH_REG_R11];
    c->R12 = (uint64_t)g[UWPH_REG_R12]; c->R13 = (uint64_t)g[UWPH_REG_R13];
    c->R14 = (uint64_t)g[UWPH_REG_R14]; c->R15 = (uint64_t)g[UWPH_REG_R15];
    c->Rdi = (uint64_t)g[UWPH_REG_RDI]; c->Rsi = (uint64_t)g[UWPH_REG_RSI];
    c->Rbp = (uint64_t)g[UWPH_REG_RBP]; c->Rbx = (uint64_t)g[UWPH_REG_RBX];
    c->Rdx = (uint64_t)g[UWPH_REG_RDX]; c->Rax = (uint64_t)g[UWPH_REG_RAX];
    c->Rcx = (uint64_t)g[UWPH_REG_RCX]; c->Rsp = (uint64_t)g[UWPH_REG_RSP];
    c->Rip = (uint64_t)g[UWPH_REG_RIP];
    /* user-modifiable flags only (CF PF AF ZF SF TF DF OF, AC, ID) */
    c->EFlags = (c->EFlags & ~0x254dd5u) | ((uint32_t)g[UWPH_REG_EFL] & 0x254dd5u);
    if (uc->uc_mcontext.fpregs) {
        memcpy(c->FltSave, uc->uc_mcontext.fpregs, sizeof(c->FltSave));
        c->MxCsr = uc->uc_mcontext.fpregs->mxcsr;
    }
}

/* ---- handler invocation ---- */
static int run_handler(int sig, uwph_siginfo_t *si, uwph_ucontext_t *uc)
{
    struct uwph_thread *t = uwph_thread_current();
    struct uwph_sigaction act;
    uwph_sigset_t saved = t->sigmask;

    uwph_lock(&sig_lock);
    act = actions[sig];
    if (act.sa_flags & UWPH_SA_RESETHAND) memset(&actions[sig], 0, sizeof(actions[sig]));
    uwph_unlock(&sig_lock);

    if ((void *)act.u.handler == UWPH_SIG_IGN) return 1;
    if ((void *)act.u.handler == UWPH_SIG_DFL) return 0;    /* default action: let the host decide */

    for (int i = 0; i < 16; i++) t->sigmask.val[i] |= act.sa_mask.val[i];
    if (!(act.sa_flags & UWPH_SA_NODEFER)) uwph_sigaddset(&t->sigmask, sig);
    t->in_handler++;
    /* TODO(SA_ONSTACK): switch to t->altstack with uwph_call_on_stack; the
       handler currently runs on the faulting thread's stack. */
    if (act.sa_flags & UWPH_SA_SIGINFO) act.u.sigaction(sig, si, uc);
    else act.u.handler(sig);
    t->in_handler--;
    t->sigmask = saved;
    return 1;
}

int uwph_raise_signal(int sig, uwph_siginfo_t *si, uwph_ucontext_t *uc)
{
    return run_handler(sig, si, uc);
}

int uwph_dispatch_exception(const struct uwph_exception *e, void *ctx)
{
    struct uwph_thread *t = uwph_thread_current();
    uwph_siginfo_t si;
    uwph_ucontext_t uc;
    int64_t trapno, err;
    int sig, handled, was_guest = t->teb_swap.in_guest;

    /* No supported UWP path for live TEB mutation. */
    if (was_guest) return 0;
    if (e->code == UWPH_EXC_ACCESS_VIOLATION && e->nparams >= 2 &&
        uwph_wx_handle_fault((const void *)(uintptr_t)e->info[1], (int)e->info[0]))
        return 1;
    if (!(sig = uwph_exception_to_siginfo(e, &si, &trapno, &err))) return 0;
    if (uwph_sigismember(&t->sigmask, sig) == 1 || t->in_handler > 2) return 0;

    if (was_guest) uwph_teb_leave_guest(uwph_teb_current(), &t->teb_swap);
    uwph_context_to_ucontext(ctx, &uc);
    uc.uc_mcontext.gregs[UWPH_REG_TRAPNO] = trapno;
    uc.uc_mcontext.gregs[UWPH_REG_ERR] = err;
    uc.uc_mcontext.gregs[UWPH_REG_CR2] = (int64_t)(uintptr_t)si.u.addr;
    /* Windows reports int3 at the instruction, Linux after it (Wine's
       handler subtracts 1 again) */
    if (e->code == UWPH_EXC_BREAKPOINT) uc.uc_mcontext.gregs[UWPH_REG_RIP]++;

    handled = run_handler(sig, &si, &uc);
    if (handled) uwph_ucontext_to_context(&uc, ctx);
    if (was_guest) uwph_teb_enter_guest(uwph_teb_current(), &t->teb_swap);
    return handled;
}

void uwph_deliver_pending_signals(void *ctx)
{
    struct uwph_thread *t = uwph_thread_current();
    uint64_t pend;
    while ((pend = __atomic_load_n(&t->pending, __ATOMIC_ACQUIRE))) {
        int sig = 0;
        for (int s = 1; s < 64; s++)
            if ((pend >> (s - 1)) & 1 && uwph_sigismember(&t->sigmask, s) != 1) { sig = s; break; }
        if (!sig) return;
        __atomic_and_fetch(&t->pending, ~((uint64_t)1 << (sig - 1)), __ATOMIC_ACQ_REL);
        {
            uwph_siginfo_t si;
            uwph_ucontext_t uc;
            memset(&si, 0, sizeof(si));
            si.si_signo = sig;
            si.si_code = UWPH_SI_TKILL;
            si.u.kill.pid = uwph_getpid();
            if (ctx) uwph_context_to_ucontext(ctx, &uc);
            else memset(&uc, 0, sizeof(uc));
            if (run_handler(sig, &si, &uc) && ctx) uwph_ucontext_to_context(&uc, ctx);
        }
    }
}

/* ---- host exception hook ---- */
#ifdef UWPH_BACKEND_WIN32
_Static_assert(sizeof(uwph_win_context) == sizeof(CONTEXT), "CONTEXT layout");
_Static_assert(offsetof(uwph_win_context, Rip) == offsetof(CONTEXT, Rip), "CONTEXT.Rip");
_Static_assert(offsetof(uwph_win_context, FltSave) == offsetof(CONTEXT, FltSave), "CONTEXT.FltSave");
_Static_assert(offsetof(uwph_win_context, EFlags) == offsetof(CONTEXT, EFlags), "CONTEXT.EFlags");

#ifdef UWPH_DESKTOP_SIGNAL_TEST
static LONG CALLBACK veh(EXCEPTION_POINTERS *ep)
{
    const EXCEPTION_RECORD *r = ep->ExceptionRecord;
    struct uwph_exception e;
    e.code = r->ExceptionCode;
    e.address = (uint64_t)(uintptr_t)r->ExceptionAddress;
    e.nparams = r->NumberParameters;
    for (int i = 0; i < 4; i++) e.info[i] = i < (int)r->NumberParameters ? r->ExceptionInformation[i] : 0;
    return uwph_dispatch_exception(&e, ep->ContextRecord) ? EXCEPTION_CONTINUE_EXECUTION
                                                          : EXCEPTION_CONTINUE_SEARCH;
}

typedef PVOID (WINAPI *add_veh_fn)(ULONG, PVECTORED_EXCEPTION_HANDLER);

int uwph_signal_init(void)
{
    /* documented "desktop apps only": resolved at runtime, see probe.h */
    add_veh_fn add = (add_veh_fn)uwph_resolve("AddVectoredExceptionHandler");
    if (!add || !add(1, veh)) {
        uwph_log("uwph: no vectored exception handler; synchronous signals unavailable\n");
        return UWPH_FAIL(UWPH_ENOSYS);
    }
    return 0;
}
#else
int uwph_signal_init(void)
{
    uwph_log("uwph: UWP cooperative signals only; exception registration disabled\n");
    return 0;
}
#endif
#else
int uwph_signal_init(void) { return 0; }   /* stub: nothing raises exceptions */
#endif
