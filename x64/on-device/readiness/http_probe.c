/* Anonymous bootstrap GET; endpoint and WinINet family from B22 O2 evidence.
 * Build: i686-w64-mingw32-gcc -O2 -Wall -Wextra -Werror http_probe.c -lwininet -o http_probe.exe
 * No certificate overrides, credentials, downloaded-code execution or installs. */
#include <windows.h>
#include <wininet.h>
#include <stdio.h>

static int failed(const char *call)
{
    DWORD error = GetLastError();
    printf("FAIL function=%s GetLastError=%lu (0x%08lx)\n", call, error, error);
    return 1;
}

int main(void)
{
    HINTERNET session = NULL, connection = NULL, request = NULL;
    int result = 1;
    DWORD timeout = 15000, status = 0, size = sizeof(status), bytes = 0;
    char buffer[4096];
    unsigned long total = 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    puts("GET https://cdn.steamstatic.com/client/steam_client_win32 API=WinINet proxy=PRECONFIG");
    session = InternetOpenW(L"B30-http-probe", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!session) return failed("InternetOpenW");
    const DWORD options[] = {INTERNET_OPTION_CONNECT_TIMEOUT, INTERNET_OPTION_SEND_TIMEOUT, INTERNET_OPTION_RECEIVE_TIMEOUT};
    for (unsigned i = 0; i < sizeof(options)/sizeof(options[0]); ++i) {
        if (!InternetSetOptionW(session, options[i], &timeout, sizeof(timeout))) {
            result = failed("InternetSetOptionW(timeout)"); goto done;
        }
    }
    connection = InternetConnectW(session, L"cdn.steamstatic.com", INTERNET_DEFAULT_HTTPS_PORT,
                                  NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
    if (!connection) { result = failed("InternetConnectW"); goto done; }
    request = HttpOpenRequestW(connection, L"GET", L"/client/steam_client_win32", NULL,
                               NULL, NULL, INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!request) { result = failed("HttpOpenRequestW"); goto done; }
    puts("CALL HttpSendRequestW");
    if (!HttpSendRequestW(request, NULL, 0, NULL, 0)) { result = failed("HttpSendRequestW"); goto done; }
    if (!HttpQueryInfoW(request, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &size, NULL)) {
        result = failed("HttpQueryInfoW(STATUS_CODE)"); goto done;
    }
    printf("HTTP %lu\n", status);
    do {
        if (!InternetReadFile(request, buffer, sizeof(buffer), &bytes)) {
            result = failed("InternetReadFile"); goto done;
        }
        total += bytes;
    } while (bytes);
    printf("BODY bytes=%lu\n", total);
    result = status == 200 ? 0 : 2;
done:
    if (request) InternetCloseHandle(request);
    if (connection) InternetCloseHandle(connection);
    InternetCloseHandle(session);
    return result;
}
