/* steamapi_probe.c - asks Valve's own redistributable steam_api64.dll whether a Steam
 * client is available, the way any Steamworks game does at startup. Read-only: it only
 * calls the public flat API (IsSteamRunning / GetSteamInstallPath / Init / Shutdown),
 * patches nothing and replaces nothing. Used in R3 to show what a *client* game needs
 * (a running, logged-in Steam client) versus a dedicated server (anonymous gameserver).
 * Usage: steamapi_probe.exe <dir containing steam_api64.dll>   (steam_appid.txt in cwd)
 * Build: x86_64-w64-mingw32-gcc -O2 -o steamapi_probe.exe steamapi_probe.c */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

typedef int (__cdecl *pfn_bool)(void);
typedef const char *(__cdecl *pfn_str)(void);
typedef void (__cdecl *pfn_void)(void);

int main(int argc, char **argv)
{
    if (argc > 1) SetDllDirectoryA(argv[1]);
    HMODULE m = LoadLibraryA("steam_api64.dll");
    if (!m) { printf("LoadLibrary(steam_api64.dll) failed, err=%lu\n", GetLastError()); return 1; }
    char path[MAX_PATH];
    GetModuleFileNameA(m, path, sizeof(path));
    printf("loaded %s\n", path);

    pfn_bool is_running = (pfn_bool)GetProcAddress(m, "SteamAPI_IsSteamRunning");
    pfn_str install_path = (pfn_str)GetProcAddress(m, "SteamAPI_GetSteamInstallPath");
    pfn_bool init = (pfn_bool)GetProcAddress(m, "SteamAPI_Init");
    pfn_void shutdown = (pfn_void)GetProcAddress(m, "SteamAPI_Shutdown");
    if (!is_running || !install_path || !init || !shutdown) { printf("missing exports\n"); return 1; }

    const char *ip = install_path();
    printf("SteamAPI_GetSteamInstallPath = %s\n", ip && *ip ? ip : "(none)");
    printf("SteamAPI_IsSteamRunning      = %d\n", is_running());
    int ok = init();
    printf("SteamAPI_Init                = %d (%s)\n", ok,
           ok ? "connected to a running Steam client" : "no logged-in Steam client to talk to");
    if (ok) shutdown();
    return 0;
}
