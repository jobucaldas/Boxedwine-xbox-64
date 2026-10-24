/* poll / select / epoll over the fd table (level-triggered). */
#include "internal.h"

void uwph_poll_notify(void)
{
    uwph_lock(&uwph_io_lock);
    uwph_cond_broadcast(&uwph_io_cond);
    uwph_unlock(&uwph_io_lock);
}

static int readiness_locked(enum uwph_fd_kind kind, struct uwph_object *o)
{
    switch (kind) {
    case UWPH_FD_FREE:    return UWPH_POLLNVAL;
    case UWPH_FD_PIPE_R: case UWPH_FD_PIPE_W: case UWPH_FD_UNIX:
        return uwph_unix_poll_locked(o, kind);
    case UWPH_FD_CONSOLE: return UWPH_POLLOUT;
    case UWPH_FD_FILE: case UWPH_FD_DIR: return UWPH_POLLIN | UWPH_POLLOUT;
    default:              return 0;    /* Winsock/inotify/nested epoll: backend */
    }
}

static int remaining(uint64_t start, int timeout_ms)
{
    uint64_t el;
    if (timeout_ms < 0) return -1;
    el = uwph_monotonic_ms() - start;
    return el >= (uint64_t)timeout_ms ? 0 : (int)(timeout_ms - el);
}

UWPH_API int uwph_poll(struct uwph_pollfd *pfd, uwph_nfds_t n, int timeout_ms)
{
    struct uwph_object *objs[256];
    enum uwph_fd_kind kinds[256];
    uint64_t start = uwph_monotonic_ms();
    int count;

    if (n > 256) return UWPH_FAIL(UWPH_EINVAL);
    for (uwph_nfds_t i = 0; i < n; i++)
        objs[i] = pfd[i].fd < 0 ? (kinds[i] = UWPH_FD_FREE, NULL) : uwph_fd_get(pfd[i].fd, &kinds[i], NULL);

    uwph_lock(&uwph_io_lock);
    for (;;) {
        int left;
        count = 0;
        for (uwph_nfds_t i = 0; i < n; i++) {
            int r = 0;
            if (pfd[i].fd >= 0)
                r = readiness_locked(kinds[i], objs[i]) &
                    (pfd[i].events | UWPH_POLLERR | UWPH_POLLHUP | UWPH_POLLNVAL);
            pfd[i].revents = (short)r;
            count += r != 0;
        }
        if (count || !(left = remaining(start, timeout_ms))) break;
        uwph_cond_wait(&uwph_io_cond, &uwph_io_lock, left);
    }
    uwph_unlock(&uwph_io_lock);
    for (uwph_nfds_t i = 0; i < n; i++) uwph_obj_unref(objs[i]);
    return count;
}

/* fd_set: Linux 1024-bit array of unsigned long */
#define FDSET_ISSET(s, fd) ((((uint64_t *)(s))[(fd) / 64] >> ((fd) % 64)) & 1)
#define FDSET_SET(s, fd)   (((uint64_t *)(s))[(fd) / 64] |= (uint64_t)1 << ((fd) % 64))

UWPH_API int uwph_select(int nfds, void *rd, void *wr, void *ex, struct uwph_timeval *tv)
{
    struct uwph_pollfd pfd[256];
    int n = 0, ret, timeout = tv ? (int)(tv->tv_sec * 1000 + tv->tv_usec / 1000) : -1;
    if (nfds > 256) return UWPH_FAIL(UWPH_EINVAL);
    for (int fd = 0; fd < nfds; fd++) {
        short ev = (short)((rd && FDSET_ISSET(rd, fd) ? UWPH_POLLIN : 0) |
                           (wr && FDSET_ISSET(wr, fd) ? UWPH_POLLOUT : 0) |
                           (ex && FDSET_ISSET(ex, fd) ? UWPH_POLLPRI : 0));
        if (ev) { pfd[n].fd = fd; pfd[n].events = ev; n++; }
    }
    if ((ret = uwph_poll(pfd, (uwph_nfds_t)n, timeout)) < 0) return ret;
    if (rd) memset(rd, 0, 128);
    if (wr) memset(wr, 0, 128);
    if (ex) memset(ex, 0, 128);
    ret = 0;
    for (int i = 0; i < n; i++) {
        if (pfd[i].revents & UWPH_POLLNVAL) return UWPH_FAIL(UWPH_EBADF);
        if (rd && (pfd[i].revents & (UWPH_POLLIN | UWPH_POLLHUP | UWPH_POLLERR))) { FDSET_SET(rd, pfd[i].fd); ret++; }
        if (wr && (pfd[i].revents & (UWPH_POLLOUT | UWPH_POLLERR))) { FDSET_SET(wr, pfd[i].fd); ret++; }
        if (ex && (pfd[i].revents & UWPH_POLLPRI)) { FDSET_SET(ex, pfd[i].fd); ret++; }
    }
    return ret;
}

/* ---- epoll ---- */
struct ep_item {
    int fd;
    uint32_t events;
    uint64_t data;
    enum uwph_fd_kind kind;
    struct uwph_object *obj;   /* owned ref */
    int disabled;              /* EPOLLONESHOT fired */
};

struct epoll_obj {
    struct uwph_object base;
    struct ep_item *items;
    int n, cap;
    struct epoll_obj *next;
};

static struct epoll_obj *all_epolls;   /* guarded by uwph_io_lock */

static void epoll_destroy(struct uwph_object *o)
{
    struct epoll_obj *ep = (struct epoll_obj *)o, **p;
    uwph_lock(&uwph_io_lock);
    for (p = &all_epolls; *p; p = &(*p)->next)
        if (*p == ep) { *p = ep->next; break; }
    uwph_unlock(&uwph_io_lock);
    for (int i = 0; i < ep->n; i++) uwph_obj_unref(ep->items[i].obj);
    free(ep->items);
    free(ep);
}

UWPH_API int uwph_epoll_create1(int flags)
{
    struct epoll_obj *ep = calloc(1, sizeof(*ep));
    int fd;
    if (!ep) return UWPH_FAIL(UWPH_ENOMEM);
    uwph_obj_init(&ep->base, epoll_destroy);
    uwph_lock(&uwph_io_lock);
    ep->next = all_epolls;
    all_epolls = ep;
    uwph_unlock(&uwph_io_lock);
    if ((fd = uwph_fd_alloc(UWPH_FD_EPOLL, &ep->base, UWPH_O_RDWR | (flags & UWPH_EPOLL_CLOEXEC))) < 0)
        uwph_obj_unref(&ep->base);
    return fd;
}

UWPH_API int uwph_epoll_create(int size)
{
    if (size <= 0) return UWPH_FAIL(UWPH_EINVAL);
    return uwph_epoll_create1(0);
}

static struct epoll_obj *get_epoll(int epfd)
{
    enum uwph_fd_kind kind;
    struct uwph_object *o = uwph_fd_get(epfd, &kind, NULL);
    if (kind == UWPH_FD_EPOLL) return (struct epoll_obj *)o;
    uwph_obj_unref(o);
    uwph_errno = kind == UWPH_FD_FREE ? UWPH_EBADF : UWPH_EINVAL;
    return NULL;
}

UWPH_API int uwph_epoll_ctl(int epfd, int op, int fd, struct uwph_epoll_event *ev)
{
    struct epoll_obj *ep = get_epoll(epfd);
    struct uwph_object *target = NULL, *drop = NULL;
    enum uwph_fd_kind kind = UWPH_FD_FREE;
    int ret = 0, i;

    if (!ep) return -1;
    if (op == UWPH_EPOLL_CTL_ADD) {
        target = uwph_fd_get(fd, &kind, NULL);
        if (kind == UWPH_FD_FREE) { uwph_obj_unref(&ep->base); return UWPH_FAIL(UWPH_EBADF); }
        if (fd == epfd) { uwph_obj_unref(target); uwph_obj_unref(&ep->base); return UWPH_FAIL(UWPH_EINVAL); }
    }
    uwph_lock(&uwph_io_lock);
    for (i = 0; i < ep->n; i++) if (ep->items[i].fd == fd) break;
    switch (op) {
    case UWPH_EPOLL_CTL_ADD:
        if (i < ep->n) { ret = UWPH_FAIL(UWPH_EEXIST); drop = target; break; }
        if (ep->n == ep->cap) {
            int cap = ep->cap ? ep->cap * 2 : 16;
            struct ep_item *ni = realloc(ep->items, cap * sizeof(*ni));
            if (!ni) { ret = UWPH_FAIL(UWPH_ENOMEM); drop = target; break; }
            ep->items = ni;
            ep->cap = cap;
        }
        ep->items[ep->n++] = (struct ep_item){ fd, ev->events, ev->data, kind, target, 0 };
        break;
    case UWPH_EPOLL_CTL_MOD:
        if (i == ep->n) { ret = UWPH_FAIL(UWPH_ENOENT); break; }
        ep->items[i].events = ev->events;
        ep->items[i].data = ev->data;
        ep->items[i].disabled = 0;
        break;
    case UWPH_EPOLL_CTL_DEL:
        if (i == ep->n) { ret = UWPH_FAIL(UWPH_ENOENT); break; }
        drop = ep->items[i].obj;
        ep->items[i] = ep->items[--ep->n];
        break;
    default:
        ret = UWPH_FAIL(UWPH_EINVAL);
    }
    uwph_cond_broadcast(&uwph_io_cond);
    uwph_unlock(&uwph_io_lock);
    uwph_obj_unref(drop);
    uwph_obj_unref(&ep->base);
    return ret;
}

UWPH_API int uwph_epoll_wait(int epfd, struct uwph_epoll_event *evs, int max, int timeout_ms)
{
    struct epoll_obj *ep = get_epoll(epfd);
    uint64_t start = uwph_monotonic_ms();
    int count;
    if (!ep) return -1;
    if (max <= 0) { uwph_obj_unref(&ep->base); return UWPH_FAIL(UWPH_EINVAL); }
    uwph_lock(&uwph_io_lock);
    for (;;) {
        int left;
        count = 0;
        for (int i = 0; i < ep->n && count < max; i++) {
            struct ep_item *it = &ep->items[i];
            uint32_t r;
            if (it->disabled) continue;
            r = (uint32_t)readiness_locked(it->kind, it->obj) & (it->events | UWPH_POLLERR | UWPH_POLLHUP);
            if (!r) continue;
            evs[count].events = r;
            evs[count].data = it->data;
            count++;
            if (it->events & UWPH_EPOLLONESHOT) it->disabled = 1;
        }
        if (count || !(left = remaining(start, timeout_ms))) break;
        uwph_cond_wait(&uwph_io_cond, &uwph_io_lock, left);
    }
    uwph_unlock(&uwph_io_lock);
    uwph_obj_unref(&ep->base);
    return count;
}

int uwph_epoll_fd_closed(int fd)
{
    struct uwph_object *drop[64];
    int ndrop = 0;
    uwph_lock(&uwph_io_lock);
    for (struct epoll_obj *ep = all_epolls; ep; ep = ep->next)
        for (int i = 0; i < ep->n && ndrop < 64; i++)
            if (ep->items[i].fd == fd) {
                drop[ndrop++] = ep->items[i].obj;
                ep->items[i--] = ep->items[--ep->n];
            }
    uwph_unlock(&uwph_io_lock);
    for (int i = 0; i < ndrop; i++) uwph_obj_unref(drop[i]);
    return ndrop;
}
