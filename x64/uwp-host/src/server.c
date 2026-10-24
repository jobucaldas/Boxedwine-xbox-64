/* In-process wineserver thread. */
#include "internal.h"

struct server_args { uwph_server_main_fn fn; };
static volatile int server_state;   /* 0 none, 1 running, 2 exited */
static int server_status;

static void *server_thread(void *p)
{
    struct server_args *a = p;
    char *argv[] = { "wineserver", "--foreground", "--persistent=0", NULL };
    server_state = 1;
    server_status = a->fn(3, argv);
    server_state = 2;
    free(a);
    return NULL;
}

int uwph_server_start(uwph_server_main_fn main_fn, const char *socket_path, int timeout_ms)
{
    struct server_args *a = malloc(sizeof(*a));
    uint64_t start = uwph_monotonic_ms();
    if (!a) return UWPH_FAIL(UWPH_ENOMEM);
    a->fn = main_fn;
    if (!uwph_host_thread_start(server_thread, a)) { free(a); return UWPH_FAIL(UWPH_EAGAIN); }
    /* the thread is never joined: wineserver lives as long as the app */
    while (!uwph_unix_path_listening(socket_path)) {
        if (server_state == 2) return UWPH_FAIL(UWPH_ECONNREFUSED);
        if (uwph_monotonic_ms() - start >= (uint64_t)timeout_ms) return UWPH_FAIL(UWPH_ETIMEDOUT);
        uwph_lock(&uwph_io_lock);
        uwph_cond_wait(&uwph_io_cond, &uwph_io_lock, 10);
        uwph_unlock(&uwph_io_lock);
    }
    return 0;
}

int uwph_server_running(void) { return server_state == 1; }
