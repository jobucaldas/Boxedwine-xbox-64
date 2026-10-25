/* One hosted server per process; Wine's server globals are not restartable.
 * Start before client threads, use absolute client paths, join before teardown.
 * The retained Wine cwd policy is documented in NOTES-x64.md (O6).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

extern int wine_server_main(int, char **);
extern void uwph_server_request_stop(void);
static pthread_t server_thread;
static int started, ready_write = -1, saved_cwd = -1;
static _Thread_local void (*cleanup[16])(void);
static _Thread_local unsigned cleanup_count;

int uwph_host_atexit(void (*fn)(void))
{
    if (cleanup_count == 16) return -1;
    cleanup[cleanup_count++] = fn;
    return 0;
}
_Noreturn void uwph_host_exit(int rc)
{
    while (cleanup_count) cleanup[--cleanup_count]();
    if (ready_write != -1) { close(ready_write); ready_write = -1; }
    pthread_exit((void *)(intptr_t)rc);
}
void uwph_host_ready(void)
{
    char ok = 'R';
    if (write(ready_write, &ok, 1) != 1) uwph_host_exit(1);
    close(ready_write);
    ready_write = -1;
}
static void *server_main(void *unused)
{
    char *argv[] = { "uwph-wineserver", "-f", "-p", NULL };
    sigset_t mask;
    (void)unused;
    /* A broken socket must report EPIPE without changing client dispositions. */
    sigemptyset(&mask);
    sigaddset(&mask, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &mask, NULL);
    uwph_host_exit(wine_server_main(3, argv));
}
int uwph_server_start(const char *prefix, int *ready_fd)
{
    int p[2], rc;
    if (!prefix || prefix[0] != '/' || !ready_fd) return EINVAL;
    if (started) return EALREADY;
    saved_cwd = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (saved_cwd == -1) return errno;
    if (setenv("WINEPREFIX", prefix, 1) || pipe2(p, O_CLOEXEC))
    { rc = errno; close(saved_cwd); return rc; }
    ready_write = p[1];
    rc = pthread_create(&server_thread, NULL, server_main, NULL);
    if (rc) { close(p[0]); close(p[1]); close(saved_cwd); return rc; }
    started = 1;
    *ready_fd = p[0];
    return 0;
}
/* Call only after consuming the ready byte. Runs shutdown on the server thread. */
int uwph_server_stop_and_join(void)
{
    void *result;
    int rc;
    uwph_server_request_stop();
    if ((rc = pthread_join(server_thread, &result))) return rc;
    if (fchdir(saved_cwd)) return errno;
    close(saved_cwd);
    return (int)(intptr_t)result;
}
/* Startup failure: no ready byte, so there is no initialized control pipe. */
int uwph_server_join_failed(void)
{
    void *result;
    int rc = pthread_join(server_thread, &result);
    fchdir(saved_cwd);
    close(saved_cwd);
    return rc ? rc : (int)(intptr_t)result;
}
