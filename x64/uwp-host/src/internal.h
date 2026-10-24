/* uwph internals shared by the modules. Not part of the host surface. */
#ifndef UWPH_INTERNAL_H
#define UWPH_INTERNAL_H

#include "uwph/uwph.h"
#include <string.h>
#include <stdlib.h>
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
#include "linux_backend.h"
#endif

#ifdef UWPH_BACKEND_WIN32
# ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
# endif
# include <windows.h>
#else
# include <pthread.h>
#endif

/* set uwph errno and return -1 (or another failure value) */
#define UWPH_FAIL(e)        (uwph_errno = (e), -1)
#define UWPH_FAILP(e)       (uwph_errno = (e), (void *)0)
#define UWPH_ENOSYS_INT()   UWPH_FAIL(UWPH_ENOSYS)
#define UWPH_ENOSYS_PTR()   UWPH_FAILP(UWPH_ENOSYS)

/* log line to the host debug channel (stderr / OutputDebugString) */
void uwph_log(const char *fmt, ...);

/* ---- host sync primitives used inside uwph ---- */
#ifdef UWPH_BACKEND_WIN32
typedef SRWLOCK uwph_lock_t;
typedef CONDITION_VARIABLE uwph_cond_t;
# define UWPH_LOCK_INIT SRWLOCK_INIT
# define UWPH_COND_INIT CONDITION_VARIABLE_INIT
#else
typedef pthread_mutex_t uwph_lock_t;
typedef pthread_cond_t uwph_cond_t;
# define UWPH_LOCK_INIT PTHREAD_MUTEX_INITIALIZER
# define UWPH_COND_INIT PTHREAD_COND_INITIALIZER
#endif
void uwph_lock(uwph_lock_t *l);
void uwph_unlock(uwph_lock_t *l);
/* returns 0 when woken, UWPH_ETIMEDOUT on timeout; timeout_ms < 0 = forever */
int  uwph_cond_wait(uwph_cond_t *c, uwph_lock_t *l, int timeout_ms);
void uwph_cond_broadcast(uwph_cond_t *c);
uint64_t uwph_monotonic_ms(void);

/* internal host threads (wineserver thread, tests) */
typedef struct uwph_host_thread uwph_host_thread;
uwph_host_thread *uwph_host_thread_start(void *(*fn)(void *), void *arg);
void *uwph_host_thread_join(uwph_host_thread *t);

/* ---- refcounted objects behind fds ---- */
struct uwph_object {
    volatile long refs;
    void (*destroy)(struct uwph_object *obj);
    /* called when one fd referring to the object is closed (pipes/sockets
       track their open ends this way); may be NULL */
    void (*fd_closed)(struct uwph_object *obj, int kind);
};
void uwph_obj_init(struct uwph_object *o, void (*destroy)(struct uwph_object *));
struct uwph_object *uwph_obj_ref(struct uwph_object *o);
void uwph_obj_unref(struct uwph_object *o);

/* in-process stream objects (socket.c): used by fd.c for read/write */
uwph_ssize_t uwph_stream_read(struct uwph_object *o, int kind, void *buf, size_t len, int nonblock, int peek);
uwph_ssize_t uwph_stream_write(struct uwph_object *o, int kind, const void *buf, size_t len, int nonblock);
uwph_ssize_t uwph_file_read(struct uwph_object *o, void *buf, size_t len, int64_t off);  /* off<0: current */
int64_t uwph_file_seek(struct uwph_object *o, int64_t off, int whence);
int uwph_file_fstat(struct uwph_object *o, struct uwph_stat *st);
int uwph_epoll_fd_closed(int fd);

/* one lock + condition for all in-process I/O state (pipes, AF_UNIX,
   epoll); every state change broadcasts io_cond, so poll/epoll waiters
   simply re-scan. Simple and correct; per-object waits can come later. */
extern uwph_lock_t uwph_io_lock;
extern uwph_cond_t uwph_io_cond;
int uwph_unix_poll_locked(struct uwph_object *o, int kind);   /* revents mask */
/* absolute, normalised Unix path for an AF_UNIX name (relative to cwd) */
int uwph_fs_abspath(const char *path, char *out, size_t len);

/* ---- Windows x64 CONTEXT layout, usable on the stub build too ---- */
typedef struct { uint64_t lo; int64_t hi; } uwph_m128a;
typedef struct uwph_win_context {
    uint64_t P1Home, P2Home, P3Home, P4Home, P5Home, P6Home;
    uint32_t ContextFlags, MxCsr;
    uint16_t SegCs, SegDs, SegEs, SegFs, SegGs, SegSs;
    uint32_t EFlags;
    uint64_t Dr0, Dr1, Dr2, Dr3, Dr6, Dr7;
    uint64_t Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi;
    uint64_t R8, R9, R10, R11, R12, R13, R14, R15;
    uint64_t Rip;
    uint8_t  FltSave[512] __attribute__((aligned(16)));
    uwph_m128a VectorRegister[26];
    uint64_t VectorControl, DebugControl, LastBranchToRip, LastBranchFromRip,
             LastExceptionToRip, LastExceptionFromRip;
} uwph_win_context;

/* ---- per-thread host state (thread.c) ---- */
struct uwph_thread {
    int tid;
    uwph_sigset_t sigmask;
    uwph_stack_t altstack;
    volatile uint64_t pending;        /* bit (sig-1): queued async signals */
    volatile int *wait_addr;          /* futex word the thread sleeps on (for tgkill wakeups) */
    struct uwph_teb_swap teb_swap;
    uint64_t wine_teb;                /* recorded by arch_prctl(ARCH_SET_GS) */
    int in_handler;
    void *(*start)(void *);
    void *arg, *ret;
    void *stack;
    size_t stack_size;
    void *keys[128];
    char name[16];
#ifdef UWPH_BACKEND_WIN32
    HANDLE handle;
#endif
};
struct uwph_thread *uwph_thread_current(void);
struct uwph_thread *uwph_thread_find(int tid);

/* run fn(arg) on another stack (SysV convention on both builds) */
typedef void *(__attribute__((sysv_abi)) *uwph_stack_fn)(void *);
__attribute__((sysv_abi)) void *uwph_call_on_stack(uwph_stack_fn fn, void *arg, void *stack_top);

/* signals raised by the host itself (tgkill to self, wine_server fatal) */
int uwph_raise_signal(int sig, uwph_siginfo_t *si, uwph_ucontext_t *uc);
int uwph_mem_protect_is_fromapp(void);
void *uwph_resolve(const char *fn);   /* GetProcAddress over kernelbase/kernel32/ntdll */

/* module init hooks */
void uwph_fd_init(void);
int  uwph_signal_init(void);
int  uwph_mem_page_size(void);

#endif
