/* App-safe subset of net_probe: bounded same-process TCP, no Steam endpoints. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include "common.h"
#include <string.h>
static int socket_ready(SOCKET s, int writing)
{
    fd_set fds; FD_ZERO(&fds); FD_SET(s, &fds);
    struct timeval timeout = {2, 0};
    return select(0, writing ? NULL : &fds, writing ? &fds : NULL, NULL, &timeout) == 1;
}
int main(void)
{
    WSADATA wd;
    int rc = WSAStartup(MAKEWORD(2,2), &wd);
    CHECK("WSAStartup 2.2", rc == 0, "rc=%d", rc);
    if (rc) SUMMARY("net_probe");
    SOCKET listener = INVALID_SOCKET, client = INVALID_SOCKET, peer = INVALID_SOCKET;
    struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int ok = listener != INVALID_SOCKET && bind(listener, (struct sockaddr *)&a, sizeof(a)) == 0;
    int len = sizeof(a);
    ok = ok && getsockname(listener, (struct sockaddr *)&a, &len) == 0 && a.sin_port != 0 && listen(listener, 1) == 0;
    CHECK("bind/listen ephemeral loopback", ok, "wsa=%d port=%u", WSAGetLastError(), ntohs(a.sin_port));
    if (!ok) goto done;
    u_long nonblocking = 1;
    client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok = client != INVALID_SOCKET && ioctlsocket(client, FIONBIO, &nonblocking) == 0;
    rc = ok ? connect(client, (struct sockaddr *)&a, sizeof(a)) : SOCKET_ERROR;
    ok = ok && (rc == 0 || WSAGetLastError() == WSAEWOULDBLOCK) && socket_ready(client, 1);
    int error = -1; len = sizeof(error);
    ok = ok && getsockopt(client, SOL_SOCKET, SO_ERROR, (char *)&error, &len) == 0 && !error;
    CHECK("bounded same-process TCP connect", ok, "socket_error=%d wsa=%d", error, WSAGetLastError());
    if (!ok) goto done;
    ok = ioctlsocket(listener, FIONBIO, &nonblocking) == 0 && socket_ready(listener, 0);
    if (ok) peer = accept(listener, NULL, NULL);
    ok = ok && peer != INVALID_SOCKET && ioctlsocket(peer, FIONBIO, &nonblocking) == 0;
    CHECK("bounded TCP accept", ok, "wsa=%d", WSAGetLastError());
    if (!ok) goto done;
    char buf[4] = {0}; int got = 0;
    ok = socket_ready(client, 1) && send(client, "ping", 4, 0) == 4;
    ULONGLONG deadline = GetTickCount64() + 2000;
    while (ok && got < 4 && GetTickCount64() < deadline) {
        if (!socket_ready(peer, 0)) break;
        int n = recv(peer, buf + got, 4 - got, 0);
        if (n <= 0) break;
        got += n;
    }
    CHECK("same-process TCP payload", ok && got == 4 && !memcmp(buf, "ping", 4), "received=%d wsa=%d", got, WSAGetLastError());
done:
    if (peer != INVALID_SOCKET) closesocket(peer);
    if (client != INVALID_SOCKET) closesocket(client);
    if (listener != INVALID_SOCKET) closesocket(listener);
    WSACleanup();
    INFO("LIMIT: no external DNS/TLS/WebSocket, privileged/fixed-port or cross-app loopback checks; no Steam requests");
    SUMMARY("net_probe");
}
