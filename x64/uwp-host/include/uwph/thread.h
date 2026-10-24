/*
 * Threads and low-level sync used by ntdll.so (thread.c, sync.c, server.c)
 * and wineserver.
 *
 * Wine allocates every thread stack itself through its virtual memory code and
 * passes it with pthread_attr_setstack(); the host creates the thread with
 * CreateThread (small host stack) and switches RSP to the Wine stack before
 * calling the start routine. futex() -> WaitOnAddress / WakeByAddress*.
 */
#ifndef UWPH_THREAD_H
#define UWPH_THREAD_H
#ifdef __cplusplus
extern "C" {
#endif

typedef struct uwph_thread *uwph_pthread_t;
typedef struct {
    void  *stack;
    size_t stack_size;
    size_t guard_size;
    int    scope;
} uwph_pthread_attr_t;
typedef struct { void *impl; int type; } uwph_pthread_mutex_t;   /* SRWLOCK-backed */
typedef struct { int type; } uwph_pthread_mutexattr_t;
typedef struct { void *impl; } uwph_pthread_cond_t;              /* CONDITION_VARIABLE-backed */
typedef unsigned int uwph_pthread_key_t;

#define UWPH_PTHREAD_MUTEX_NORMAL    0
#define UWPH_PTHREAD_MUTEX_RECURSIVE 1

UWPH_API int  uwph_pthread_attr_init(uwph_pthread_attr_t *a);
UWPH_API int  uwph_pthread_attr_destroy(uwph_pthread_attr_t *a);
UWPH_API int  uwph_pthread_attr_setstack(uwph_pthread_attr_t *a, void *stack, size_t size);
UWPH_API int  uwph_pthread_attr_setguardsize(uwph_pthread_attr_t *a, size_t size);
UWPH_API int  uwph_pthread_attr_setscope(uwph_pthread_attr_t *a, int scope);
UWPH_API int  uwph_pthread_create(uwph_pthread_t *t, const uwph_pthread_attr_t *a,
                                  void *(*start)(void *), void *arg);
UWPH_API int  uwph_pthread_join(uwph_pthread_t t, void **ret);
UWPH_API void uwph_pthread_exit(void *ret);
UWPH_API uwph_pthread_t uwph_pthread_self(void);
UWPH_API int  uwph_pthread_key_create(uwph_pthread_key_t *k, void (*dtor)(void *));
UWPH_API void *uwph_pthread_getspecific(uwph_pthread_key_t k);
UWPH_API int  uwph_pthread_setspecific(uwph_pthread_key_t k, const void *v);
UWPH_API int  uwph_pthread_mutexattr_init(uwph_pthread_mutexattr_t *a);
UWPH_API int  uwph_pthread_mutexattr_destroy(uwph_pthread_mutexattr_t *a);
UWPH_API int  uwph_pthread_mutexattr_settype(uwph_pthread_mutexattr_t *a, int type);
UWPH_API int  uwph_pthread_mutex_init(uwph_pthread_mutex_t *m, const uwph_pthread_mutexattr_t *a);
UWPH_API int  uwph_pthread_mutex_lock(uwph_pthread_mutex_t *m);
UWPH_API int  uwph_pthread_mutex_unlock(uwph_pthread_mutex_t *m);
UWPH_API int  uwph_pthread_cond_wait(uwph_pthread_cond_t *c, uwph_pthread_mutex_t *m);
UWPH_API int  uwph_pthread_cond_broadcast(uwph_pthread_cond_t *c);
UWPH_API int  uwph_sched_yield(void);
UWPH_API int  uwph_sched_getcpu(void);
UWPH_API int  uwph_sched_getaffinity(uwph_pid_t pid, size_t size, void *mask);
UWPH_API int  uwph_sched_setaffinity(uwph_pid_t pid, size_t size, const void *mask);

/* syscall-level helpers (reached through uwph_syscall too) */
UWPH_API int  uwph_gettid(void);
UWPH_API long uwph_futex(int *uaddr, int op, int val, const struct uwph_timespec *timeout,
                         int *uaddr2, int val3);
UWPH_API int  uwph_tgkill(uwph_pid_t tgid, int tid, int sig);

#ifdef __cplusplus
}
#endif
#endif
