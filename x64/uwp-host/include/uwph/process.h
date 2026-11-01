/*
 * Process identity and the process model.
 *
 * A UWP app cannot fork or create arbitrary processes, so the host is a
 * single process: one Wine "Windows process" (the game) plus wineserver on
 * a host thread. fork/execve/posix_spawn/waitpid fail with ENOSYS; Wine's
 * CreateProcess of a second Windows process therefore fails cleanly
 * (launchers/crash reporters are a known limitation, see NOTES-x64.md).
 */
#ifndef UWPH_PROCESS_H
#define UWPH_PROCESS_H
#ifdef __cplusplus
extern "C" {
#endif

struct uwph_passwd { char *pw_name; char *pw_passwd; uwph_uid_t pw_uid; uwph_gid_t pw_gid;
                     char *pw_gecos; char *pw_dir; char *pw_shell; };
struct uwph_rusage;
struct uwph_tms;

UWPH_API uwph_pid_t uwph_getpid(void);
UWPH_API uwph_uid_t uwph_getuid(void);
UWPH_API struct uwph_passwd *uwph_getpwuid(uwph_uid_t uid);
UWPH_API uwph_pid_t uwph_setsid(void);
UWPH_API int  uwph_uname(struct uwph_utsname *u);
UWPH_API int  uwph_sysinfo(struct uwph_sysinfo *si);
UWPH_API long uwph_sysconf(int name);
UWPH_API unsigned long uwph_getauxval(unsigned long type);
UWPH_API int  uwph_getrlimit(int res, struct uwph_rlimit *rl);
UWPH_API int  uwph_setrlimit(int res, const struct uwph_rlimit *rl);
UWPH_API int  uwph_getrusage(int who, struct uwph_rusage *ru);
UWPH_API int  uwph_prctl(int op, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);
UWPH_API uwph_ssize_t uwph_getrandom(void *buf, size_t len, unsigned int flags);
UWPH_API char *uwph_getenv(const char *name);
UWPH_API char *uwph_secure_getenv(const char *name);
UWPH_API int  uwph_setenv(const char *name, const char *val, int overwrite);
UWPH_API int  uwph_unsetenv(const char *name);
UWPH_API int  uwph_putenv(char *s);
UWPH_API void uwph__exit(int code);
UWPH_API void uwph_exit(int code);
UWPH_API void uwph_abort(void);
/* not available in a UWP app: ENOSYS */
UWPH_API uwph_pid_t uwph_fork(void);
UWPH_API int uwph_execve(const char *path, char *const argv[], char *const envp[]);
UWPH_API int uwph_execv(const char *path, char *const argv[]);
UWPH_API int uwph_execvp(const char *file, char *const argv[]);
UWPH_API int uwph_posix_spawn(uwph_pid_t *pid, const char *path, const void *fa, const void *attr,
                              char *const argv[], char *const envp[]);
UWPH_API uwph_pid_t uwph_waitpid(uwph_pid_t pid, int *status, int options);
UWPH_API long uwph_ptrace(long req, uwph_pid_t pid, void *addr, void *data);

#ifdef __cplusplus
}
#endif
#endif
