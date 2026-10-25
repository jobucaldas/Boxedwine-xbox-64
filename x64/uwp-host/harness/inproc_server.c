/* Linux integration proof: start Wine's own server before Unix ntdll loads.
 * The launcher execs wine64; do not start a thread in the pre-exec launcher.
 * Descendant Wine loaders connect to the original loader's hosted server.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int uwph_server_start(const char *, int *);
__attribute__((constructor)) static void start_inproc_server(void)
{
    char exe[4096], byte;
    int fd, rc;
    const char *enabled = getenv("UWPH_INPROC_SERVER");
    if (!enabled || strcmp(enabled, "1")) return;
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe)-1);
    if (n < 0) _exit(125);
    exe[n] = 0;
    const char *base = strrchr(exe, '/');
    if (!base || strcmp(base+1, "wine64")) return;
    /* ntdll re-execs the loader once to establish its address layout.
     * Threads do not survive exec: start only in the final loader image. */
    if (!getenv("WINELOADERNOEXEC")) return;
    if (getenv("UWPH_INPROC_OWNER")) return;
    char owner[32];
    snprintf(owner, sizeof(owner), "%ld", (long)getpid());
    if (setenv("UWPH_INPROC_OWNER", owner, 1)) _exit(125);
    rc = uwph_server_start(getenv("WINEPREFIX"), &fd);
    if (rc) { fprintf(stderr, "[inproc] start error=%d\n", rc); _exit(125); }
    struct pollfd ready = { fd, POLLIN, 0 };
    do { rc = poll(&ready, 1, 10000); } while (rc < 0 && errno == EINTR);
    if (rc <= 0 || read(fd, &byte, 1) != 1 || byte != 'R')
    { fprintf(stderr, "[inproc] readiness failed\n"); _exit(125); }
    close(fd);
    fprintf(stderr, "[inproc] Wine loader pid=%ld server thread ready; no separate server process\n", (long)getpid());
    /* Wine terminates the host with exit_group, including its server thread.
     * Joining in a destructor could deadlock while ntdll is still connected. */
}
