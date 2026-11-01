/* Controlled allocation coverage for the production Win32 binding helper.
 * No network/platform claim: socket operations below are deterministic fakes. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <netinet/in.h>
#include <errno.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define WSAEADDRINUSE EADDRINUSE
static int error, opened, closed, live, mode, binds, queries;
static unsigned last_port;
static int fake_socket(int domain, int type, int protocol)
{
    assert(domain == AF_INET && type == SOCK_STREAM && protocol == IPPROTO_TCP);
    ++opened; ++live;
    return opened;
}
static int fake_close(int s) { assert(s > 0 && live == 1); --live; ++closed; error = 999; return 0; }
static int fake_bind(int s, const struct sockaddr *a, int size)
{
    const struct sockaddr_in *in = (const struct sockaddr_in *)a;
    assert(s > 0 && size == sizeof(*in));
    assert(in->sin_addr.s_addr == htonl(INADDR_LOOPBACK));
    last_port = ntohs(in->sin_port);
    assert(last_port == 0 || (last_port >= 1025 && last_port <= 49151));
    ++binds;
    if (last_port && (mode == 1 || (mode == 0 && last_port < 1045))) {
        error = WSAEADDRINUSE; return -1;
    }
    if (last_port && mode == 2) { error = EACCES; return -1; }
    return 0;
}
static int fake_name(int s, struct sockaddr *a, int *size)
{
    (void)s; (void)size;
    ++queries;
    if (mode == 3) { error = EIO; return -1; }
    ((struct sockaddr_in *)a)->sin_port = htons(last_port ? last_port : 60000);
    return 0;
}
#define socket fake_socket
#define bind fake_bind
#define getsockname fake_name
#define closesocket fake_close
#define WSAGetLastError() error
#define WSASetLastError(e) (error = (e))
#include "inet_loopback.h"
static void reset(int m) { assert(live == 0); mode = m; opened = closed = binds = queries = 0; }
int main(void)
{
    struct sockaddr_in a;
    /* More high port-0 assignments than the former 17-attempt cap, each
     * followed by occupied allowed ports and then a usable allowed port. */
    reset(0);
    for (int i = 0; i < 32; ++i) {
        SOCKET s = loopback_bound(&a);
        assert(s != INVALID_SOCKET && ntohs(a.sin_port) == 1045);
        fake_close(s);
    }
    assert(opened == closed && live == 0 && binds == 32 * 22);
    reset(1);
    assert(loopback_bound(&a) == INVALID_SOCKET && error == WSAEADDRINUSE);
    assert(binds == 1 + 49151 - 1025 + 1 && opened == closed && live == 0);
    reset(2);
    assert(loopback_bound(&a) == INVALID_SOCKET && error == EACCES);
    assert(binds == 2 && opened == closed && live == 0);
    reset(3);
    assert(loopback_bound(&a) == INVALID_SOCKET && error == EIO);
    assert(binds == 1 && queries == 1 && opened == closed && live == 0);
    puts("inet_loopback controlled high-port run, exhaustion, refusal, cleanup PASS");
    return 0;
}
