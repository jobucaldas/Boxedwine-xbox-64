/*
 * Threads: per-thread host state, pthread subset, futex, tgkill.
 */
#include "internal.h"

/* fn(arg) on stack_top; SysV on both builds (rdi=fn, rsi=arg, rdx=stack) */
__asm__(".text\n"
        ".globl uwph_call_on_stack\n"
        "uwph_call_on_stack:\n\t"
        "pushq %rbp\n\t"
        "movq %rsp, %rbp\n\t"
        "movq %rdx, %rsp\n\t"
        "andq $-16, %rsp\n\t"
        "movq %rdi, %rax\n\t"
        "movq %rsi, %rdi\n\t"
        "callq *%rax\n\t"
        "movq %rbp, %rsp\n\t"
        "popq %rbp\n\t"
        "ret\n");

/* ---- registry (tid -> state), for tgkill ---- */
#define MAX_THREADS 1024
static struct uwph_thread *registry[MAX_THREADS];
static uwph_lock_t reg_lock = UWPH_LOCK_INIT;
static __thread struct uwph_thread *self;

#ifdef UWPH_BACKEND_WIN32
/* Stack-owned waiters are published and removed under reg_lock. All users
 * finish touching them before removal; sequence closes the check/sleep race. */
struct futex_waiter {
    struct futex_waiter *next;
    struct uwph_thread *thread;
    int *addr;
    int sequence, woken;
};
static struct futex_waiter *waiters;
#endif

static int new_tid(void)
{
#ifdef UWPH_BACKEND_WIN32
    return (int)GetCurrentThreadId();
#else
    static volatile int next = 1000;
    return __atomic_fetch_add(&next, 1, __ATOMIC_RELAXED);
#endif
}

static void register_thread(struct uwph_thread *t)
{
    uwph_lock(&reg_lock);
    for (int i = 0; i < MAX_THREADS; i++) if (!registry[i]) { registry[i] = t; break; }
    uwph_unlock(&reg_lock);
}

static __attribute__((unused)) void unregister_thread(struct uwph_thread *t)
{
    uwph_lock(&reg_lock);
    for (int i = 0; i < MAX_THREADS; i++) if (registry[i] == t) registry[i] = NULL;
    uwph_unlock(&reg_lock);
}

struct uwph_thread *uwph_thread_current(void)
{
    if (!self) {
        /* a thread uwph did not create (app main thread): adopt it */
        self = calloc(1, sizeof(*self));
        self->tid = new_tid();
        register_thread(self);
    }
    return self;
}

struct uwph_thread *uwph_thread_find(int tid)
{
    struct uwph_thread *t = NULL;
    uwph_lock(&reg_lock);
    for (int i = 0; i < MAX_THREADS; i++) if (registry[i] && registry[i]->tid == tid) { t = registry[i]; break; }
    uwph_unlock(&reg_lock);
    return t;
}

UWPH_API int uwph_gettid(void) {
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    return uwph_linux_gettid();
#else
    return uwph_thread_current()->tid;
#endif
}

UWPH_API int uwph_tgkill(uwph_pid_t tgid, int tid, int sig)
{
    struct uwph_thread *t;
    if (tgid != uwph_getpid()) return UWPH_FAIL(UWPH_ESRCH);
    if (sig < 0 || sig >= 64) return UWPH_FAIL(UWPH_EINVAL);
    /* Keep registry lifetime protection through pending publication and kick. */
    uwph_lock(&reg_lock);
    t = NULL;
    for (int i = 0; i < MAX_THREADS; i++)
        if (registry[i] && registry[i]->tid == tid) { t = registry[i]; break; }
    if (!t) { uwph_unlock(&reg_lock); return UWPH_FAIL(UWPH_ESRCH); }
    if (sig) {
        __atomic_or_fetch(&t->pending, (uint64_t)1 << (sig - 1), __ATOMIC_ACQ_REL);
#ifdef UWPH_BACKEND_WIN32
        for (struct futex_waiter *w = waiters; w; w = w->next) if (w->thread == t) {
            __atomic_add_fetch(&w->sequence, 1, __ATOMIC_RELEASE);
            WakeByAddressAll(&w->sequence);
        }
        WakeByAddressAll((PVOID)&t->pending);
#endif
    }
    uwph_unlock(&reg_lock);
    if (sig && t == self) uwph_deliver_pending_signals(NULL);
    return 0;
}

/* ---- pthread attrs ---- */
UWPH_API int uwph_pthread_attr_init(uwph_pthread_attr_t *a) { memset(a, 0, sizeof(*a)); return 0; }
UWPH_API int uwph_pthread_attr_destroy(uwph_pthread_attr_t *a) { (void)a; return 0; }
UWPH_API int uwph_pthread_attr_setstack(uwph_pthread_attr_t *a, void *stack, size_t size)
{ a->stack = stack; a->stack_size = size; return 0; }
UWPH_API int uwph_pthread_attr_setguardsize(uwph_pthread_attr_t *a, size_t size) { a->guard_size = size; return 0; }
UWPH_API int uwph_pthread_attr_setscope(uwph_pthread_attr_t *a, int scope) { a->scope = scope; return 0; }

/* ---- keys ---- */
static void (*key_dtors[128])(void *);
static volatile int key_used[128];

UWPH_API int uwph_pthread_key_create(uwph_pthread_key_t *k, void (*dtor)(void *))
{
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    UWPH_NATIVE(int, "pthread_key_create", (uwph_pthread_key_t *, void (*)(void *)));
    UWPH_NATIVE_RETURN(fn(k, dtor));
#endif

    for (int i = 0; i < 128; i++) {
        int expect = 0;
        if (__atomic_compare_exchange_n(&key_used[i], &expect, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            key_dtors[i] = dtor;
            *k = (uwph_pthread_key_t)i;
            return 0;
        }
    }
    return UWPH_EAGAIN;
}
UWPH_API void *uwph_pthread_getspecific(uwph_pthread_key_t k)
{
    uwph_trace_hit(UWPH_TRACE_PTHREAD_GETSPECIFIC);
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    UWPH_NATIVE(void *, "pthread_getspecific", (uwph_pthread_key_t));
    UWPH_NATIVE_RETURN(fn(k));
#endif
    return k < 128 ? uwph_thread_current()->keys[k] : NULL;
}
UWPH_API int uwph_pthread_setspecific(uwph_pthread_key_t k, const void *v)
{
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    UWPH_NATIVE(int, "pthread_setspecific", (uwph_pthread_key_t, const void *));
    UWPH_NATIVE_RETURN(fn(k, v));
#endif

    if (k >= 128) return UWPH_EINVAL;
    uwph_thread_current()->keys[k] = (void *)v;
    return 0;
}

static void run_key_dtors(struct uwph_thread *t)
{
    for (int i = 0; i < 128; i++)
        if (t->keys[i] && key_dtors[i]) { void *v = t->keys[i]; t->keys[i] = NULL; key_dtors[i](v); }
}

/* ---- mutex / cond (lazily allocated so zero-initialised statics work) ---- */
struct mutex_impl { uwph_lock_t l; volatile int owner; int count; };

static struct mutex_impl *mutex_get(uwph_pthread_mutex_t *m)
{
    struct mutex_impl *mi = __atomic_load_n((struct mutex_impl **)&m->impl, __ATOMIC_ACQUIRE), *n;
    void *expect = NULL;
    if (mi) return mi;
    n = calloc(1, sizeof(*n));
    {
        uwph_lock_t init = UWPH_LOCK_INIT;
        n->l = init;
    }
    if (__atomic_compare_exchange_n(&m->impl, &expect, n, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return n;
    free(n);
    return expect;
}

UWPH_API int uwph_pthread_mutexattr_init(uwph_pthread_mutexattr_t *a) { a->type = UWPH_PTHREAD_MUTEX_NORMAL; return 0; }
UWPH_API int uwph_pthread_mutexattr_destroy(uwph_pthread_mutexattr_t *a) { (void)a; return 0; }
UWPH_API int uwph_pthread_mutexattr_settype(uwph_pthread_mutexattr_t *a, int type) { a->type = type; return 0; }
UWPH_API int uwph_pthread_mutex_init(uwph_pthread_mutex_t *m, const uwph_pthread_mutexattr_t *a)
{
    m->impl = NULL;
    m->type = a ? a->type : UWPH_PTHREAD_MUTEX_NORMAL;
    mutex_get(m);
    return 0;
}
UWPH_API int uwph_pthread_mutex_lock(uwph_pthread_mutex_t *m)
{
    uwph_trace_hit(UWPH_TRACE_PTHREAD_MUTEX_LOCK);
    struct mutex_impl *mi = mutex_get(m);
    int me = uwph_gettid();
    if (m->type == UWPH_PTHREAD_MUTEX_RECURSIVE && mi->owner == me) { mi->count++; return 0; }
    uwph_lock(&mi->l);
    mi->owner = me;
    mi->count = 1;
    return 0;
}
UWPH_API int uwph_pthread_mutex_unlock(uwph_pthread_mutex_t *m)
{
    uwph_trace_hit(UWPH_TRACE_PTHREAD_MUTEX_UNLOCK);
    struct mutex_impl *mi = mutex_get(m);
    if (mi->owner != uwph_gettid()) return UWPH_EPERM;
    if (--mi->count) return 0;
    mi->owner = 0;
    uwph_unlock(&mi->l);
    return 0;
}

static uwph_cond_t *cond_get(uwph_pthread_cond_t *c)
{
    uwph_cond_t *ci = __atomic_load_n((uwph_cond_t **)&c->impl, __ATOMIC_ACQUIRE), *n;
    void *expect = NULL;
    uwph_cond_t init = UWPH_COND_INIT;
    if (ci) return ci;
    n = malloc(sizeof(*n));
    *n = init;
    if (__atomic_compare_exchange_n(&c->impl, &expect, n, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return n;
    free(n);
    return expect;
}
UWPH_API int uwph_pthread_cond_wait(uwph_pthread_cond_t *c, uwph_pthread_mutex_t *m)
{
    struct mutex_impl *mi = mutex_get(m);
    int count = mi->count, me = mi->owner;
    mi->owner = 0;
    mi->count = 0;
    uwph_cond_wait(cond_get(c), &mi->l, -1);
    mi->owner = me;
    mi->count = count;
    return 0;
}
UWPH_API int uwph_pthread_cond_broadcast(uwph_pthread_cond_t *c) { uwph_cond_broadcast(cond_get(c)); return 0; }

/* ---- threads ---- */
static __attribute__((sysv_abi, unused)) void *thread_body(void *p)
{
    struct uwph_thread *t = p;
    t->ret = t->start(t->arg);
    return t->ret;
}

#ifdef UWPH_BACKEND_WIN32
static DWORD WINAPI thread_entry(LPVOID p)
{
    struct uwph_thread *t = p;
    self = t;
    if (t->stack)   /* Wine's own stack (pthread_attr_setstack) */
        uwph_call_on_stack(thread_body, t, (char *)t->stack + t->stack_size);
    else
        thread_body(t);
    run_key_dtors(t);
    unregister_thread(t);
    return 0;
}

UWPH_API int uwph_pthread_create(uwph_pthread_t *out, const uwph_pthread_attr_t *a,
                                 void *(*start)(void *), void *arg)
{
    struct uwph_thread *t = calloc(1, sizeof(*t));
    DWORD id;
    if (!t) return UWPH_EAGAIN;
    t->start = start;
    t->arg = arg;
    if (a && a->stack) { t->stack = a->stack; t->stack_size = a->stack_size; }
    t->sigmask = uwph_thread_current()->sigmask;   /* inherited, as on Linux */
    /* small host stack: guest work runs on the Wine-provided stack */
    t->handle = CreateThread(NULL, t->stack ? 64 * 1024 : 0, thread_entry, t, CREATE_SUSPENDED, &id);
    if (!t->handle) { free(t); return UWPH_EAGAIN; }
    t->tid = (int)id;
    register_thread(t);
    ResumeThread(t->handle);
    *out = t;
    return 0;
}

UWPH_API int uwph_pthread_join(uwph_pthread_t t, void **ret)
{
    WaitForSingleObjectEx(t->handle, INFINITE, FALSE);
    CloseHandle(t->handle);
    if (ret) *ret = t->ret;
    unregister_thread(t);
    free(t);
    return 0;
}

UWPH_API void uwph_pthread_exit(void *ret)
{
    struct uwph_thread *t = uwph_thread_current();
    t->ret = ret;
    run_key_dtors(t);
    unregister_thread(t);
    ExitThread(0);
}

UWPH_API int uwph_sched_yield(void) { SwitchToThread(); return 0; }
UWPH_API int uwph_sched_getcpu(void) { return (int)GetCurrentProcessorNumber(); }

UWPH_API int uwph_sched_getaffinity(uwph_pid_t pid, size_t size, void *mask)
{
    SYSTEM_INFO si;
    (void)pid;
    GetSystemInfo(&si);
    memset(mask, 0, size);
    for (DWORD i = 0; i < si.dwNumberOfProcessors && i / 8 < size; i++)
        ((uint8_t *)mask)[i / 8] |= (uint8_t)(1 << (i % 8));
    return 0;
}

UWPH_API int uwph_sched_setaffinity(uwph_pid_t pid, size_t size, const void *mask)
{
    DWORD_PTR m = 0;
    struct uwph_thread *t = pid ? uwph_thread_find(pid) : uwph_thread_current();
    memcpy(&m, mask, size < sizeof(m) ? size : sizeof(m));
    if (!t) return UWPH_FAIL(UWPH_ESRCH);
    return SetThreadAffinityMask(t == self ? GetCurrentThread() : t->handle, m) ? 0 : UWPH_FAIL(UWPH_EINVAL);
}

/* Only the target reads its signal mask; senders publish pending atomically. */
static int deliverable(struct uwph_thread *t)
{
    return (__atomic_load_n(&t->pending, __ATOMIC_ACQUIRE) & ~t->sigmask.val[0]) != 0;
}

static int futex_wait(int *uaddr, int val, int64_t timeout_ms)
{
    struct uwph_thread *t = uwph_thread_current();
    struct futex_waiter w = { NULL, t, uaddr, 0, 0 }, **link;
    uint64_t start = GetTickCount64();
    int error = 0, saved_errno = uwph_errno;
    uwph_lock(&reg_lock);
    if (__atomic_load_n(uaddr, __ATOMIC_ACQUIRE) != val) {
        uwph_unlock(&reg_lock);
        return UWPH_FAIL(UWPH_EAGAIN);
    }
    w.next = waiters;
    waiters = &w;
    __atomic_store_n(&t->wait_addr, uaddr, __ATOMIC_RELEASE);
    uwph_unlock(&reg_lock);
    for (;;) {
        int seq = __atomic_load_n(&w.sequence, __ATOMIC_ACQUIRE);
        DWORD slice = 100;
        /* Check after every wake, including a successful host wake. */
        if (deliverable(t)) { error = UWPH_EINTR; break; }
        if (__atomic_load_n(&w.woken, __ATOMIC_ACQUIRE)) break;
        if (timeout_ms >= 0) {
            uint64_t el = GetTickCount64() - start;
            if (el >= (uint64_t)timeout_ms) { error = UWPH_ETIMEDOUT; break; }
            if ((uint64_t)timeout_ms - el < slice) slice = (DWORD)(timeout_ms - el);
        }
        WaitOnAddress(&w.sequence, &seq, sizeof(seq), slice);
    }
    uwph_lock(&reg_lock);
    for (link = &waiters; *link != &w; link = &(*link)->next) {}
    *link = w.next;
    __atomic_store_n(&t->wait_addr, NULL, __ATOMIC_RELEASE);
    uwph_unlock(&reg_lock);
    /* Delivery may call wrappers that change errno. Preserve this operation. */
    if (deliverable(t)) error = UWPH_EINTR;
    uwph_deliver_pending_signals(NULL);
    uwph_errno = error ? error : saved_errno;
    return error ? -1 : 0;
}

UWPH_API long uwph_futex(int *uaddr, int op, int val, const struct uwph_timespec *timeout,
                         int *uaddr2, int val3)
{
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    return uwph_linux_futex(uaddr, op, val, timeout, uaddr2, val3);
#else
    int cmd = op & ~(UWPH_FUTEX_PRIVATE_FLAG | UWPH_FUTEX_CLOCK_REALTIME);
    int64_t ms = -1;
    (void)uaddr2; (void)val3;
    switch (cmd) {
    case UWPH_FUTEX_WAIT:
    case UWPH_FUTEX_WAIT_BITSET:
        if (__atomic_load_n(uaddr, __ATOMIC_ACQUIRE) != val) return UWPH_FAIL(UWPH_EAGAIN);
        if (timeout) {
            ms = timeout->tv_sec * 1000 + timeout->tv_nsec / 1000000;
            if (cmd == UWPH_FUTEX_WAIT_BITSET) {   /* absolute */
                struct uwph_timespec now;
                uwph_clock_gettime(op & UWPH_FUTEX_CLOCK_REALTIME ? UWPH_CLOCK_REALTIME : UWPH_CLOCK_MONOTONIC, &now);
                ms -= now.tv_sec * 1000 + now.tv_nsec / 1000000;
                if (ms < 0) ms = 0;
            }
        }
        return futex_wait(uaddr, val, ms);
    case UWPH_FUTEX_WAKE:
    case UWPH_FUTEX_WAKE_BITSET:
        {
            int count = 0;
            uwph_lock(&reg_lock);
            for (struct futex_waiter *w = waiters; w && count < val; w = w->next)
                if (w->addr == uaddr && !w->woken) {
                    __atomic_store_n(&w->woken, 1, __ATOMIC_RELEASE);
                    __atomic_add_fetch(&w->sequence, 1, __ATOMIC_RELEASE);
                    WakeByAddressAll(&w->sequence);
                    count++;
                }
            uwph_unlock(&reg_lock);
            return count;
        }
    default:
        return UWPH_FAIL(UWPH_ENOSYS);
    }
#endif
}
#else
UWPH_API int uwph_pthread_create(uwph_pthread_t *out, const uwph_pthread_attr_t *a,
                                 void *(*start)(void *), void *arg)
{ (void)out; (void)a; (void)start; (void)arg; return UWPH_ENOSYS; }
UWPH_API int uwph_pthread_join(uwph_pthread_t t, void **ret) { (void)t; (void)ret; return UWPH_ENOSYS; }
UWPH_API void uwph_pthread_exit(void *ret) { (void)ret; run_key_dtors(uwph_thread_current()); uwph__exit(0); }
UWPH_API int uwph_sched_yield(void) { return UWPH_ENOSYS_INT(); }
UWPH_API int uwph_sched_getcpu(void) { return UWPH_ENOSYS_INT(); }
UWPH_API int uwph_sched_getaffinity(uwph_pid_t pid, size_t size, void *mask)
{ (void)pid; (void)size; (void)mask; return UWPH_ENOSYS_INT(); }
UWPH_API int uwph_sched_setaffinity(uwph_pid_t pid, size_t size, const void *mask)
{ (void)pid; (void)size; (void)mask; return UWPH_ENOSYS_INT(); }
UWPH_API long uwph_futex(int *uaddr, int op, int val, const struct uwph_timespec *timeout,
                         int *uaddr2, int val3)
{
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    return uwph_linux_futex(uaddr, op, val, timeout, uaddr2, val3);
#else
    (void)uaddr; (void)op; (void)val; (void)timeout; (void)uaddr2; (void)val3; return UWPH_ENOSYS_INT();
#endif
}
#endif

UWPH_API uwph_pthread_t uwph_pthread_self(void) { return uwph_thread_current(); }
