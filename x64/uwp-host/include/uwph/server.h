/*
 * In-process wineserver. Normally ntdll.so fork/execs wineserver and talks
 * to it over an AF_UNIX socket. Here wineserver's main() (built from Wine's
 * server/ sources against this same host) runs on a dedicated host thread;
 * the socket is the in-process AF_UNIX namespace and ptrace() is not needed
 * because the server only ever manages threads of its own process.
 */
#ifndef UWPH_SERVER_H
#define UWPH_SERVER_H
#ifdef __cplusplus
extern "C" {
#endif

typedef int (*uwph_server_main_fn)(int argc, char **argv);

/* Start wineserver on a host thread; returns once its socket is listening
 * (polls for `socket_path` to exist in the in-process namespace) or after
 * timeout_ms. */
int uwph_server_start(uwph_server_main_fn main_fn, const char *socket_path, int timeout_ms);
int uwph_server_running(void);

/* is `path` a listening in-process AF_UNIX socket? */
int uwph_unix_path_listening(const char *path);

#ifdef __cplusplus
}
#endif
#endif
