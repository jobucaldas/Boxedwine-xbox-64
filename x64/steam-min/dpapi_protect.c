/* dpapi_protect.c - B50 O2 helper for seed-login.py. Runs INSIDE the Wine prefix
 * so the blob is made by that prefix's own CryptProtectData (what Valve's client
 * later reads back with CryptUnprotectData).
 *
 * stdin:  line 1 = optional entropy (may be empty), then the secret (rest of stdin,
 *         no trailing newline). Nothing secret is ever taken from argv.
 * stdout: hex of the DPAPI blob, nothing else.
 *
 * Build: x86_64-w64-mingw32-gcc -O2 -o dpapi_protect.exe dpapi_protect.c -lcrypt32
 */
#include <windows.h>
#include <wincrypt.h>
#include <stdio.h>
#include <fcntl.h>
#include <io.h>

int main(void)
{
    static unsigned char buf[16384];
    size_t len = 0, n, i, nl;
    DATA_BLOB in, ent, out;

    _setmode(_fileno(stdin), _O_BINARY);
    while ((n = fread(buf + len, 1, sizeof(buf) - len, stdin)) > 0)
        len += n;
    for (nl = 0; nl < len && buf[nl] != '\n'; nl++)
        ;
    if (nl >= len) {
        fprintf(stderr, "dpapi_protect: need '<entropy>\\n<secret>' on stdin\n");
        return 2;
    }
    ent.pbData = buf;
    ent.cbData = (DWORD)nl;
    in.pbData = buf + nl + 1;
    in.cbData = (DWORD)(len - nl - 1);

    if (!CryptProtectData(&in, NULL, ent.cbData ? &ent : NULL, NULL, NULL,
                          CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        fprintf(stderr, "dpapi_protect: CryptProtectData failed %lu\n", GetLastError());
        SecureZeroMemory(buf, sizeof(buf));
        return 1;
    }
    SecureZeroMemory(buf, sizeof(buf));
    for (i = 0; i < out.cbData; i++)
        printf("%02x", out.pbData[i]);
    LocalFree(out.pbData);
    return 0;
}
