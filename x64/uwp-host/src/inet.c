/* Winsock transport for the Linux-layout host surface. Original implementation. */
#include "internal.h"
#include <limits.h>
#ifdef UWPH_BACKEND_WIN32
struct inet_socket {
    struct uwph_object base;
    SOCKET socket;
    int reuseaddr;      /* Linux SO_REUSEADDR: recorded only, see sockopt */
};
static INIT_ONCE startup_once = INIT_ONCE_STATIC_INIT;
static int startup_error;
static BOOL CALLBACK startup(PINIT_ONCE once, void *arg, void **ctx)
{
    WSADATA data;
    startup_error = WSAStartup(MAKEWORD(2,2), &data);
    return TRUE;
}
static int socket_errno(int e)
{
    switch (e) {
    case 0: return 0;
    case WSAEINTR: return UWPH_EINTR;
    case WSAEBADF: return UWPH_EBADF;
    case WSAEACCES: return UWPH_EACCES;
    case WSAEFAULT: return UWPH_EFAULT;
    case WSAEINVAL: return UWPH_EINVAL;
    case WSAEMFILE: return UWPH_EMFILE;
    case WSAEWOULDBLOCK: return UWPH_EAGAIN;
    case WSAEINPROGRESS: return 115;
    case WSAEALREADY: return 114;
    case WSAENOTSOCK: return UWPH_ENOTSOCK;
    case WSAEMSGSIZE: return UWPH_EMSGSIZE;
    case WSAEPROTONOSUPPORT: return UWPH_EPROTONOSUPPORT;
    case WSAENOPROTOOPT: return 92;
    case WSAEOPNOTSUPP: return UWPH_EOPNOTSUPP;
    case WSAEAFNOSUPPORT: return UWPH_EAFNOSUPPORT;
    case WSAEADDRINUSE: return UWPH_EADDRINUSE;
    case WSAEADDRNOTAVAIL: return 99;
    case WSAENETDOWN: return 100;
    case WSAENETUNREACH: return 101;
    case WSAENETRESET: return 102;
    case WSAECONNABORTED: return 103;
    case WSAECONNRESET: return UWPH_ECONNRESET;
    case WSAENOBUFS: return UWPH_ENOMEM;
    case WSAEISCONN: return UWPH_EISCONN;
    case WSAENOTCONN: return UWPH_ENOTCONN;
    case WSAESHUTDOWN: return UWPH_EPIPE;
    case WSAETIMEDOUT: return UWPH_ETIMEDOUT;
    case WSAECONNREFUSED: return UWPH_ECONNREFUSED;
    case WSAEHOSTUNREACH: return 113;
    default: return UWPH_EIO;
    }
}
static int fail_socket(void) { return UWPH_FAIL(socket_errno(WSAGetLastError())); }
static void inet_destroy(struct uwph_object *o)
{
    closesocket(((struct inet_socket *)o)->socket);
    free(o);
}
static int own_socket(SOCKET sock, int flags)
{
    struct inet_socket *s;
    int fd;
    u_long nonblock = 1;
    if (sock == INVALID_SOCKET) return fail_socket();
    if (ioctlsocket(sock, FIONBIO, &nonblock)) { int e = WSAGetLastError(); closesocket(sock); return UWPH_FAIL(socket_errno(e)); }
    s = calloc(1, sizeof(*s));
    if (!s) { closesocket(sock); return UWPH_FAIL(UWPH_ENOMEM); }
    uwph_obj_init(&s->base, inet_destroy);
    s->socket = sock;
    fd = uwph_fd_alloc(UWPH_FD_INET, &s->base, UWPH_O_RDWR | flags);
    if (fd < 0) uwph_obj_unref(&s->base);
    return fd;
}
int uwph_is_inet(int fd)
{
    enum uwph_fd_kind kind;
    uwph_obj_unref(uwph_fd_get(fd, &kind, NULL));
    return kind == UWPH_FD_INET;
}
int uwph_inet_socket(int domain, int type, int proto)
{
    InitOnceExecuteOnce(&startup_once, startup, NULL, NULL);
    if (startup_error) return UWPH_FAIL(socket_errno(startup_error));
    return own_socket(socket(domain == UWPH_AF_INET6 ? AF_INET6 : AF_INET, type & 15, proto),
                      type & (UWPH_SOCK_NONBLOCK | UWPH_SOCK_CLOEXEC));
}
static int poll_socket(SOCKET s, short events, int timeout)
{
    WSAPOLLFD p = {s, events, 0};
    if (WSAPoll(&p, 1, timeout) == SOCKET_ERROR) return fail_socket();
    return p.revents;
}
/* Microsoft's WSAPoll fails with WSAEINVAL when POLLPRI is requested, so
 * urgent data is never reported. */
int uwph_inet_poll(struct uwph_object *o)
{
    int r = poll_socket(((struct inet_socket *)o)->socket, POLLRDNORM | POLLWRNORM, 0);
    if (r < 0) return UWPH_POLLERR;
    return ((r & POLLRDNORM) ? UWPH_POLLIN : 0) | ((r & POLLWRNORM) ? UWPH_POLLOUT : 0) |
           ((r & POLLERR) ? UWPH_POLLERR : 0) |
           ((r & POLLHUP) ? UWPH_POLLHUP : 0) | ((r & POLLNVAL) ? UWPH_POLLNVAL : 0);
}
/* Linux option numbers -> Winsock; -1 = unsupported (ENOPROTOOPT). Linux
 * SO_REUSEADDR only permits rebinding past TIME_WAIT, which Winsock already
 * allows; Winsock's SO_REUSEADDR would let a second socket steal a bound
 * port, so the Linux option is recorded, not forwarded. */
static int sockopt_native(int level, int name, int *nlevel, int *nname)
{
    *nlevel = level;
    switch (level) {
    case UWPH_SOL_SOCKET:
        *nlevel = SOL_SOCKET;
        switch (name) {
        case UWPH_SO_TYPE:   *nname = SO_TYPE; return 0;
        case UWPH_SO_ERROR:  *nname = SO_ERROR; return 0;
        case UWPH_SO_SNDBUF: *nname = SO_SNDBUF; return 0;
        case UWPH_SO_RCVBUF: *nname = SO_RCVBUF; return 0;
        case 6:              *nname = SO_BROADCAST; return 0;  /* Linux SO_BROADCAST */
        case 9:              *nname = SO_KEEPALIVE; return 0;  /* Linux SO_KEEPALIVE */
        }
        return -1;
    case IPPROTO_TCP:                   /* TCP_NODELAY is 1 on both */
        *nname = TCP_NODELAY; return name == 1 ? 0 : -1;
    case IPPROTO_IP:                    /* Linux IP_TOS 1, IP_TTL 2 */
        *nname = name == 1 ? IP_TOS : IP_TTL; return name == 1 || name == 2 ? 0 : -1;
    case 41:                            /* Linux SOL_IPV6, IPV6_V6ONLY 26 */
        *nlevel = IPPROTO_IPV6; *nname = IPV6_V6ONLY; return name == 26 ? 0 : -1;
    }
    return -1;
}

int uwph_inet_sockopt(int fd, int set, int level, int name, void *val, uwph_socklen_t *len)
{
    enum uwph_fd_kind kind;
    struct inet_socket *s = (struct inet_socket *)uwph_fd_get(fd, &kind, NULL);
    int nlevel, nname, n, ret = -1;
    if (kind != UWPH_FD_INET) { uwph_obj_unref((struct uwph_object *)s); return UWPH_FAIL(UWPH_ENOTSOCK); }
    if (!len || !val) { uwph_errno = UWPH_EFAULT; goto done; }
    n = (int)*len;
    if (level == UWPH_SOL_SOCKET && name == UWPH_SO_REUSEADDR) {
        if (n < (int)sizeof(int)) { uwph_errno = UWPH_EINVAL; goto done; }
        if (set) s->reuseaddr = *(int *)val != 0;
        else { *(int *)val = s->reuseaddr; *len = sizeof(int); }
        ret = 0; goto done;
    }
    if (sockopt_native(level, name, &nlevel, &nname)) { uwph_errno = 92; goto done; }
    ret = set ? setsockopt(s->socket, nlevel, nname, val, n) : getsockopt(s->socket, nlevel, nname, val, &n);
    if (ret) { ret = fail_socket(); goto done; }
    if (!set) {
        *len = n;
        if (nlevel == SOL_SOCKET && nname == SO_ERROR && n == sizeof(int)) *(int *)val = socket_errno(*(int *)val);
    }
done:
    uwph_obj_unref(&s->base);
    return ret;
}

/* All native sockets stay nonblocking. Blocking host operations wait without
 * holding the fd table or I/O lock; F_SETFL and MSG_DONTWAIT work per call. */
int uwph_inet_call(int fd, int op, void *buf, size_t size, int flags,
                   struct uwph_sockaddr *addr, uwph_socklen_t *len)
{
    enum uwph_fd_kind kind;
    int fl, ret = -1, n = len ? (int)*len : 0, error, native_flags = 0;
    struct inet_socket *s = (struct inet_socket *)uwph_fd_get(fd, &kind, &fl);
    struct sockaddr_storage storage;
    SOCKET accepted;
    if (kind != UWPH_FD_INET) { uwph_obj_unref((struct uwph_object *)s); return UWPH_FAIL(UWPH_ENOTSOCK); }
    if ((op == INET_GETSOCKNAME || op == INET_GETPEERNAME) && (!addr || !len)) {
        uwph_errno = UWPH_EFAULT; goto done;
    }
    memset(&storage, 0, sizeof(storage));
    if (op == INET_BIND || op == INET_CONNECT || op == INET_SENDTO) {
        if (!addr || !len || (*len != 16 && *len != 28) ||
            (addr->sa_family != UWPH_AF_INET && addr->sa_family != UWPH_AF_INET6) ||
            *len != (addr->sa_family == UWPH_AF_INET ? 16u : 28u)) { uwph_errno = UWPH_EINVAL; goto done; }
        memcpy(&storage, addr, *len);
        storage.ss_family = addr->sa_family == UWPH_AF_INET6 ? AF_INET6 : AF_INET;
        if (op == INET_BIND) {
            unsigned port = ntohs(((struct sockaddr_in *)&storage)->sin_port);
            if (port >= 57344) {
                uwph_log("bind refused: Xbox known issue: ports 57344-65535 silently drop traffic (port %u); use port 0", port);
                uwph_errno = UWPH_EACCES; goto done;
            }
        }
    }
    if (size > INT_MAX) { uwph_errno = UWPH_EMSGSIZE; goto done; }
    if (op >= INET_SEND) {
        if (flags & ~(UWPH_MSG_PEEK | UWPH_MSG_DONTWAIT | UWPH_MSG_NOSIGNAL)) { uwph_errno = UWPH_EOPNOTSUPP; goto done; }
        native_flags = flags & UWPH_MSG_PEEK ? MSG_PEEK : 0;
    }
retry:
    switch (op) {
    case INET_BIND: ret = bind(s->socket, (struct sockaddr *)&storage, n); break;
    case INET_CONNECT: ret = connect(s->socket, (struct sockaddr *)&storage, n); break;
    case INET_LISTEN: ret = listen(s->socket, flags); break;
    case INET_ACCEPT:
        n = sizeof(storage);
        accepted = accept(s->socket, (struct sockaddr *)&storage, &n);
        if (accepted != INVALID_SOCKET) {
            ret = own_socket(accepted, 0);
            if (ret < 0) goto done;
        } else ret = -1;
        break;
    case INET_SHUTDOWN: ret = shutdown(s->socket, flags); break;
    case INET_GETSOCKNAME: case INET_GETPEERNAME:
        n = sizeof(storage);
        ret = op == INET_GETSOCKNAME ? getsockname(s->socket, (struct sockaddr *)&storage, &n) : getpeername(s->socket, (struct sockaddr *)&storage, &n); break;
    case INET_SEND: ret = send(s->socket, buf, (int)size, native_flags); break;
    case INET_RECV: ret = recv(s->socket, buf, (int)size, native_flags); break;
    case INET_SENDTO: ret = sendto(s->socket, buf, (int)size, native_flags, (struct sockaddr *)&storage, n); break;
    case INET_RECVFROM:
        n = sizeof(storage);
        ret = recvfrom(s->socket, buf, (int)size, native_flags, (struct sockaddr *)&storage, &n); break;
    }
    if (ret < 0) {
        error = WSAGetLastError();
        if ((error == WSAEWOULDBLOCK || error == WSAEINPROGRESS) &&
            !(fl & UWPH_O_NONBLOCK) && !(op >= INET_SEND && (flags & UWPH_MSG_DONTWAIT))) {
            if (poll_socket(s->socket, op == INET_ACCEPT || op == INET_RECV || op == INET_RECVFROM ? POLLRDNORM : POLLWRNORM, -1) < 0) goto done;
            if (op == INET_CONNECT) {
                int bytes = sizeof(error);
                ret = getsockopt(s->socket, SOL_SOCKET, SO_ERROR, (char *)&error, &bytes);
                if (!ret && error) ret = UWPH_FAIL(socket_errno(error));
                else if (ret) ret = fail_socket();
                goto done;
            }
            goto retry;
        }
        uwph_errno = op == INET_CONNECT && error == WSAEWOULDBLOCK ? 115 : socket_errno(error);
    } else if ((op == INET_ACCEPT || op == INET_GETSOCKNAME || op == INET_GETPEERNAME || op == INET_RECVFROM) && addr && len) {
        storage.ss_family = storage.ss_family == AF_INET6 ? UWPH_AF_INET6 : UWPH_AF_INET;
        memcpy(addr, &storage, *len < (unsigned)n ? *len : (unsigned)n);
        *len = n;
    }
done:
    uwph_obj_unref(&s->base);
    return ret;
}
#endif

UWPH_API uwph_ssize_t uwph_sendto(int fd, const void *buf, size_t size, int flags, const struct uwph_sockaddr *addr, uwph_socklen_t len)
{
#ifdef UWPH_BACKEND_WIN32
    if (uwph_is_inet(fd)) return uwph_inet_call(fd, addr ? INET_SENDTO : INET_SEND, (void *)buf, size, flags, (struct uwph_sockaddr *)addr, &len);
#endif
    if (!addr) return uwph_send(fd, buf, size, flags);
    return UWPH_ENOSYS_INT();
}
UWPH_API uwph_ssize_t uwph_recvfrom(int fd, void *buf, size_t size, int flags, struct uwph_sockaddr *addr, uwph_socklen_t *len)
{
#ifdef UWPH_BACKEND_WIN32
    if (uwph_is_inet(fd)) return uwph_inet_call(fd, INET_RECVFROM, buf, size, flags, addr, len);
#endif
    if (!addr) return uwph_recv(fd, buf, size, flags);
    return UWPH_ENOSYS_INT();
}
UWPH_API void uwph_freeaddrinfo(struct uwph_addrinfo *ai)
{
    while (ai) { struct uwph_addrinfo *next = ai->ai_next; free(ai->ai_addr); free(ai->ai_canonname); free(ai); ai = next; }
}
UWPH_API int uwph_getaddrinfo(const char *node, const char *service, const struct uwph_addrinfo *hints, struct uwph_addrinfo **out)
{
    if (!out) { uwph_errno = UWPH_EFAULT; return -11; }
    *out = NULL;
#ifdef UWPH_BACKEND_WIN32
    ADDRINFOA h = {0}, *native, *it;
    struct uwph_addrinfo **tail = out;
    int rc;
    InitOnceExecuteOnce(&startup_once, startup, NULL, NULL);
    if (startup_error) { uwph_errno = socket_errno(startup_error); return -11; }
    if (hints) {
        if (hints->ai_family && hints->ai_family != UWPH_AF_INET && hints->ai_family != UWPH_AF_INET6) return -6;
        h.ai_family = hints->ai_family == UWPH_AF_INET6 ? AF_INET6 : hints->ai_family;
        h.ai_socktype = hints->ai_socktype; h.ai_protocol = hints->ai_protocol;
        if (hints->ai_flags & ~(7 | UWPH_AI_V4MAPPED | UWPH_AI_ALL | UWPH_AI_ADDRCONFIG | UWPH_AI_NUMERICSERV)) return -1;
        h.ai_flags = (hints->ai_flags & 7) |
            (hints->ai_flags & UWPH_AI_V4MAPPED ? AI_V4MAPPED : 0) |
            (hints->ai_flags & UWPH_AI_ALL ? AI_ALL : 0) |
            (hints->ai_flags & UWPH_AI_ADDRCONFIG ? AI_ADDRCONFIG : 0) |
            (hints->ai_flags & UWPH_AI_NUMERICSERV ? AI_NUMERICSERV : 0);
    }
    rc = getaddrinfo(node, service, hints ? &h : NULL, &native);
    if (rc) { uwph_errno = socket_errno(rc); return rc == WSAHOST_NOT_FOUND ? -2 : rc == WSATRY_AGAIN ? -3 : rc == WSA_NOT_ENOUGH_MEMORY ? -10 : -4; }
    for (it = native; it; it = it->ai_next) {
        struct uwph_addrinfo *a;
        if (it->ai_family != AF_INET && it->ai_family != AF_INET6) continue;
        a = calloc(1, sizeof(*a));
        if (!a) goto nomem;
        *tail = a; tail = &a->ai_next;
        a->ai_flags = hints ? hints->ai_flags : 0; a->ai_family = it->ai_family == AF_INET6 ? UWPH_AF_INET6 : UWPH_AF_INET;
        a->ai_socktype = it->ai_socktype; a->ai_protocol = it->ai_protocol; a->ai_addrlen = it->ai_addrlen;
        a->ai_addr = malloc(a->ai_addrlen);
        if (!a->ai_addr) goto nomem;
        memcpy(a->ai_addr, it->ai_addr, a->ai_addrlen); a->ai_addr->sa_family = a->ai_family;
        if (it->ai_canonname && !(a->ai_canonname = strdup(it->ai_canonname))) goto nomem;
    }
    freeaddrinfo(native); return 0;
nomem:
    freeaddrinfo(native); uwph_freeaddrinfo(*out); *out = NULL; return -10;
#else
    return UWPH_ENOSYS_INT();
#endif
}
