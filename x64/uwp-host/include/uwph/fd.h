/*
 * File-descriptor table. Every fd-shaped thing Wine uses lives here: host
 * files and directories, in-process pipes and AF_UNIX sockets (the wineserver
 * channel), epoll and inotify instances, Winsock sockets.
 *
 * This is the piece BoxedWine already has (KFileDescriptor / KObject), with
 * host pointers instead of 32-bit guest addresses.
 */
#ifndef UWPH_FD_H
#define UWPH_FD_H
#ifdef __cplusplus
extern "C" {
#endif

#define UWPH_MAX_FDS 4096

enum uwph_fd_kind {
    UWPH_FD_FREE = 0,
    UWPH_FD_FILE,       /* host file handle (CreateFile2) */
    UWPH_FD_DIR,        /* directory handle + enumeration state */
    UWPH_FD_PIPE_R,     /* in-process pipe, read end */
    UWPH_FD_PIPE_W,     /* in-process pipe, write end */
    UWPH_FD_UNIX,       /* in-process AF_UNIX socket */
    UWPH_FD_INET,       /* Winsock socket */
    UWPH_FD_EPOLL,
    UWPH_FD_INOTIFY,
    UWPH_FD_CONSOLE     /* 0/1/2 -> host debug output */
};

struct uwph_object;   /* refcounted backing object, defined in src/internal.h */

/* fd-level operations */
UWPH_API int uwph_close(int fd);
UWPH_API int uwph_dup(int fd);
UWPH_API int uwph_dup2(int fd, int newfd);
UWPH_API int uwph_fcntl(int fd, int cmd, ...);
UWPH_API int uwph_ioctl(int fd, unsigned long req, ...);
UWPH_API int uwph_isatty(int fd);
UWPH_API uwph_ssize_t uwph_read(int fd, void *buf, size_t len);
UWPH_API uwph_ssize_t uwph_write(int fd, const void *buf, size_t len);
UWPH_API uwph_ssize_t uwph_writev(int fd, const struct uwph_iovec *iov, int cnt);
UWPH_API uwph_ssize_t uwph_pread(int fd, void *buf, size_t len, uwph_off_t off);
UWPH_API uwph_ssize_t uwph_pwrite(int fd, const void *buf, size_t len, uwph_off_t off);
UWPH_API uwph_off_t   uwph_lseek(int fd, uwph_off_t off, int whence);
UWPH_API int uwph_pipe(int fds[2]);
UWPH_API int uwph_pipe2(int fds[2], int flags);

/* internal API for the other modules */
int  uwph_fd_alloc(enum uwph_fd_kind kind, struct uwph_object *obj, int flags);
int  uwph_fd_alloc_at(int fd, enum uwph_fd_kind kind, struct uwph_object *obj, int flags);
struct uwph_object *uwph_fd_get(int fd, enum uwph_fd_kind *kind, int *flags); /* +1 ref */
int  uwph_fd_count(void);

#ifdef __cplusplus
}
#endif
#endif
