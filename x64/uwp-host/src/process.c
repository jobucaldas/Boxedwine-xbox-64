/* Process identity, environment, limits, and the (absent) process model. */
#include "internal.h"
#include <stdio.h>
#ifdef UWPH_BACKEND_WIN32
# include <bcrypt.h>
#endif

#define UWPH_UID 1000

UWPH_API uwph_pid_t uwph_getpid(void)
{
#ifdef UWPH_BACKEND_WIN32
    return (uwph_pid_t)GetCurrentProcessId();
#else
    return 1;
#endif
}

UWPH_API uwph_uid_t uwph_getuid(void)
{
    uwph_trace_hit(UWPH_TRACE_GETUID);
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    UWPH_NATIVE(uwph_uid_t, "getuid", (void));
    UWPH_NATIVE_RETURN(fn());
#endif
    return UWPH_UID;
}

UWPH_API struct uwph_passwd *uwph_getpwuid(uwph_uid_t uid)
{
    static struct uwph_passwd pw = { "xbox", "x", UWPH_UID, UWPH_UID, "", "/home/xbox", "/bin/false" };
    if (uid != UWPH_UID) return UWPH_FAILP(UWPH_ENOENT);
    return &pw;
}

UWPH_API uwph_pid_t uwph_setsid(void) { return uwph_getpid(); }

UWPH_API int uwph_uname(struct uwph_utsname *u)
{
    memset(u, 0, sizeof(*u));
    strcpy(u->sysname, "uwph");          /* a new Wine host OS, not Linux */
    strcpy(u->nodename, "xbox");
    snprintf(u->release, sizeof(u->release), "%d.%d", UWPH_VERSION_MAJOR, UWPH_VERSION_MINOR);
    strcpy(u->version, uwph_backend_name());
    strcpy(u->machine, "x86_64");
    return 0;
}

UWPH_API int uwph_sysinfo(struct uwph_sysinfo *si)
{
#ifdef UWPH_BACKEND_WIN32
    MEMORYSTATUSEX ms; memset(&ms, 0, sizeof(ms)); ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms)) return UWPH_FAIL(UWPH_EIO);
    memset(si, 0, sizeof(*si));
    si->uptime = (int64_t)(GetTickCount64() / 1000);
    si->totalram = ms.ullTotalPhys;
    si->freeram = ms.ullAvailPhys;
    si->totalswap = ms.ullTotalPageFile - ms.ullTotalPhys;
    si->freeswap = ms.ullAvailPageFile > ms.ullAvailPhys ? ms.ullAvailPageFile - ms.ullAvailPhys : 0;
    si->procs = 1;
    si->mem_unit = 1;
    return 0;
#else
    (void)si;
    return UWPH_ENOSYS_INT();
#endif
}

UWPH_API long uwph_sysconf(int name)
{
    uwph_trace_hit(UWPH_TRACE_SYSCONF);
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    UWPH_NATIVE(long, "sysconf", (int));
    UWPH_NATIVE_RETURN(fn(name));
#endif

    switch (name) {
    case UWPH_SC_PAGESIZE: return uwph_mem_page_size();
    case UWPH_SC_CLK_TCK:  return 100;
    case UWPH_SC_OPEN_MAX: return UWPH_MAX_FDS;
#ifdef UWPH_BACKEND_WIN32
    case UWPH_SC_NPROCESSORS_CONF:
    case UWPH_SC_NPROCESSORS_ONLN: { SYSTEM_INFO s; GetSystemInfo(&s); return (long)s.dwNumberOfProcessors; }
    case UWPH_SC_PHYS_PAGES: {
        MEMORYSTATUSEX ms; memset(&ms, 0, sizeof(ms)); ms.dwLength = sizeof(ms);
        return GlobalMemoryStatusEx(&ms) ? (long)(ms.ullTotalPhys / 4096) : -1;
    }
#else
    case UWPH_SC_NPROCESSORS_CONF: case UWPH_SC_NPROCESSORS_ONLN: case UWPH_SC_PHYS_PAGES:
        return UWPH_ENOSYS_INT();
#endif
    default: return UWPH_FAIL(UWPH_EINVAL);
    }
}

UWPH_API unsigned long uwph_getauxval(unsigned long type)
{
    switch (type) {
    case UWPH_AT_PAGESZ:      return (unsigned long)uwph_mem_page_size();
    case UWPH_AT_MINSIGSTKSZ: return 2048;
    default: uwph_errno = UWPH_ENOENT; return 0;   /* no vDSO, no HWCAP: Wine uses cpuid */
    }
}

static struct uwph_rlimit nofile = { UWPH_MAX_FDS, UWPH_MAX_FDS };

UWPH_API int uwph_getrlimit(int res, struct uwph_rlimit *rl)
{
    uwph_trace_hit(UWPH_TRACE_GETRLIMIT);
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    UWPH_NATIVE(int, "getrlimit", (int, struct uwph_rlimit *));
    UWPH_NATIVE_RETURN(fn(res, rl));
#endif

    switch (res) {
    case UWPH_RLIMIT_NOFILE: *rl = nofile; return 0;
    case UWPH_RLIMIT_STACK:  rl->rlim_cur = 8 << 20; rl->rlim_max = UWPH_RLIM_INFINITY; return 0;
    case UWPH_RLIMIT_AS: case UWPH_RLIMIT_DATA:
        rl->rlim_cur = rl->rlim_max = UWPH_RLIM_INFINITY; return 0;
    default: return UWPH_FAIL(UWPH_EINVAL);
    }
}

UWPH_API int uwph_setrlimit(int res, const struct uwph_rlimit *rl)
{
    uwph_trace_hit(UWPH_TRACE_SETRLIMIT);
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    UWPH_NATIVE(int, "setrlimit", (int, const struct uwph_rlimit *));
    UWPH_NATIVE_RETURN(fn(res, rl));
#endif

    if (res != UWPH_RLIMIT_NOFILE) return UWPH_ENOSYS_INT();
    if (rl->rlim_cur > rl->rlim_max) return UWPH_FAIL(UWPH_EINVAL);
    if (rl->rlim_cur > UWPH_MAX_FDS || rl->rlim_max > UWPH_MAX_FDS) return UWPH_FAIL(UWPH_EPERM);
    nofile = *rl;
    return 0;
}

UWPH_API int uwph_getrusage(int who, struct uwph_rusage *ru) { (void)who; (void)ru; return UWPH_ENOSYS_INT(); }

UWPH_API int uwph_prctl(int op, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    struct uwph_thread *t = uwph_thread_current();
    (void)a3; (void)a4; (void)a5;
    switch (op) {
    case UWPH_PR_SET_NAME:
        strncpy(t->name, (const char *)(uintptr_t)a2, sizeof(t->name) - 1);
        return 0;
    case UWPH_PR_GET_NAME:
        memcpy((char *)(uintptr_t)a2, t->name, sizeof(t->name));
        return 0;
    default:
        return UWPH_FAIL(UWPH_EINVAL);   /* incl. PR_SET_SYSCALL_USER_DISPATCH: no such thing here */
    }
}

UWPH_API uwph_ssize_t uwph_getrandom(void *buf, size_t len, unsigned int flags)
{
    (void)flags;
#ifdef UWPH_BACKEND_WIN32
    if (len > 0xffffffffu) len = 0xffffffffu;
    if (BCryptGenRandom(NULL, buf, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG)) return UWPH_FAIL(UWPH_EIO);
    return (uwph_ssize_t)len;
#else
    (void)buf; (void)len;
    return UWPH_ENOSYS_INT();
#endif
}

/* ---- environment: our own table (the app process has almost none) ---- */
#define MAX_ENV 256
static char *env_store[MAX_ENV + 1];
char **uwph_environ = env_store;
static uwph_lock_t env_lock = UWPH_LOCK_INIT;

static int env_find(const char *name, size_t n)
{
    for (int i = 0; env_store[i]; i++)
        if (!strncmp(env_store[i], name, n) && env_store[i][n] == '=') return i;
    return -1;
}

UWPH_API char *uwph_getenv(const char *name)
{
    uwph_trace_hit(UWPH_TRACE_GETENV);
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    UWPH_NATIVE(char *, "getenv", (const char *));
    UWPH_NATIVE_RETURN(fn(name));
#endif

    char *r = NULL;
    int i;
    uwph_lock(&env_lock);
    if ((i = env_find(name, strlen(name))) >= 0) r = env_store[i] + strlen(name) + 1;
    uwph_unlock(&env_lock);
    return r;
}

UWPH_API char *uwph_secure_getenv(const char *name) { return uwph_getenv(name); }

static int env_put(char *entry, size_t namelen, int overwrite)
{
    int i, n;
    uwph_lock(&env_lock);
    if ((i = env_find(entry, namelen)) >= 0) {
        if (overwrite) env_store[i] = entry;   /* old string leaks, like glibc's putenv model */
        uwph_unlock(&env_lock);
        return 0;
    }
    for (n = 0; env_store[n]; n++);
    if (n == MAX_ENV) { uwph_unlock(&env_lock); return UWPH_FAIL(UWPH_ENOMEM); }
    env_store[n] = entry;
    env_store[n + 1] = NULL;
    uwph_unlock(&env_lock);
    return 0;
}

UWPH_API int uwph_setenv(const char *name, const char *val, int overwrite)
{
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    /* Same native table as the passthrough getenv (uwph_environ is not it). */
    UWPH_NATIVE(int, "setenv", (const char *, const char *, int));
    UWPH_NATIVE_RETURN(fn(name, val, overwrite));
#endif
    size_t nl = strlen(name);
    char *e;
    if (!nl || strchr(name, '=')) return UWPH_FAIL(UWPH_EINVAL);
    if (!(e = malloc(nl + strlen(val) + 2))) return UWPH_FAIL(UWPH_ENOMEM);
    sprintf(e, "%s=%s", name, val);
    return env_put(e, nl, overwrite);
}

UWPH_API int uwph_putenv(char *s)
{
    uwph_trace_hit(UWPH_TRACE_PUTENV);

#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    UWPH_NATIVE(int, "putenv", (char *));
    UWPH_NATIVE_RETURN(fn(s));
#endif
    char *eq = strchr(s, '=');
    if (!eq) return uwph_unsetenv(s);
    return env_put(s, (size_t)(eq - s), 1);
}

UWPH_API int uwph_unsetenv(const char *name)
{
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    UWPH_NATIVE(int, "unsetenv", (const char *));
    UWPH_NATIVE_RETURN(fn(name));
#endif
    int i;
    uwph_lock(&env_lock);
    if ((i = env_find(name, strlen(name))) >= 0)
        for (; env_store[i]; i++) env_store[i] = env_store[i + 1];
    uwph_unlock(&env_lock);
    return 0;
}

UWPH_API void uwph__exit(int code)
{
#ifdef UWPH_BACKEND_WIN32
    TerminateProcess(GetCurrentProcess(), (UINT)code);
#endif
    _Exit(code);
}

UWPH_API void uwph_exit(int code) { exit(code); }
UWPH_API void uwph_abort(void) { abort(); }

/* ---- no process creation in a UWP app ---- */
UWPH_API uwph_pid_t uwph_fork(void) { return UWPH_ENOSYS_INT(); }
UWPH_API int uwph_execve(const char *p, char *const a[], char *const e[]) { (void)p; (void)a; (void)e; return UWPH_ENOSYS_INT(); }
UWPH_API int uwph_execv(const char *p, char *const a[])
{
    uwph_trace_hit(UWPH_TRACE_EXECV);
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    UWPH_NATIVE(int, "execv", (const char *, char *const []));
    UWPH_NATIVE_RETURN(fn(p, a));
#endif
    (void)p; (void)a;
    return UWPH_ENOSYS_INT();
}
UWPH_API int uwph_execvp(const char *f, char *const a[]) { (void)f; (void)a; return UWPH_ENOSYS_INT(); }
UWPH_API int uwph_posix_spawn(uwph_pid_t *pid, const char *path, const void *fa, const void *attr,
                              char *const argv[], char *const envp[])
{ (void)pid; (void)path; (void)fa; (void)attr; (void)argv; (void)envp; return UWPH_ENOSYS; }
UWPH_API uwph_pid_t uwph_waitpid(uwph_pid_t pid, int *status, int options)
{ (void)pid; (void)status; (void)options; return UWPH_FAIL(UWPH_ECHILD); }
UWPH_API long uwph_ptrace(long req, uwph_pid_t pid, void *addr, void *data)
{ (void)req; (void)pid; (void)addr; (void)data; return UWPH_ENOSYS_INT(); }
