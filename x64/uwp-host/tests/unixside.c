/*
 * Stand-in for Wine's Unix side: plain C calling the uwph exports the way
 * ntdll.so would once libc_names.h renames pipe -> uwph_pipe etc.
 */
#include "uwph/uwph.h"
#include <string.h>

/* mimics server.c: create the request pipe, push a request, read it back */
long unixside_roundtrip(const char *msg)
{
    int fds[2];
    char buf[64] = {0};
    long n;
    if (uwph_pipe(fds) < 0) return -1000 - uwph_errno;
    if (uwph_write(fds[1], msg, strlen(msg)) != (uwph_ssize_t)strlen(msg)) return -2;
    n = uwph_read(fds[0], buf, sizeof(buf) - 1);
    uwph_close(fds[0]);
    uwph_close(fds[1]);
    return n == (long)strlen(msg) && !memcmp(buf, msg, (size_t)n) ? n : -3;
}

/* A zero-argument raw syscall must not read absent variadic slots. */
long unixside_syscall_gettid(void)
{
    return uwph_syscall(UWPH_NR_gettid);
}
