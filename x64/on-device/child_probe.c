/* Silent packaged x64 helper. The parent supplies verified LocalState. */
#include <windows.h>
#include <wchar.h>
int wmain(int argc, wchar_t **argv)
{
    WCHAR path[MAX_PATH];
    if (argc != 2 || wcslen(argv[1]) + wcslen(L"\\child-ok.txt") >= MAX_PATH)
        return 8;
    wcscpy(path, argv[1]);
    wcscat(path, L"\\child-ok.txt");
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 8;
    DWORD written = 0;
    BOOL ok = WriteFile(file, "ok\n", 3, &written, NULL);
    if (!CloseHandle(file)) ok = FALSE;
    return ok && written == 3 ? 7 : 8;
}
