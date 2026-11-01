/* Paths a Steam game + Steam client touch, run with the install dir read-only and only
 * LocalState writable (UWP package install dir is read-only; app data goes to LocalState). */
#include "common.h"
#include <shlobj.h>
static int try_write(const char *path) { HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL); if (h == INVALID_HANDLE_VALUE) return 0; DWORD w; WriteFile(h, "x", 1, &w, NULL); CloseHandle(h); DeleteFileA(path); return 1; }
int main(int argc, char **argv)
{
    char p[MAX_PATH], q[MAX_PATH + 64];
    GetModuleFileNameA(NULL, p, MAX_PATH); INFO("exe: %s", p);
    char *slash = strrchr(p, '\\'); if (slash) *slash = 0;
    snprintf(q, sizeof q, "%s\\write_test.tmp", p);
    int instw = try_write(q);
    printf("INSTALL_DIR_WRITABLE=%d\n", instw);
    INFO("install dir writable: %d (UWP package dir is read-only; games that write next to their exe need redirection)", instw);
    if (SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, p) == S_OK) {
        snprintf(q, sizeof q, "%s\\uwp_probe", p); CreateDirectoryA(q, NULL); strcat(q, "\\save.dat");
        CHECK("write %LOCALAPPDATA% (-> LocalState)", try_write(q), "%s", q);
    }
    if (SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, p) == S_OK) { snprintf(q, sizeof q, "%s\\uwp_probe.tmp", p); CHECK("write %APPDATA%", try_write(q), "%s", q); }
    if (SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, p) == S_OK) { snprintf(q, sizeof q, "%s\\uwp_probe.tmp", p); CHECK("write Documents (Unity/Unreal saves)", try_write(q), "%s", q); }
    GetTempPathA(MAX_PATH, p); snprintf(q, sizeof q, "%suwp_probe.tmp", p); CHECK("write %TEMP%", try_write(q), "%s", q);
    snprintf(q, sizeof q, "%s\\UWP_Case.TMP", p); try_write(q);
    HANDLE h = CreateFileA(q, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL); CloseHandle(h);
    char lower[MAX_PATH + 64]; snprintf(lower, sizeof lower, "%s\\uwp_case.tmp", p);
    CHECK("case-insensitive open", GetFileAttributesA(lower) != INVALID_FILE_ATTRIBUTES, "%s", lower); DeleteFileA(q);
    /* memory-mapped file (asset streaming) */
    snprintf(q, sizeof q, "%suwp_map.bin", p);
    h = CreateFileA(q, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    SetFilePointer(h, 1 << 20, NULL, FILE_BEGIN); SetEndOfFile(h);
    HANDLE m = CreateFileMappingA(h, NULL, PAGE_READWRITE, 0, 0, NULL); char *v = m ? MapViewOfFile(m, FILE_MAP_WRITE, 0, 0, 0) : NULL;
    if (v) { v[12345] = 7; FlushViewOfFile(v, 0); }
    CHECK("1MB memory-mapped file RW", v && v[12345] == 7, "v=%p", v);
    if (v) UnmapViewOfFile(v); if (m) CloseHandle(m); CloseHandle(h); DeleteFileA(q);
    if (argc > 1) { /* read every file under a game dir (proves read-only install access) */
        WIN32_FIND_DATAA fd; snprintf(q, sizeof q, "%s\\*", argv[1]); HANDLE f = FindFirstFileA(q, &fd); int n = 0;
        if (f != INVALID_HANDLE_VALUE) { do n++; while (FindNextFileA(f, &fd)); FindClose(f); }
        CHECK("enumerate game install dir", n > 2, "%d entries in %s", n, argv[1]);
    }
    ULARGE_INTEGER fb, tb, tf; CHECK("GetDiskFreeSpaceEx", GetDiskFreeSpaceExA("C:\\", &fb, &tb, &tf), "free=%llu MB", fb.QuadPart >> 20);
    SUMMARY("fs_probe");
}
