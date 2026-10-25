/* B44 O2 negative control: anonymous WinHTTP HTTPS GET with the default
 * (validating) security flags.  No SECURITY_FLAG_IGNORE_* is ever set, so a
 * bad chain must fail.  Wine 10 winhttp (dlls/winhttp/net.c) reports any
 * netconn_verify_cert failure as ERROR_WINHTTP_SECURE_CHANNEL_ERROR (12157);
 * the per-cause code (12045/12037/12038) is in the +winhttp trace.
 *   cert_probe.exe host [host...]  ->  "host status=200" | "host error=N flags=0x.." */
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>

static void CALLBACK status_cb(HINTERNET h, DWORD_PTR ctx, DWORD status, void *info, DWORD len)
{
    if (status == WINHTTP_CALLBACK_STATUS_SECURE_FAILURE && info) *(DWORD *)ctx = *(DWORD *)info;
}

static int probe(const wchar_t *host)
{
    HINTERNET s = WinHttpOpen(L"uwph-b44-cert-probe/1", WINHTTP_ACCESS_TYPE_NO_PROXY, NULL, NULL, 0), c = NULL, r = NULL;
    DWORD status = 0, len = sizeof(status), err = 0, flags = 0;
    WinHttpSetTimeouts(s, 10000, 10000, 10000, 10000);
    if (s && (c = WinHttpConnect(s, host, INTERNET_DEFAULT_HTTPS_PORT, 0)) &&
        (r = WinHttpOpenRequest(c, L"GET", L"/", NULL, NULL, NULL, WINHTTP_FLAG_SECURE)) &&
        WinHttpSetStatusCallback(r, status_cb, WINHTTP_CALLBACK_FLAG_SECURE_FAILURE, 0) != WINHTTP_INVALID_STATUS_CALLBACK &&
        WinHttpSendRequest(r, NULL, 0, NULL, 0, 0, (DWORD_PTR)&flags) && WinHttpReceiveResponse(r, NULL) &&
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &len, NULL))
        printf("%ls status=%lu\n", host, status);
    else {
        err = GetLastError();
        printf("%ls error=%lu flags=0x%lx\n", host, err, flags);
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return status == 200 ? 0 : 1;
}

int wmain(int argc, wchar_t **argv)
{
    int rc = 0;
    for (int i = 1; i < argc; i++) rc |= probe(argv[i]) << (i - 1);
    fflush(stdout);
    return rc;
}
