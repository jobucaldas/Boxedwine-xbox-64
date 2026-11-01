/*
 * In-process pipes and AF_UNIX sockets (wineserver transport), plus the
 * Winsock entry points.
 *
 * A connection ("conn") has two sides with one byte queue each; side s reads
 * q[s] and writes q[1-s]. A pipe is a conn whose read end is side 0 and write
 * end side 1. Each queue is a list of chunks, one per write/sendmsg; a chunk
 * carries the fds passed with it (SCM_RIGHTS), delivered with its first byte.
 */
#include "internal.h"

#define QUEUE_LIMIT  (256 * 1024)
#define MAX_PASS_FDS 16
#define MAX_PENDING  64

uwph_lock_t uwph_io_lock = UWPH_LOCK_INIT;
uwph_cond_t uwph_io_cond = UWPH_COND_INIT;

struct passed_fd { enum uwph_fd_kind kind; struct uwph_object *obj; int flags; };

struct chunk {
    struct chunk *next;
    size_t len, pos;
    int nfds;
    struct passed_fd fds[MAX_PASS_FDS];
    unsigned char data[];
};

struct queue { struct chunk *head, *tail; size_t bytes; };

struct conn {
    volatile long refs;
    struct queue q[2];
    int closed[2];      /* side's endpoint is gone */
    int shut_wr[2];     /* side did shutdown(SHUT_WR) */
};

enum { EP_NEW, EP_BOUND, EP_LISTENING, EP_CONNECTED };

struct endpoint {
    struct uwph_object base;
    int is_socket;
    int type;
    int state;
    struct conn *conn;
    int side;
    char path[108];                       /* bound name (absolute) */
    char peer_path[108];
    struct endpoint *pending[MAX_PENDING];  /* listening: accept queue (owned refs) */
    int npending, backlog;
};

/* ---- named socket namespace (listeners) ---- */
static struct endpoint *listeners[64];

static struct endpoint *find_listener_locked(const char *abs)
{
    for (unsigned i = 0; i < sizeof(listeners) / sizeof(listeners[0]); i++)
        if (listeners[i] && !strcmp(listeners[i]->path, abs)) return listeners[i];
    return NULL;
}

int uwph_unix_path_listening(const char *path)
{
    char abs[108];
    int ret;
    if (uwph_fs_abspath(path, abs, sizeof(abs)) < 0) return 0;
    uwph_lock(&uwph_io_lock);
    ret = find_listener_locked(abs) != NULL;
    uwph_unlock(&uwph_io_lock);
    return ret;
}

/* ---- chunks / queues ---- */
static void free_chunk(struct chunk *c)
{
    for (int i = 0; i < c->nfds; i++) uwph_obj_unref(c->fds[i].obj);
    free(c);
}

static void free_queue(struct queue *q)
{
    struct chunk *c = q->head, *n;
    for (; c; c = n) { n = c->next; free_chunk(c); }
    q->head = q->tail = NULL;
    q->bytes = 0;
}

static void conn_unref(struct conn *c)
{
    if (c && !__atomic_sub_fetch(&c->refs, 1, __ATOMIC_ACQ_REL)) {
        free_queue(&c->q[0]);
        free_queue(&c->q[1]);
        free(c);
    }
}

static void endpoint_destroy(struct uwph_object *o)
{
    struct endpoint *ep = (struct endpoint *)o;
    uwph_lock(&uwph_io_lock);
    if (ep->conn) ep->conn->closed[ep->side] = 1;
    if (ep->state == EP_LISTENING)
        for (unsigned i = 0; i < sizeof(listeners) / sizeof(listeners[0]); i++)
            if (listeners[i] == ep) listeners[i] = NULL;
    uwph_cond_broadcast(&uwph_io_cond);
    uwph_unlock(&uwph_io_lock);
    for (int i = 0; i < ep->npending; i++) uwph_obj_unref(&ep->pending[i]->base);
    conn_unref(ep->conn);
    free(ep);
}

static struct endpoint *endpoint_new(int is_socket, int type)
{
    struct endpoint *ep = calloc(1, sizeof(*ep));
    if (!ep) return NULL;
    uwph_obj_init(&ep->base, endpoint_destroy);
    ep->is_socket = is_socket;
    ep->type = type;
    return ep;
}

static struct conn *conn_new(void)
{
    struct conn *c = calloc(1, sizeof(*c));
    if (c) c->refs = 2;     /* one per side */
    return c;
}

static int make_pair(int is_socket, int type, struct endpoint **a, struct endpoint **b)
{
    struct conn *c = conn_new();
    *a = endpoint_new(is_socket, type);
    *b = endpoint_new(is_socket, type);
    if (!c || !*a || !*b) { free(c); free(*a); free(*b); return UWPH_FAIL(UWPH_ENOMEM); }
    (*a)->conn = (*b)->conn = c;
    (*a)->side = 0;
    (*b)->side = 1;
    (*a)->state = (*b)->state = EP_CONNECTED;
    return 0;
}

/* ---- data path (shared by read/write and send/recv*) ---- */
static uwph_ssize_t do_send(struct endpoint *ep, const struct uwph_iovec *iov, size_t iovcnt,
                            const struct passed_fd *pfds, int npfds, int nonblock)
{
    struct conn *c = ep->conn;
    struct queue *q;
    struct chunk *ch;
    size_t len = 0, off = 0;

    for (size_t i = 0; i < iovcnt; i++) len += iov[i].iov_len;
    if (!c) return UWPH_FAIL(UWPH_ENOTCONN);
    q = &c->q[1 - ep->side];

    uwph_lock(&uwph_io_lock);
    for (;;) {
        if (c->closed[1 - ep->side] || c->shut_wr[ep->side]) {
            uwph_unlock(&uwph_io_lock);
            return UWPH_FAIL(UWPH_EPIPE);
        }
        if (q->bytes + len <= QUEUE_LIMIT || !q->bytes) break;
        if (nonblock) { uwph_unlock(&uwph_io_lock); return UWPH_FAIL(UWPH_EAGAIN); }
        uwph_cond_wait(&uwph_io_cond, &uwph_io_lock, -1);
    }
    if (!(ch = malloc(sizeof(*ch) + len))) { uwph_unlock(&uwph_io_lock); return UWPH_FAIL(UWPH_ENOMEM); }
    ch->next = NULL;
    ch->len = len;
    ch->pos = 0;
    ch->nfds = npfds;
    for (int i = 0; i < npfds; i++) ch->fds[i] = pfds[i];
    for (size_t i = 0; i < iovcnt; i++) {
        memcpy(ch->data + off, iov[i].iov_base, iov[i].iov_len);
        off += iov[i].iov_len;
    }
    if (q->tail) q->tail->next = ch; else q->head = ch;
    q->tail = ch;
    q->bytes += len;
    uwph_cond_broadcast(&uwph_io_cond);
    uwph_unlock(&uwph_io_lock);
    return (uwph_ssize_t)len;
}

/* Reads into iov. Fds attached to the first chunk read are returned in
 * out_fds (refs transferred). Stops before a later chunk that carries fds. */
static uwph_ssize_t do_recv(struct endpoint *ep, const struct uwph_iovec *iov, size_t iovcnt,
                            struct passed_fd *out_fds, int *nout, int nonblock, int peek)
{
    struct conn *c = ep->conn;
    struct queue *q;
    size_t total = 0, want = 0, vi = 0, voff = 0;

    *nout = 0;
    for (size_t i = 0; i < iovcnt; i++) want += iov[i].iov_len;
    if (!c) return UWPH_FAIL(UWPH_ENOTCONN);
    q = &c->q[ep->side];

    uwph_lock(&uwph_io_lock);
    while (!q->head) {
        if (c->closed[1 - ep->side] || c->shut_wr[1 - ep->side]) { uwph_unlock(&uwph_io_lock); return 0; }
        if (nonblock) { uwph_unlock(&uwph_io_lock); return UWPH_FAIL(UWPH_EAGAIN); }
        uwph_cond_wait(&uwph_io_cond, &uwph_io_lock, -1);
    }
    for (struct chunk *ch = q->head; ch && total < want;) {
        if (ch->nfds && total) break;          /* deliver its fds with its own read */
        if (ch->nfds && !peek) {
            for (int i = 0; i < ch->nfds; i++) out_fds[i] = ch->fds[i];
            *nout = ch->nfds;
            ch->nfds = 0;
        }
        while (ch->pos < ch->len && total < want) {
            size_t n = ch->len - ch->pos, room = iov[vi].iov_len - voff;
            if (n > room) n = room;
            memcpy((char *)iov[vi].iov_base + voff, ch->data + ch->pos, n);
            total += n;
            voff += n;
            if (!peek) { ch->pos += n; q->bytes -= n; }
            else break;
            if (voff == iov[vi].iov_len) { vi++; voff = 0; }
        }
        if (peek) break;
        if (ch->pos == ch->len) {
            q->head = ch->next;
            if (!q->head) q->tail = NULL;
            free(ch);
            ch = q->head;
        }
    }
    uwph_cond_broadcast(&uwph_io_cond);
    uwph_unlock(&uwph_io_lock);
    return (uwph_ssize_t)total;
}

uwph_ssize_t uwph_stream_read(struct uwph_object *o, int kind, void *buf, size_t len, int nonblock, int peek)
{
    struct passed_fd pf[MAX_PASS_FDS];
    struct uwph_iovec iov = { buf, len };
    int n;
    uwph_ssize_t r;
    (void)kind;
    r = do_recv((struct endpoint *)o, &iov, 1, pf, &n, nonblock, peek);
    for (int i = 0; i < n; i++) uwph_obj_unref(pf[i].obj);  /* plain read drops passed fds */
    return r;
}

uwph_ssize_t uwph_stream_write(struct uwph_object *o, int kind, const void *buf, size_t len, int nonblock)
{
    struct uwph_iovec iov = { (void *)buf, len };
    (void)kind;
    return do_send((struct endpoint *)o, &iov, 1, NULL, 0, nonblock);
}

int uwph_unix_poll_locked(struct uwph_object *o, int kind)
{
    struct endpoint *ep = (struct endpoint *)o;
    struct conn *c = ep->conn;
    int ev = 0;
    if (ep->state == EP_LISTENING) return ep->npending ? UWPH_POLLIN : 0;
    if (!c) return ep->is_socket ? UWPH_POLLOUT | UWPH_POLLHUP : UWPH_POLLNVAL;
    if (kind != UWPH_FD_PIPE_W) {
        if (c->q[ep->side].head) ev |= UWPH_POLLIN;
        if (c->closed[1 - ep->side] || c->shut_wr[1 - ep->side]) ev |= UWPH_POLLIN | UWPH_POLLHUP;
    }
    if (kind != UWPH_FD_PIPE_R) {
        if (c->closed[1 - ep->side]) ev |= kind == UWPH_FD_PIPE_W ? UWPH_POLLERR : UWPH_POLLHUP;
        else if (c->q[1 - ep->side].bytes < QUEUE_LIMIT) ev |= UWPH_POLLOUT;
    }
    return ev;
}

/* ---- pipes ---- */
UWPH_API int uwph_pipe2(int fds[2], int flags)
{
    uwph_trace_hit(UWPH_TRACE_PIPE2);
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    return uwph_linux_pipe2(fds, flags);
#endif

    struct endpoint *r, *w;
    int fr, fw;
    if (make_pair(0, 0, &r, &w) < 0) return -1;
    if ((fr = uwph_fd_alloc(UWPH_FD_PIPE_R, &r->base, UWPH_O_RDONLY | flags)) < 0) {
        uwph_obj_unref(&r->base); uwph_obj_unref(&w->base); return -1;
    }
    if ((fw = uwph_fd_alloc(UWPH_FD_PIPE_W, &w->base, UWPH_O_WRONLY | flags)) < 0) {
        uwph_close(fr); uwph_obj_unref(&w->base); return -1;
    }
    fds[0] = fr;
    fds[1] = fw;
    return 0;
}

UWPH_API int uwph_pipe(int fds[2]) { return uwph_pipe2(fds, 0); }

/* ---- sockets ---- */
/* Wine passthrough uses native descriptors throughout the socket lifecycle,
 * matching read/write/close and Wine's native descriptors. Owned-slot Linux and other backends keep
 * the private in-process endpoint table; never mix these descriptor spaces. */
static struct endpoint *get_unix(int fd, int *flags)
{
    enum uwph_fd_kind kind;
    struct uwph_object *o = uwph_fd_get(fd, &kind, flags);
    if (kind == UWPH_FD_UNIX) return (struct endpoint *)o;
    uwph_obj_unref(o);
    uwph_errno = kind == UWPH_FD_FREE ? UWPH_EBADF : kind == UWPH_FD_INET ? UWPH_ENOSYS : UWPH_ENOTSOCK;
    return NULL;
}

UWPH_API int uwph_socket(int domain, int type, int proto)
{
    uwph_trace_hit(UWPH_TRACE_SOCKET);
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    if (uwph_linux_native_fds()) {
        UWPH_NATIVE(int, "socket", (int, int, int));
        UWPH_NATIVE_RETURN(fn(domain, type, proto));
    }
#endif

    struct endpoint *ep;
    int fd, base_type = type & 0xf;
    (void)proto;
    if (domain == UWPH_AF_INET || domain == UWPH_AF_INET6)
#ifdef UWPH_BACKEND_WIN32
        return uwph_inet_socket(domain, type, proto);
#else
        return UWPH_ENOSYS_INT();
#endif
    if (domain != UWPH_AF_UNIX) return UWPH_FAIL(UWPH_EAFNOSUPPORT);
    if (base_type != UWPH_SOCK_STREAM) return UWPH_FAIL(UWPH_EPROTONOSUPPORT);
    if (!(ep = endpoint_new(1, base_type))) return UWPH_FAIL(UWPH_ENOMEM);
    fd = uwph_fd_alloc(UWPH_FD_UNIX, &ep->base,
                       UWPH_O_RDWR | (type & (UWPH_SOCK_NONBLOCK | UWPH_SOCK_CLOEXEC)));
    if (fd < 0) uwph_obj_unref(&ep->base);
    return fd;
}

UWPH_API int uwph_socketpair(int domain, int type, int proto, int sv[2])
{
    uwph_trace_hit(UWPH_TRACE_SOCKETPAIR);
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    if (uwph_linux_native_fds()) {
        UWPH_NATIVE(int, "socketpair", (int, int, int, int *));
        UWPH_NATIVE_RETURN(fn(domain, type, proto, sv));
    }
#endif

    struct endpoint *a, *b;
    int fa, fb, fl = UWPH_O_RDWR | (type & (UWPH_SOCK_NONBLOCK | UWPH_SOCK_CLOEXEC));
    (void)proto;
#ifdef UWPH_BACKEND_WIN32
    if (domain == UWPH_AF_INET) return uwph_inet_socketpair(domain, type, proto, sv);
#endif
    if (domain != UWPH_AF_UNIX) return UWPH_FAIL(UWPH_EAFNOSUPPORT);
    if ((type & 0xf) != UWPH_SOCK_STREAM) return UWPH_FAIL(UWPH_EPROTONOSUPPORT);
    if (make_pair(1, UWPH_SOCK_STREAM, &a, &b) < 0) return -1;
    if ((fa = uwph_fd_alloc(UWPH_FD_UNIX, &a->base, fl)) < 0) {
        uwph_obj_unref(&a->base); uwph_obj_unref(&b->base); return -1;
    }
    if ((fb = uwph_fd_alloc(UWPH_FD_UNIX, &b->base, fl)) < 0) {
        uwph_close(fa); uwph_obj_unref(&b->base); return -1;
    }
    sv[0] = fa;
    sv[1] = fb;
    return 0;
}

static int sun_abspath(const struct uwph_sockaddr *addr, uwph_socklen_t len, char *abs)
{
    const struct uwph_sockaddr_un *un = (const struct uwph_sockaddr_un *)addr;
    char name[109];
    size_t n;
    if (!addr || len <= sizeof(un->sun_family) || un->sun_family != UWPH_AF_UNIX) return UWPH_FAIL(UWPH_EINVAL);
    n = len - sizeof(un->sun_family);
    if (n > sizeof(un->sun_path)) n = sizeof(un->sun_path);
    memcpy(name, un->sun_path, n);
    name[n] = 0;
    if (!name[0]) return UWPH_FAIL(UWPH_EOPNOTSUPP);   /* abstract namespace: unused by Wine */
    return uwph_fs_abspath(name, abs, 108) < 0 ? -1 : 0;
}

UWPH_API int uwph_bind(int fd, const struct uwph_sockaddr *addr, uwph_socklen_t len)
{
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    if (uwph_linux_native_fds()) {
        UWPH_NATIVE(int, "bind", (int, const struct uwph_sockaddr *, uwph_socklen_t));
        UWPH_NATIVE_RETURN(fn(fd, addr, len));
    }
#endif

#ifdef UWPH_BACKEND_WIN32
    if (uwph_is_inet(fd)) return uwph_inet_call(fd, INET_BIND, NULL, 0, 0, (struct uwph_sockaddr *)addr, &len);
#endif

    char abs[108];
    struct endpoint *ep = get_unix(fd, NULL);
    int ret = 0;
    if (!ep) return -1;
    if (sun_abspath(addr, len, abs) < 0) ret = -1;
    else {
        uwph_lock(&uwph_io_lock);
        if (ep->state != EP_NEW) ret = UWPH_FAIL(UWPH_EINVAL);
        else if (find_listener_locked(abs)) ret = UWPH_FAIL(UWPH_EADDRINUSE);
        else { strcpy(ep->path, abs); ep->state = EP_BOUND; }
        uwph_unlock(&uwph_io_lock);
    }
    uwph_obj_unref(&ep->base);
    return ret;
}

UWPH_API int uwph_listen(int fd, int backlog)
{
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    if (uwph_linux_native_fds()) {
        UWPH_NATIVE(int, "listen", (int, int));
        UWPH_NATIVE_RETURN(fn(fd, backlog));
    }
#endif

#ifdef UWPH_BACKEND_WIN32
    if (uwph_is_inet(fd)) return uwph_inet_call(fd, INET_LISTEN, NULL, 0, backlog, NULL, NULL);
#endif

    struct endpoint *ep = get_unix(fd, NULL);
    int ret = UWPH_FAIL(UWPH_EINVAL);
    if (!ep) return -1;
    uwph_lock(&uwph_io_lock);
    if (ep->state == EP_BOUND || ep->state == EP_LISTENING) {
        for (unsigned i = 0; i < sizeof(listeners) / sizeof(listeners[0]); i++) {
            if (listeners[i] && listeners[i] != ep) continue;
            listeners[i] = ep;   /* non-owning; cleared in endpoint_destroy */
            ep->state = EP_LISTENING;
            ep->backlog = backlog <= 0 || backlog > MAX_PENDING ? MAX_PENDING : backlog;
            ret = 0;
            break;
        }
    }
    uwph_unlock(&uwph_io_lock);
    uwph_obj_unref(&ep->base);
    return ret;
}

UWPH_API int uwph_connect(int fd, const struct uwph_sockaddr *addr, uwph_socklen_t len)
{
    uwph_trace_hit(UWPH_TRACE_CONNECT);
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    if (uwph_linux_native_fds()) {
        UWPH_NATIVE(int, "connect", (int, const struct uwph_sockaddr *, uwph_socklen_t));
        UWPH_NATIVE_RETURN(fn(fd, addr, len));
    }
#endif

#ifdef UWPH_BACKEND_WIN32
    if (uwph_is_inet(fd)) return uwph_inet_call(fd, INET_CONNECT, NULL, 0, 0, (struct uwph_sockaddr *)addr, &len);
#endif

    char abs[108];
    struct endpoint *ep = get_unix(fd, NULL), *srv, *peer;
    struct conn *c;
    int ret = 0;
    if (!ep) return -1;
    if (sun_abspath(addr, len, abs) < 0) { uwph_obj_unref(&ep->base); return -1; }
    peer = endpoint_new(1, ep->type);
    c = conn_new();
    uwph_lock(&uwph_io_lock);
    if (!peer || !c) ret = UWPH_FAIL(UWPH_ENOMEM);
    else if (ep->state == EP_CONNECTED) ret = UWPH_FAIL(UWPH_EISCONN);
    else if (!(srv = find_listener_locked(abs))) ret = UWPH_FAIL(UWPH_ECONNREFUSED);
    else if (srv->npending >= srv->backlog) ret = UWPH_FAIL(UWPH_EAGAIN);
    else {
        ep->conn = peer->conn = c;
        ep->side = 0;
        peer->side = 1;
        ep->state = peer->state = EP_CONNECTED;
        strcpy(ep->peer_path, abs);
        strcpy(peer->path, abs);
        srv->pending[srv->npending++] = peer;
        uwph_cond_broadcast(&uwph_io_cond);
        peer = NULL;
        c = NULL;
    }
    uwph_unlock(&uwph_io_lock);
    free(c);
    if (peer) free(peer);
    uwph_obj_unref(&ep->base);
    return ret;
}

UWPH_API int uwph_accept(int fd, struct uwph_sockaddr *addr, uwph_socklen_t *len)
{
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    if (uwph_linux_native_fds()) {
        UWPH_NATIVE(int, "accept", (int, struct uwph_sockaddr *, uwph_socklen_t *));
        UWPH_NATIVE_RETURN(fn(fd, addr, len));
    }
#endif

    int flags, nfd;
#ifdef UWPH_BACKEND_WIN32
    if (uwph_is_inet(fd)) return uwph_inet_call(fd, INET_ACCEPT, NULL, 0, 0, addr, len);
#endif

    struct endpoint *ep = get_unix(fd, &flags), *peer = NULL;
    if (!ep) return -1;
    uwph_lock(&uwph_io_lock);
    while (ep->state == EP_LISTENING && !ep->npending && !(flags & UWPH_O_NONBLOCK))
        uwph_cond_wait(&uwph_io_cond, &uwph_io_lock, -1);
    if (ep->state == EP_LISTENING && ep->npending) {
        peer = ep->pending[0];
        memmove(ep->pending, ep->pending + 1, --ep->npending * sizeof(ep->pending[0]));
    }
    uwph_unlock(&uwph_io_lock);
    if (!peer) {
        int e = ep->state == EP_LISTENING ? UWPH_EAGAIN : UWPH_EINVAL;
        uwph_obj_unref(&ep->base);
        return UWPH_FAIL(e);
    }
    uwph_obj_unref(&ep->base);
    if (addr && len && *len >= sizeof(uint16_t)) {
        addr->sa_family = UWPH_AF_UNIX;   /* unnamed peer */
        *len = sizeof(uint16_t);
    }
    nfd = uwph_fd_alloc(UWPH_FD_UNIX, &peer->base, UWPH_O_RDWR);
    if (nfd < 0) uwph_obj_unref(&peer->base);
    return nfd;
}

UWPH_API int uwph_shutdown(int fd, int how)
{
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    if (uwph_linux_native_fds()) {
        UWPH_NATIVE(int, "shutdown", (int, int));
        UWPH_NATIVE_RETURN(fn(fd, how));
    }
#endif

#ifdef UWPH_BACKEND_WIN32
    if (uwph_is_inet(fd)) return uwph_inet_call(fd, INET_SHUTDOWN, NULL, 0, how, NULL, NULL);
#endif

    struct endpoint *ep = get_unix(fd, NULL);
    if (!ep) return -1;
    uwph_lock(&uwph_io_lock);
    if (ep->conn && (how == UWPH_SHUT_WR || how == UWPH_SHUT_RDWR)) ep->conn->shut_wr[ep->side] = 1;
    uwph_cond_broadcast(&uwph_io_cond);
    uwph_unlock(&uwph_io_lock);
    uwph_obj_unref(&ep->base);
    return 0;
}

UWPH_API uwph_ssize_t uwph_sendmsg(int fd, const struct uwph_msghdr *msg, int flags)
{
    uwph_trace_hit(UWPH_TRACE_SENDMSG);
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    if (uwph_linux_native_fds()) {
        UWPH_NATIVE(uwph_ssize_t, "sendmsg", (int, const struct uwph_msghdr *, int));
        UWPH_NATIVE_RETURN(fn(fd, msg, flags));
    }
#endif

    struct passed_fd pf[MAX_PASS_FDS];
    int npf = 0, fl;
    uwph_ssize_t r;
    struct endpoint *ep = get_unix(fd, &fl);
    if (!ep) return -1;
    for (struct uwph_cmsghdr *c = UWPH_CMSG_FIRSTHDR(msg); c;) {
        if (c->cmsg_level == UWPH_SOL_SOCKET && c->cmsg_type == UWPH_SCM_RIGHTS) {
            int n = (int)((c->cmsg_len - UWPH_CMSG_LEN(0)) / sizeof(int));
            const int *in = (const int *)UWPH_CMSG_DATA(c);
            for (int i = 0; i < n && npf < MAX_PASS_FDS; i++) {
                pf[npf].obj = uwph_fd_get(in[i], &pf[npf].kind, &pf[npf].flags);
                if (pf[npf].kind == UWPH_FD_FREE) {
                    for (int j = 0; j < npf; j++) uwph_obj_unref(pf[j].obj);
                    uwph_obj_unref(&ep->base);
                    return UWPH_FAIL(UWPH_EBADF);
                }
                npf++;
            }
        }
        size_t next = (size_t)((char *)c - (char *)msg->msg_control) + UWPH_CMSG_ALIGN(c->cmsg_len);
        c = next + sizeof(struct uwph_cmsghdr) <= msg->msg_controllen && c->cmsg_len
            ? (struct uwph_cmsghdr *)((char *)msg->msg_control + next) : NULL;
    }
    r = do_send(ep, msg->msg_iov, msg->msg_iovlen, pf, npf,
                (fl & UWPH_O_NONBLOCK) || (flags & UWPH_MSG_DONTWAIT));
    if (r < 0) for (int j = 0; j < npf; j++) uwph_obj_unref(pf[j].obj);
    uwph_obj_unref(&ep->base);
    return r;
}

UWPH_API uwph_ssize_t uwph_recvmsg(int fd, struct uwph_msghdr *msg, int flags)
{
    uwph_trace_hit(UWPH_TRACE_RECVMSG);
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    if (uwph_linux_native_fds()) {
        UWPH_NATIVE(uwph_ssize_t, "recvmsg", (int, struct uwph_msghdr *, int));
        UWPH_NATIVE_RETURN(fn(fd, msg, flags));
    }
#endif

    struct passed_fd pf[MAX_PASS_FDS];
    int npf = 0, fl;
    uwph_ssize_t r;
    struct endpoint *ep = get_unix(fd, &fl);
    if (!ep) return -1;
    r = do_recv(ep, msg->msg_iov, msg->msg_iovlen, pf, &npf,
                (fl & UWPH_O_NONBLOCK) || (flags & UWPH_MSG_DONTWAIT), flags & UWPH_MSG_PEEK);
    msg->msg_flags = 0;
    if (r >= 0 && npf) {
        size_t space = msg->msg_controllen;
        int fit = space >= UWPH_CMSG_LEN(sizeof(int)) ? (int)((space - UWPH_CMSG_LEN(0)) / sizeof(int)) : 0;
        struct uwph_cmsghdr *c = msg->msg_control;
        int *out = c ? (int *)UWPH_CMSG_DATA(c) : NULL;
        int n = 0;
        if (fit > npf) fit = npf;
        for (int i = 0; i < npf; i++) {
            int nfd = -1;
            if (i < fit)
                nfd = uwph_fd_alloc(pf[i].kind, pf[i].obj,
                                    pf[i].flags | (flags & UWPH_MSG_CMSG_CLOEXEC ? UWPH_O_CLOEXEC : 0));
            if (nfd < 0) { uwph_obj_unref(pf[i].obj); msg->msg_flags |= UWPH_MSG_CTRUNC; }
            else out[n++] = nfd;
        }
        if (n) {
            c->cmsg_len = UWPH_CMSG_LEN(n * sizeof(int));
            c->cmsg_level = UWPH_SOL_SOCKET;
            c->cmsg_type = UWPH_SCM_RIGHTS;
            msg->msg_controllen = UWPH_CMSG_SPACE(n * sizeof(int));
        } else msg->msg_controllen = 0;
    } else if (r >= 0) msg->msg_controllen = 0;
    uwph_obj_unref(&ep->base);
    return r;
}

UWPH_API uwph_ssize_t uwph_send(int fd, const void *buf, size_t len, int flags)
{
#ifdef UWPH_BACKEND_WIN32
    if (uwph_is_inet(fd)) return uwph_inet_call(fd, INET_SEND, (void *)buf, len, flags, NULL, NULL);
#endif

    struct uwph_iovec iov = { (void *)buf, len };
    struct uwph_msghdr m = { NULL, 0, &iov, 1, NULL, 0, 0 };
    return uwph_sendmsg(fd, &m, flags);
}

UWPH_API uwph_ssize_t uwph_recv(int fd, void *buf, size_t len, int flags)
{
#ifdef UWPH_BACKEND_WIN32
    if (uwph_is_inet(fd)) return uwph_inet_call(fd, INET_RECV, (void *)buf, len, flags, NULL, NULL);
#endif

    struct uwph_iovec iov = { buf, len };
    struct uwph_msghdr m = { NULL, 0, &iov, 1, NULL, 0, 0 };
    return uwph_recvmsg(fd, &m, flags);
}

UWPH_API int uwph_getsockopt(int fd, int level, int name, void *val, uwph_socklen_t *len)
{
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    if (uwph_linux_native_fds()) {
        UWPH_NATIVE(int, "getsockopt", (int, int, int, void *, uwph_socklen_t *));
        UWPH_NATIVE_RETURN(fn(fd, level, name, val, len));
    }
#endif

#ifdef UWPH_BACKEND_WIN32
    if (uwph_is_inet(fd)) return uwph_inet_sockopt(fd, 0, level, name, val, len);
#endif

    struct endpoint *ep = get_unix(fd, NULL);
    int ret = 0;
    if (!ep) return -1;
    if (level != UWPH_SOL_SOCKET) ret = UWPH_FAIL(UWPH_EOPNOTSUPP);
    else if (name == UWPH_SO_PEERCRED && *len >= 12) {
        int32_t cred[3] = { uwph_getpid(), (int32_t)uwph_getuid(), (int32_t)uwph_getuid() };
        memcpy(val, cred, 12);
        *len = 12;
    } else if (*len >= sizeof(int)) {
        int v = name == UWPH_SO_TYPE ? ep->type : name == UWPH_SO_ERROR ? 0
              : (name == UWPH_SO_SNDBUF || name == UWPH_SO_RCVBUF) ? QUEUE_LIMIT : 0;
        memcpy(val, &v, sizeof(v));
        *len = sizeof(v);
    } else ret = UWPH_FAIL(UWPH_EINVAL);
    uwph_obj_unref(&ep->base);
    return ret;
}

UWPH_API int uwph_setsockopt(int fd, int level, int name, const void *val, uwph_socklen_t len)
{
    uwph_trace_hit(UWPH_TRACE_SETSOCKOPT);
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    if (uwph_linux_native_fds()) {
        UWPH_NATIVE(int, "setsockopt", (int, int, int, const void *, uwph_socklen_t));
        UWPH_NATIVE_RETURN(fn(fd, level, name, val, len));
    }
#endif

#ifdef UWPH_BACKEND_WIN32
    if (uwph_is_inet(fd)) return uwph_inet_sockopt(fd, 1, level, name, (void *)val, &len);
#endif

    struct endpoint *ep = get_unix(fd, NULL);
    (void)level; (void)name; (void)val; (void)len;
    if (!ep) return -1;
    uwph_obj_unref(&ep->base);
    return 0;   /* buffer sizes / SO_PASSCRED have no effect in-process */
}

static int fill_name(const char *path, struct uwph_sockaddr *addr, uwph_socklen_t *len)
{
    struct uwph_sockaddr_un un = { UWPH_AF_UNIX, {0} };
    size_t n = sizeof(un.sun_family) + (path[0] ? strlen(path) + 1 : 0);
    strncpy(un.sun_path, path, sizeof(un.sun_path) - 1);
    memcpy(addr, &un, *len < n ? *len : n);
    *len = (uwph_socklen_t)n;
    return 0;
}

UWPH_API int uwph_getsockname(int fd, struct uwph_sockaddr *addr, uwph_socklen_t *len)
{
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    if (uwph_linux_native_fds()) {
        UWPH_NATIVE(int, "getsockname", (int, struct uwph_sockaddr *, uwph_socklen_t *));
        UWPH_NATIVE_RETURN(fn(fd, addr, len));
    }
#endif

#ifdef UWPH_BACKEND_WIN32
    if (uwph_is_inet(fd)) return uwph_inet_call(fd, INET_GETSOCKNAME, NULL, 0, 0, addr, len);
#endif

    struct endpoint *ep = get_unix(fd, NULL);
    int ret;
    if (!ep) return -1;
    ret = fill_name(ep->state == EP_CONNECTED && ep->side == 0 ? "" : ep->path, addr, len);
    uwph_obj_unref(&ep->base);
    return ret;
}

UWPH_API int uwph_getpeername(int fd, struct uwph_sockaddr *addr, uwph_socklen_t *len)
{
#ifdef UWPH_BACKEND_LINUX_PASSTHROUGH
    if (uwph_linux_native_fds()) {
        UWPH_NATIVE(int, "getpeername", (int, struct uwph_sockaddr *, uwph_socklen_t *));
        UWPH_NATIVE_RETURN(fn(fd, addr, len));
    }
#endif

#ifdef UWPH_BACKEND_WIN32
    if (uwph_is_inet(fd)) return uwph_inet_call(fd, INET_GETPEERNAME, NULL, 0, 0, addr, len);
#endif

    struct endpoint *ep = get_unix(fd, NULL);
    int ret;
    if (!ep) return -1;
    ret = ep->state != EP_CONNECTED ? UWPH_FAIL(UWPH_ENOTCONN) : fill_name(ep->peer_path, addr, len);
    uwph_obj_unref(&ep->base);
    return ret;
}

UWPH_API int uwph_getifaddrs(struct uwph_ifaddrs **ifap)
{
    *ifap = NULL;
    return UWPH_ENOSYS_INT();   /* -> GetAdaptersAddresses */
}

UWPH_API void uwph_freeifaddrs(struct uwph_ifaddrs *ifa) { (void)ifa; }
