/* Desktop services oracle, loaded by Wine's own PE loader. */
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static int failures;
static void check(const char *feature, BOOL ok)
{
    printf("DESK %s %s\n", feature, ok ? "PASS" : "FAIL");
    fflush(stdout);
    if (!ok) ++failures;
}
int main(int argc, char **argv)
{
    HWND desktop, window, child, parent;
    HDC dc;
    HBRUSH brush;
    RECT rect = {0, 0, 320, 240};
    COLORREF color = RGB(27, 103, 191);
    BOOL drawn = FALSE, clip = FALSE;
    DEVMODEW mode = {0};
    const WCHAR text[] = L"desk_probe clipboard roundtrip";
    HGLOBAL data;
    WCHAR *ptr;
    if (argc > 1 && !strcmp(argv[1], "--self-host")) {
        HMODULE module = LoadLibraryW(L"uwpdesktop.dll");
        LONG (WINAPI *start)(DWORD) = module ? (void *)GetProcAddress(module, "uwp_desktop_start") : NULL;
        if (!module) printf("DESK self_host load_error=%lu\n", GetLastError());
        LONG status = start ? start(10000) : (LONG)0xc0000135;
        printf("DESK self_host status=%08lx\n", (unsigned long)status);
        if (status) { check("result", FALSE); return 1; }
        check("self_host_repeat", start(10000) == 0);
    }
    desktop = GetDesktopWindow();
    check("desktop_window", desktop != NULL);
    check("display_guid", desktop && GetPropW(desktop, L"__wine_display_device_guid") != NULL);
    window = CreateWindowExW(0, L"STATIC", L"Desk probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                             CW_USEDEFAULT, CW_USEDEFAULT, 320, 240, NULL, NULL, NULL, NULL);
    if (window) {
        ShowWindow(window, SW_SHOW);
        UpdateWindow(window);
        dc = GetDC(window);
        if (dc) {
            brush = CreateSolidBrush(color);
            if (brush) {
                drawn = FillRect(dc, &rect, brush) && GetPixel(dc, 10, 10) == color;
                DeleteObject(brush);
            }
            ReleaseDC(window, dc);
        }
    }
    check("driver_window", drawn);
    child = CreateWindowExW(0, L"STATIC", L"desk_probe_message", 0, 0, 0, 0, 0,
                            HWND_MESSAGE, NULL, NULL, NULL);
    parent = child ? GetAncestor(child, GA_PARENT) : NULL;
    check("message_window", child && parent && parent != desktop &&
          FindWindowExW(HWND_MESSAGE, NULL, L"STATIC", L"desk_probe_message") == child);
    if (OpenClipboard(window)) {
        if (EmptyClipboard()) {
            data = GlobalAlloc(GMEM_MOVEABLE, sizeof(text));
            if (data) {
                ptr = GlobalLock(data);
                if (ptr) {
                    memcpy(ptr, text, sizeof(text));
                    GlobalUnlock(data);
                    if (SetClipboardData(CF_UNICODETEXT, data)) {
                        data = GetClipboardData(CF_UNICODETEXT);
                        ptr = data ? GlobalLock(data) : NULL;
                        if (ptr) { clip = wcscmp(ptr, text) == 0; GlobalUnlock(data); }
                    } else GlobalFree(data);
                } else GlobalFree(data);
            }
        }
        CloseClipboard();
    }
    check("clipboard", clip);
    mode.dmSize = sizeof(mode);
    check("display_modes", EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &mode) && mode.dmPelsWidth > 0);
    printf("DESK systray INFO present=%d\n", FindWindowW(L"Shell_TrayWnd", NULL) != NULL);
    if (child) DestroyWindow(child);
    if (window) DestroyWindow(window);
    printf("DESK result %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
