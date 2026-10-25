/* steamapi_probe.c - B50 O2: does the game's UNMODIFIED steam_api64.dll reach
 * Valve's real steamclient64.dll (via HKCU\Software\Valve\Steam\ActiveProcess)?
 *
 * Only public, flat Steamworks exports are used. Prints exactly:
 *   SteamAPI_Init=<0|1> msg=<errmsg>
 *   IsSteamRunning=<0|1>
 *   BIsSubscribedApp(3405340)=<0|1>
 *   steamid_present=<0|1>
 * The SteamID and account name are never printed.
 *
 * Build: x86_64-w64-mingw32-gcc -O2 -o steamapi_probe.exe steamapi_probe.c
 * Usage: steamapi_probe.exe <path\to\steam_api64.dll>
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define APPID 3405340u

typedef int (__cdecl *InitFlat_t)(char *errmsg /* SteamErrMsg = char[1024] */);
typedef BOOL (__cdecl *InitSafe_t)(void);
typedef BOOL (__cdecl *IsSteamRunning_t)(void);
typedef void (__cdecl *Shutdown_t)(void);
typedef void *(__cdecl *GetIface_t)(void);
typedef BOOL (__cdecl *BIsSubscribedApp_t)(void *self, unsigned int appid);
typedef unsigned long long (__cdecl *GetSteamID_t)(void *self);

static void one_line(char *s)
{
    for (; *s; s++)
        if (*s == '\r' || *s == '\n')
            *s = ' ';
}

int main(int argc, char **argv)
{
    char errmsg[1024] = "";
    int ok = 0, running = 0, subscribed = 0, steamid_present = 0;
    HMODULE api;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) {
        fprintf(stderr, "usage: %s <steam_api64.dll>\n", argv[0]);
        return 2;
    }
    SetEnvironmentVariableA("SteamAppId", "3405340");
    SetEnvironmentVariableA("SteamGameId", "3405340");

    api = LoadLibraryA(argv[1]);
    if (!api) {
        printf("SteamAPI_Init=0 msg=LoadLibrary failed err=%lu\n", GetLastError());
        printf("IsSteamRunning=0\nBIsSubscribedApp(3405340)=0\nsteamid_present=0\n");
        return 1;
    }

    InitFlat_t init_flat = (InitFlat_t)GetProcAddress(api, "SteamAPI_InitFlat");
    InitSafe_t init_safe = (InitSafe_t)GetProcAddress(api, "SteamAPI_Init");
    IsSteamRunning_t is_running = (IsSteamRunning_t)GetProcAddress(api, "SteamAPI_IsSteamRunning");
    Shutdown_t shutdown = (Shutdown_t)GetProcAddress(api, "SteamAPI_Shutdown");
    GetIface_t get_apps = (GetIface_t)GetProcAddress(api, "SteamAPI_SteamApps_v008");
    GetIface_t get_user = (GetIface_t)GetProcAddress(api, "SteamAPI_SteamUser_v023");
    BIsSubscribedApp_t is_sub = (BIsSubscribedApp_t)GetProcAddress(api, "SteamAPI_ISteamApps_BIsSubscribedApp");
    GetSteamID_t get_id = (GetSteamID_t)GetProcAddress(api, "SteamAPI_ISteamUser_GetSteamID");

    if (init_flat) {
        int r = init_flat(errmsg); /* ESteamAPIInitResult: 0 == k_ESteamAPIInitResult_OK */
        ok = (r == 0);
        if (!ok && !errmsg[0])
            snprintf(errmsg, sizeof(errmsg), "InitFlat result %d", r);
    } else if (init_safe) {
        ok = init_safe() ? 1 : 0;
        if (!ok)
            strcpy(errmsg, "SteamAPI_Init returned false");
    } else {
        strcpy(errmsg, "no SteamAPI_InitFlat/SteamAPI_Init export");
    }
    errmsg[sizeof(errmsg) - 1] = 0;
    one_line(errmsg);
    printf("SteamAPI_Init=%d msg=%s\n", ok, ok ? "" : errmsg);

    if (is_running)
        running = is_running() ? 1 : 0;
    printf("IsSteamRunning=%d\n", running);

    if (ok && get_apps && is_sub) {
        void *apps = get_apps();
        if (apps)
            subscribed = is_sub(apps, APPID) ? 1 : 0;
    }
    printf("BIsSubscribedApp(%u)=%d\n", APPID, subscribed);

    if (ok && get_user && get_id) {
        void *user = get_user();
        if (user)
            steamid_present = get_id(user) != 0;
    }
    printf("steamid_present=%d\n", steamid_present);

    if (ok && shutdown)
        shutdown();
    return ok ? 0 : 1;
}
