/*
 * Readiness: poll/select (ntdll.so: server pipes, sockets) and epoll
 * (wineserver's main loop in server/fd.c). In-process objects signal a
 * global change counter; waiters block on it with a timeout. Host-backed fds
 * (Winsock, files) get readiness from WSAPoll / overlapped I/O.
 */
#ifndef UWPH_POLL_H
#define UWPH_POLL_H
#ifdef __cplusplus
extern "C" {
#endif

UWPH_API int uwph_poll(struct uwph_pollfd *fds, uwph_nfds_t n, int timeout_ms);
UWPH_API int uwph_select(int nfds, void *rd, void *wr, void *ex, struct uwph_timeval *tv);
UWPH_API int uwph_epoll_create(int size);
UWPH_API int uwph_epoll_create1(int flags);
UWPH_API int uwph_epoll_ctl(int epfd, int op, int fd, struct uwph_epoll_event *ev);
UWPH_API int uwph_epoll_wait(int epfd, struct uwph_epoll_event *evs, int max, int timeout_ms);

/* called by in-process objects whenever readiness may have changed */
void uwph_poll_notify(void);

#ifdef __cplusplus
}
#endif
#endif
