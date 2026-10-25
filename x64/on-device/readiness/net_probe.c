/* Networking a Steam client/game needs, through Wine's winsock/winhttp:
 * UWP internetClient(+Server): outbound OK, servers OK on non-privileged ports, no loopback to OTHER apps. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include "common.h"
#include <winhttp.h>
static int bind_port(int port) { SOCKET s = socket(AF_INET, SOCK_STREAM, 0); struct sockaddr_in a = { AF_INET, htons(port) }; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); int r = bind(s, (void *)&a, sizeof a); int e = WSAGetLastError(); closesocket(s); return r == 0 ? 0 : e; }
static char *https_get(const wchar_t *host, const wchar_t *path, DWORD *status)
{
    HINTERNET s = WinHttpOpen(L"uwp-readiness-probe/1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0), c = NULL, r = NULL; char *buf = NULL; *status = 0;
    if (s && (c = WinHttpConnect(s, host, 443, 0)) && (r = WinHttpOpenRequest(c, L"GET", path, NULL, NULL, NULL, WINHTTP_FLAG_SECURE)) &&
        WinHttpSendRequest(r, NULL, 0, NULL, 0, 0, 0) && WinHttpReceiveResponse(r, NULL)) {
        DWORD sz = sizeof *status; WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, status, &sz, NULL);
        buf = calloc(1, 1 << 20); DWORD got = 0, n;
        while (WinHttpReadData(r, buf + got, (1 << 20) - 1 - got, &n) && n) got += n;
    } else printf("INFO winhttp error %lu\n", GetLastError());
    if (r) WinHttpCloseHandle(r); if (c) WinHttpCloseHandle(c); if (s) WinHttpCloseHandle(s);
    return buf;
}
int main(void)
{
    WSADATA wd; CHECK("WSAStartup 2.2", WSAStartup(MAKEWORD(2, 2), &wd) == 0, "ok");
    int e80 = bind_port(80), e1023 = bind_port(1023), e27036 = bind_port(27036), e0 = bind_port(0);
    printf("BIND80_ERR=%d BIND27036_ERR=%d\n", e80, e27036);
    INFO("bind 127.0.0.1:80 -> %d, :1023 -> %d (UWP: privileged ports unavailable)", e80, e1023);
    CHECK("bind :27036 (Steam client local port)", e27036 == 0, "err=%d", e27036);
    CHECK("bind ephemeral port", e0 == 0, "err=%d", e0);
    /* loopback within the same process (Steam/game IPC that uses sockets) */
    SOCKET l = socket(AF_INET, SOCK_STREAM, 0); struct sockaddr_in a = { AF_INET, 0 }; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(l, (void *)&a, sizeof a); listen(l, 1); int al = sizeof a; getsockname(l, (void *)&a, &al);
    SOCKET c = socket(AF_INET, SOCK_STREAM, 0); int cr = connect(c, (void *)&a, sizeof a); SOCKET acc = accept(l, NULL, NULL);
    char b[8] = {0}; send(c, "ping", 4, 0); recv(acc, b, 4, 0);
    CHECK("same-process loopback TCP", cr == 0 && !strcmp(b, "ping"), "cr=%d", cr);
    closesocket(c); closesocket(acc); closesocket(l);
    struct addrinfo *ai = NULL; int g = getaddrinfo("api.steampowered.com", "443", NULL, &ai);
    CHECK("DNS api.steampowered.com", g == 0 && ai, "err=%d", g); if (ai) freeaddrinfo(ai);
    DWORD st; char *body = https_get(L"api.steampowered.com", L"/ISteamDirectory/GetCMListForConnect/v1/?cellid=0&maxcount=20", &st);
    CHECK("HTTPS (schannel/gnutls) Steam directory API", body && st == 200 && strstr(body, "serverlist"), "status=%lu bytes=%zu", st, body ? strlen(body) : 0);
    char host[256] = {0}; int port = 443;
    char *ws = body ? strstr(body, "\"websockets\"") : NULL;
    if (ws) { char *ep = body; char *best = NULL; while ((ep = strstr(ep, "\"endpoint\":\"")) && ep < ws) { best = ep; ep++; }
        if (best) sscanf(best + 12, "%255[^:]:%d", host, &port); }
    INFO("CM websocket endpoint: %s:%d", host, port);
    int upgraded = 0;
    if (*host) {
        wchar_t wh[256]; MultiByteToWideChar(CP_UTF8, 0, host, -1, wh, 256);
        HINTERNET s = WinHttpOpen(L"uwp-readiness-probe/1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
        HINTERNET cn = WinHttpConnect(s, wh, port, 0);
        HINTERNET r = WinHttpOpenRequest(cn, L"GET", L"/cmsocket/", NULL, NULL, NULL, WINHTTP_FLAG_SECURE);
        if (r && WinHttpSetOption(r, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0) && WinHttpSendRequest(r, NULL, 0, NULL, 0, 0, 0) && WinHttpReceiveResponse(r, NULL)) {
            DWORD code = 0, sz = sizeof code; WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &code, &sz, NULL);
            HINTERNET w = WinHttpWebSocketCompleteUpgrade(r, 0);
            INFO("websocket upgrade status=%lu handle=%p", code, w);
            if (w) { upgraded = 1; WinHttpWebSocketClose(w, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, NULL, 0); WinHttpCloseHandle(w); }
        } else INFO("websocket request error %lu", GetLastError());
        if (r) WinHttpCloseHandle(r); if (cn) WinHttpCloseHandle(cn); if (s) WinHttpCloseHandle(s);
    }
    CHECK("TLS WebSocket upgrade to Steam CM (/cmsocket/)", upgraded, "%s:%d", host, port);
    SUMMARY("net_probe");
}
