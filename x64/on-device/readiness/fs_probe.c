/* LocalState port of readiness fs_probe; no desktop known-folder assumptions. */
#include "common.h"
#include <wchar.h>
#include <string.h>
int readiness_fs_main(const WCHAR *root)
{
    WCHAR path[MAX_PATH], lower[MAX_PATH];
    if (wcslen(root) + 32 >= MAX_PATH) return 1;
    swprintf(path, MAX_PATH, L"%ls\\B02_Readiness.TMP", root);
    swprintf(lower, MAX_PATH, L"%ls\\b02_readiness.tmp", root);
    HANDLE h = CreateFile2(path, GENERIC_READ | GENERIC_WRITE, 0, CREATE_NEW, NULL);
    CHECK("create unique LocalState fixture", h != INVALID_HANDLE_VALUE, "err=%lu", GetLastError());
    if (h == INVALID_HANDLE_VALUE) SUMMARY("fs_probe");
    DWORD n = 0; char buf[5] = {0}; LARGE_INTEGER zero = {0}, size = {.QuadPart = 1 << 20};
    BOOL ok = WriteFile(h, "save", 4, &n, NULL) && n == 4 && SetFilePointerEx(h, zero, NULL, FILE_BEGIN) &&
              ReadFile(h, buf, 4, &n, NULL) && n == 4 && !memcmp(buf, "save", 4);
    CHECK("LocalState save write/read", ok, "bytes=%lu", n);
    ok = SetFilePointerEx(h, size, NULL, FILE_BEGIN) && SetEndOfFile(h);
    HANDLE m = ok ? CreateFileMappingFromApp(h, NULL, PAGE_READWRITE, 0, NULL) : NULL;
    char *v = m ? MapViewOfFileFromApp(m, FILE_MAP_WRITE, 0, 0) : NULL;
    if (v) v[12345] = 7;
    CHECK("1MB app-safe mapping RW and flush", v && v[12345] == 7 && FlushViewOfFile(v, 0), "view=%p", v);
    if (v) UnmapViewOfFile(v);
    if (m) CloseHandle(m);
    LARGE_INTEGER pos = {.QuadPart = 12345}; char tag = 0;
    ok = SetFilePointerEx(h, pos, NULL, FILE_BEGIN) && ReadFile(h, &tag, 1, &n, NULL) && n == 1 && tag == 7;
    CHECK("mapping persisted to file", ok, "tag=%d", tag);
    CloseHandle(h);
    h = CreateFile2(lower, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, NULL);
    CHECK("case-insensitive reopen", h != INVALID_HANDLE_VALUE, "err=%lu", GetLastError());
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    CHECK("delete LocalState fixture", DeleteFileW(path), "err=%lu", GetLastError());
    ULARGE_INTEGER free_bytes = {0};
    CHECK("LocalState disk free space", GetDiskFreeSpaceExW(root, &free_bytes, NULL, NULL), "free=%llu", free_bytes.QuadPart);
    INFO("LIMIT: desktop AppData/Documents/TEMP redirection and install-dir writes are not tested");
    SUMMARY("fs_probe");
}
