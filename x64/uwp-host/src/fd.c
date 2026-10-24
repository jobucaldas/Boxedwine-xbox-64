/* fd table and the generic fd operations. */
#include "internal.h"
#include <stdarg.h>
#include <stdio.h>

struct fd_entry {
    enum uwph_fd_kind kind;
    struct uwph_object *obj;
    int flags;      /* O_NONBLOCK / O_APPEND / access mode (F_GETFL) */
    int cloexec;
};

static struct fd_entry fds[UWPH_MAX_FDS];
static uwph_lock_t fd_lock = UWPH_LOCK_INIT;

void uwph_fd_init(void)
{
    for (int i = 0; i < 3; i++) {
        fds[i].kind = UWPH_FD_CONSOLE;
        fds[i].flags = i ? UWPH_O_WRONLY : UWPH_O_RDONLY;
    }
}

static int alloc_locked(int start, enum uwph_fd_kind kind, struct uwph_object *obj, int flags)
{
    for (int i = start; i < UWPH_MAX_FDS; i++) {
        if (fds[i].kind != UWPH_FD_FREE) continue;
        fds[i].kind = kind;
        fds[i].obj = obj;
        fds[i].flags = flags & ~UWPH_O_CLOEXEC;
        fds[i].cloexec = !!(flags & UWPH_O_CLOEXEC);
        return i;
    }
    return UWPH_FAIL(UWPH_EMFILE);
}

int uwph_fd_alloc(enum uwph_fd_kind kind, struct uwph_object *obj, int flags)
{
    int fd;
    uwph_lock(&fd_lock);
    fd = alloc_locked(0, kind, obj, flags);
    uwph_unlock(&fd_lock);
    return fd;
}

static void release_entry(struct fd_entry e, int fd)
{
    if (e.kind == UWPH_FD_FREE) return;
    uwph_epoll_fd_closed(fd);
    if (e.obj && e.obj->fd_closed) e.obj->fd_closed(e.obj, e.kind);
    uwph_obj_unref(e.obj);
}

int uwph_fd_alloc_at(int fd, enum uwph_fd_kind kind, struct uwph_object *obj, int flags)
{
    struct fd_entry old;
    if (fd < 0 || fd >= UWPH_MAX_FDS) return UWPH_FAIL(UWPH_EBADF);
    uwph_lock(&fd_lock);
    old = fds[fd];
    fds[fd].kind = kind;
    fds[fd].obj = obj;
    fds[fd].flags = flags & ~UWPH_O_CLOEXEC;
    fds[fd].cloexec = !!(flags & UWPH_O_CLOEXEC);
    uwph_unlock(&fd_lock);
    release_entry(old, fd);
    return fd;
}

struct uwph_object *uwph_fd_get(int fd, enum uwph_fd_kind *kind, int *flags)
{
    struct uwph_object *o = NULL;
    if (fd < 0 || fd >= UWPH_MAX_FDS) { *kind = UWPH_FD_FREE; return NULL; }
    uwph_lock(&fd_lock);
    *kind = fds[fd].kind;
    if (flags) *flags = fds[fd].flags;
    o = uwph_obj_ref(fds[fd].obj);
    uwph_unlock(&fd_lock);
    return o;
}

int uwph_fd_count(void)
{
    int n = 0;
    uwph_lock(&fd_lock);
    for (int i = 0; i < UWPH_MAX_FDS; i++) n += fds[i].kind != UWPH_FD_FREE;
    uwph_unlock(&fd_lock);
    return n;
}

UWPH_API int uwph_close(int fd)
{
    struct fd_entry old;
    if (fd < 0 || fd >= UWPH_MAX_FDS) return UWPH_FAIL(UWPH_EBADF);
    uwph_lock(&fd_lock);
    old = fds[fd];
    memset(&fds[fd], 0, sizeof(fds[fd]));
    uwph_unlock(&fd_lock);
    if (old.kind == UWPH_FD_FREE) return UWPH_FAIL(UWPH_EBADF);
    release_entry(old, fd);
    return 0;
}

static int dup_from(int fd, int start, int cloexec)
{
    int nfd;
    if (fd < 0 || fd >= UWPH_MAX_FDS) return UWPH_FAIL(UWPH_EBADF);
    uwph_lock(&fd_lock);
    if (fds[fd].kind == UWPH_FD_FREE) { uwph_unlock(&fd_lock); return UWPH_FAIL(UWPH_EBADF); }
    nfd = alloc_locked(start, fds[fd].kind, uwph_obj_ref(fds[fd].obj),
                       fds[fd].flags | (cloexec ? UWPH_O_CLOEXEC : 0));
    if (nfd < 0) uwph_obj_unref(fds[fd].obj);
    uwph_unlock(&fd_lock);
    return nfd;
}

UWPH_API int uwph_dup(int fd) { return dup_from(fd, 0, 0); }

UWPH_API int uwph_dup2(int fd, int newfd)
{
    enum uwph_fd_kind kind;
    int flags;
    struct uwph_object *o;
    if (newfd < 0 || newfd >= UWPH_MAX_FDS) return UWPH_FAIL(UWPH_EBADF);
    o = uwph_fd_get(fd, &kind, &flags);
    if (kind == UWPH_FD_FREE) return UWPH_FAIL(UWPH_EBADF);
    if (fd == newfd) { uwph_obj_unref(o); return newfd; }
    return uwph_fd_alloc_at(newfd, kind, o, flags);
}

UWPH_API int uwph_fcntl(int fd, int cmd, ...)
{
    va_list ap;
    long arg;
    int ret = 0;
    va_start(ap, cmd);
    arg = va_arg(ap, long);
    va_end(ap);
    switch (cmd) {
    case UWPH_F_DUPFD:         return dup_from(fd, (int)arg, 0);
    case UWPH_F_DUPFD_CLOEXEC: return dup_from(fd, (int)arg, 1);
    case UWPH_F_GETLK: case UWPH_F_SETLK: case UWPH_F_SETLKW:
        return UWPH_ENOSYS_INT();      /* -> LockFileEx on host files */
    }
    if (fd < 0 || fd >= UWPH_MAX_FDS) return UWPH_FAIL(UWPH_EBADF);
    uwph_lock(&fd_lock);
    if (fds[fd].kind == UWPH_FD_FREE) ret = UWPH_FAIL(UWPH_EBADF);
    else switch (cmd) {
    case UWPH_F_GETFD: ret = fds[fd].cloexec ? UWPH_FD_CLOEXEC : 0; break;
    case UWPH_F_SETFD: fds[fd].cloexec = !!(arg & UWPH_FD_CLOEXEC); break;
    case UWPH_F_GETFL: ret = fds[fd].flags; break;
    case UWPH_F_SETFL:
        fds[fd].flags = (fds[fd].flags & UWPH_O_ACCMODE) | ((int)arg & (UWPH_O_NONBLOCK | UWPH_O_APPEND));
        break;
    default: ret = UWPH_FAIL(UWPH_EINVAL);
    }
    uwph_unlock(&fd_lock);
    return ret;
}

UWPH_API int uwph_ioctl(int fd, unsigned long req, ...)
{
    (void)fd; (void)req;
    return UWPH_ENOSYS_INT();   /* FIONREAD / FIONBIO / TCGETS: per-object, not wired yet */
}

UWPH_API int uwph_isatty(int fd)
{
    enum uwph_fd_kind kind;
    uwph_obj_unref(uwph_fd_get(fd, &kind, NULL));
    if (kind == UWPH_FD_FREE) return uwph_errno = UWPH_EBADF, 0;
    if (kind != UWPH_FD_CONSOLE) return uwph_errno = UWPH_ENOTTY, 0;
    return 1;
}

UWPH_API uwph_ssize_t uwph_read(int fd, void *buf, size_t len)
{
    enum uwph_fd_kind kind;
    int flags;
    uwph_ssize_t ret;
    struct uwph_object *o = uwph_fd_get(fd, &kind, &flags);
    switch (kind) {
    case UWPH_FD_FREE: return UWPH_FAIL(UWPH_EBADF);
    case UWPH_FD_PIPE_R: case UWPH_FD_UNIX:
        ret = uwph_stream_read(o, kind, buf, len, flags & UWPH_O_NONBLOCK, 0); break;
    case UWPH_FD_FILE:
        ret = uwph_file_read(o, buf, len, -1); break;
    case UWPH_FD_CONSOLE:
        ret = 0; break;  /* no stdin in the app */
    default:
        ret = UWPH_FAIL(UWPH_EINVAL);
    }
    uwph_obj_unref(o);
    return ret;
}

UWPH_API uwph_ssize_t uwph_write(int fd, const void *buf, size_t len)
{
    enum uwph_fd_kind kind;
    int flags;
    uwph_ssize_t ret;
    struct uwph_object *o = uwph_fd_get(fd, &kind, &flags);
    switch (kind) {
    case UWPH_FD_FREE: return UWPH_FAIL(UWPH_EBADF);
    case UWPH_FD_PIPE_W: case UWPH_FD_UNIX:
        ret = uwph_stream_write(o, kind, buf, len, flags & UWPH_O_NONBLOCK); break;
    case UWPH_FD_CONSOLE:
        fwrite(buf, 1, len, fd == 1 ? stdout : stderr);
        ret = (uwph_ssize_t)len; break;
    case UWPH_FD_FILE:
        ret = UWPH_ENOSYS_INT(); break;  /* WriteFile on host files: next step */
    default:
        ret = UWPH_FAIL(UWPH_EINVAL);
    }
    uwph_obj_unref(o);
    return ret;
}

UWPH_API uwph_ssize_t uwph_writev(int fd, const struct uwph_iovec *iov, int cnt)
{
    uwph_ssize_t total = 0;
    for (int i = 0; i < cnt; i++) {
        uwph_ssize_t r = uwph_write(fd, iov[i].iov_base, iov[i].iov_len);
        if (r < 0) return total ? total : r;
        total += r;
        if ((size_t)r < iov[i].iov_len) break;
    }
    return total;
}

UWPH_API uwph_ssize_t uwph_pread(int fd, void *buf, size_t len, uwph_off_t off)
{
    enum uwph_fd_kind kind;
    uwph_ssize_t ret;
    struct uwph_object *o = uwph_fd_get(fd, &kind, NULL);
    if (kind == UWPH_FD_FREE) return UWPH_FAIL(UWPH_EBADF);
    ret = kind == UWPH_FD_FILE ? uwph_file_read(o, buf, len, off < 0 ? 0 : off) : UWPH_FAIL(UWPH_ESPIPE);
    uwph_obj_unref(o);
    return ret;
}

UWPH_API uwph_ssize_t uwph_pwrite(int fd, const void *buf, size_t len, uwph_off_t off)
{
    (void)fd; (void)buf; (void)len; (void)off;
    return UWPH_ENOSYS_INT();
}

UWPH_API uwph_off_t uwph_lseek(int fd, uwph_off_t off, int whence)
{
    enum uwph_fd_kind kind;
    uwph_off_t ret;
    struct uwph_object *o = uwph_fd_get(fd, &kind, NULL);
    if (kind == UWPH_FD_FREE) return UWPH_FAIL(UWPH_EBADF);
    ret = kind == UWPH_FD_FILE ? uwph_file_seek(o, off, whence) : UWPH_FAIL(UWPH_ESPIPE);
    uwph_obj_unref(o);
    return ret;
}
