/*
 * Sockets.
 *
 * AF_UNIX is the wineserver transport: ntdll connects to
 * $WINEPREFIX-derived  /tmp/.wine-<uid>/server-<dev>-<ino>/socket, then
 * passes pipe fds with SCM_RIGHTS (server.c wine_server_send_fd /
 * receive_fd). wineserver runs in-process (server.h), so AF_UNIX is an
 * in-process object: a path namespace of listeners, stream pairs with byte
 * queues, and fd passing that just dups fd-table entries.
 *
 * AF_INET/AF_INET6 (ws2_32.so, nsiproxy) go to Winsock; UWP allows it with
 * the internetClient / internetClientServer / privateNetworkClientServer
 * capabilities. AF_NETLINK (route/link queries) -> GetAdaptersAddresses.
 */
#ifndef UWPH_SOCKET_H
#define UWPH_SOCKET_H
#ifdef __cplusplus
extern "C" {
#endif

UWPH_API int uwph_socket(int domain, int type, int proto);
UWPH_API int uwph_socketpair(int domain, int type, int proto, int sv[2]);
UWPH_API int uwph_bind(int fd, const struct uwph_sockaddr *addr, uwph_socklen_t len);
UWPH_API int uwph_listen(int fd, int backlog);
UWPH_API int uwph_accept(int fd, struct uwph_sockaddr *addr, uwph_socklen_t *len);
UWPH_API int uwph_connect(int fd, const struct uwph_sockaddr *addr, uwph_socklen_t len);
UWPH_API int uwph_shutdown(int fd, int how);
UWPH_API uwph_ssize_t uwph_send(int fd, const void *buf, size_t len, int flags);
UWPH_API uwph_ssize_t uwph_recv(int fd, void *buf, size_t len, int flags);
UWPH_API uwph_ssize_t uwph_sendmsg(int fd, const struct uwph_msghdr *msg, int flags);
UWPH_API uwph_ssize_t uwph_recvmsg(int fd, struct uwph_msghdr *msg, int flags);
UWPH_API int uwph_getsockopt(int fd, int level, int name, void *val, uwph_socklen_t *len);
UWPH_API int uwph_setsockopt(int fd, int level, int name, const void *val, uwph_socklen_t len);
UWPH_API int uwph_getsockname(int fd, struct uwph_sockaddr *addr, uwph_socklen_t *len);
UWPH_API int uwph_getpeername(int fd, struct uwph_sockaddr *addr, uwph_socklen_t *len);

#define UWPH_AI_PASSIVE 1
#define UWPH_AI_CANONNAME 2
#define UWPH_AI_NUMERICHOST 4
#define UWPH_AI_V4MAPPED 8
#define UWPH_AI_ALL 16
#define UWPH_AI_ADDRCONFIG 32
#define UWPH_AI_NUMERICSERV 1024

struct uwph_addrinfo {
    int ai_flags, ai_family, ai_socktype, ai_protocol;
    uwph_socklen_t ai_addrlen;
    struct uwph_sockaddr *ai_addr;
    char *ai_canonname;
    struct uwph_addrinfo *ai_next;
};
UWPH_API uwph_ssize_t uwph_sendto(int fd, const void *buf, size_t size, int flags, const struct uwph_sockaddr *addr, uwph_socklen_t len);
UWPH_API uwph_ssize_t uwph_recvfrom(int fd, void *buf, size_t size, int flags, struct uwph_sockaddr *addr, uwph_socklen_t *len);
UWPH_API int uwph_getaddrinfo(const char *node, const char *service, const struct uwph_addrinfo *hints, struct uwph_addrinfo **out);
UWPH_API void uwph_freeaddrinfo(struct uwph_addrinfo *ai);

struct uwph_ifaddrs;
UWPH_API int  uwph_getifaddrs(struct uwph_ifaddrs **ifap);
UWPH_API void uwph_freeifaddrs(struct uwph_ifaddrs *ifa);

/* in-process stream queue state, for poll/epoll */
/* (readiness of in-process sockets: see uwph_unix_poll_locked in src/internal.h) */

#ifdef __cplusplus
}
#endif
#endif
