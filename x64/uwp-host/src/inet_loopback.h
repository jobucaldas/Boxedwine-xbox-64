/* Port 0 is preferred by the Xbox known-issues guidance. If it assigns a
 * restricted high port, search the documented explicit range [1025,49151].
 * Only address-in-use permits trying another port; permission/platform errors
 * stop immediately. Every unsuccessful socket is closed before proceeding. */
static SOCKET loopback_bound(struct sockaddr_in *a)
{
    unsigned port = 0;
    for (;;) {
        SOCKET s;
        int n = sizeof(*a), e;
        memset(a, 0, sizeof(*a));
        a->sin_family = AF_INET;
        a->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a->sin_port = htons((unsigned short)port);
        s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) return s;
        if (bind(s, (struct sockaddr *)a, sizeof(*a))) {
            e = WSAGetLastError();
            closesocket(s);
            if (!port || e != WSAEADDRINUSE || port == 49151) {
                WSASetLastError(e);
                return INVALID_SOCKET;
            }
            ++port;
            continue;
        }
        if (getsockname(s, (struct sockaddr *)a, &n)) {
            e = WSAGetLastError();
            closesocket(s);
            WSASetLastError(e);
            return INVALID_SOCKET;
        }
        if (ntohs(a->sin_port) < 57344) return s;
        closesocket(s);
        port = 1025;
    }
}
