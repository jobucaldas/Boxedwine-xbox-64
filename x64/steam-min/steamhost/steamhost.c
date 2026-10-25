/* Our bootstrapper host; API signatures follow OSC (see PIN.txt). */
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "slots.h"
#include "out/host-map.h"
#define API __declspec(dllexport)
static char install_dir[MAX_PATH], logging_dir[MAX_PATH];
API const char *SteamBootstrapper_GetInstallDir(void) { return install_dir; }
API const char *SteamBootstrapper_GetLoggingDir(void) { return logging_dir; }
API int SteamBootstrapper_GetEUniverse(void) { return 1; }
API uint32_t GetBootstrapperVersion(void) { return PIN_VERSION; }
API unsigned char StartCheckingForUpdates(void) { return 1; }
API void ClientUpdateRunFrame(void) {}
API unsigned char IsClientUpdateAvailable(void) { return 0; }
API unsigned char IsClientUpdateOutOfDiskSpace(void) { return 0; }
API void PermitDownloadClientUpdates(unsigned char permit) { (void)permit; }
API const char *GetCurrentClientBeta(void) { return ""; }
API unsigned char CanSetClientBeta(void) { return 1; }
API void SetClientBeta(const char *beta) { (void)beta; }
API int GetClientLauncherType(void) { return 5; }
API void ForceUpdateNextRestart(void) {}
API uint32_t GetClientUpdateBytesDownloaded(void) { return 0; }
API uint32_t GetClientUpdateBytesToDownload(void) { return 0; }

static int matches_pin(const char *path) {
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    unsigned char buf[65536], digest[32];
    char hex[65];
    FILE *f = fopen(path, "rb");
    size_t n;
    int ok = 0;
    if (!f) return 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0) goto done;
    if (BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0) < 0) goto done;
    while ((n = fread(buf, 1, sizeof(buf), f)) != 0)
        if (BCryptHashData(hash, buf, (ULONG)n, 0) < 0) goto done;
    if (ferror(f) || BCryptFinishHash(hash, digest, sizeof(digest), 0) < 0) goto done;
    for (int i = 0; i < 32; ++i) sprintf(hex + 2*i, "%02x", digest[i]);
    ok = strcmp(hex, PIN_SHA256) == 0;
done:
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    fclose(f);
    return ok;
}

#define CB_SERVERS_CONNECTED 101    /* SteamServersConnected_t, public Steamworks */
#define CB_SERVER_CONNECT_FAILURE 102 /* SteamServerConnectFailure_t { EResult; bool } */
typedef struct { int32_t user; int32_t id; uint8_t *param; int32_t size; } CallbackMsg_t;
typedef unsigned char (__cdecl *bgetcb_fn)(int32_t, CallbackMsg_t *);
typedef void (__cdecl *freecb_fn)(int32_t);
static void *mapped_slot(void *object, const char *iface_name, const char *name) {
    for (size_t i = 0; i < sizeof(host_slots)/sizeof(host_slots[0]); ++i)
        if (!strcmp(host_slots[i].iface_name, iface_name) && !strcmp(host_slots[i].name, name))
            return resolve_slot(&host_slots[i], object);
    denied_slot_name = name;
    denied_trap();
    return NULL;
}
#define SLOT(obj, method) mapped_slot(obj, "IClientEngine", method)
#define USER_SLOT(obj, method) mapped_slot(obj, "IClientUser", method)

static int tables_match(void) { return HOST_MAP_BUILD == PIN_VERSION; }

/* account + SteamID from Steam's own loginusers.vdf (seed-login.py wrote it); never printed. */
static int read_loginusers(char *account, size_t acc_len, uint64_t *steamid) {
    char path[MAX_PATH], buf[8192], *p, *q;
    size_t n;
    FILE *f;
    if (snprintf(path, sizeof(path), "%s\\config\\loginusers.vdf", install_dir) >= (int)sizeof(path) ||
        !(f = fopen(path, "rb"))) return 0;
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    for (p = buf; (p = strchr(p, '"')) && !(strspn(p + 1, "0123456789") == 17 && p[18] == '"'); ++p) {}
    if (!p) return 0;
    *steamid = strtoull(p + 1, NULL, 10);
    if (!(p = strstr(p, "\"AccountName\"")) || !(p = strchr(p + 13, '"')) || !(q = strchr(++p, '"')) ||
        q == p || (size_t)(q - p) >= acc_len) return 0;
    memcpy(account, p, q - p);
    account[q - p] = 0;
    return 1;
}

static int active_pid_is_self(void) {
    DWORD pid = 0, sz = sizeof(pid);
    return RegGetValueA(HKEY_CURRENT_USER, "Software\\Valve\\Steam\\ActiveProcess", "pid",
                        RRF_RT_REG_DWORD, NULL, &pid, &sz) == ERROR_SUCCESS && pid == GetCurrentProcessId();
}

/* As Proton steam_helper/steam.c#L65-L70: ActiveProcess pid + SteamClientDll64. */
static int write_active_process(const char *dll) {
    HKEY key;
    DWORD pid = GetCurrentProcessId();
    int ok;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\Valve\\Steam\\ActiveProcess", 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &key, NULL)) return 0;
    ok = !RegSetValueExA(key, "pid", 0, REG_DWORD, (const BYTE *)&pid, sizeof(pid)) &&
         !RegSetValueExA(key, "SteamClientDll64", 0, REG_SZ, (const BYTE *)dll, (DWORD)strlen(dll) + 1);
    RegCloseKey(key);
    return ok;
}

static int logon(HMODULE client, void *engine, const char *dll, int wait_s, int serve_s) {
    bgetcb_fn bget = (bgetcb_fn)(void *)GetProcAddress(client, "Steam_BGetCallback");
    freecb_fn bfree = (freecb_fn)(void *)GetProcAddress(client, "Steam_FreeLastCallback");
    char account[128];
    uint64_t steamid;
    int32_t pipe = 0, user;
    void *iuser;
    int result = -1, eresult = 0, wrote = 0;
    DWORD start = GetTickCount();
    if (!read_loginusers(account, sizeof(account), &steamid)) { puts("loginusers=0"); return 7; }
    puts("loginusers=1");
    user = ((int32_t (*)(void *, int32_t *))SLOT(engine, "CreateGlobalUser"))(engine, &pipe);
    printf("global_user=%d\n", user != 0 && pipe != 0);
    if (!user || !pipe) return 8;
    iuser = ((void *(*)(void *, int32_t, int32_t))SLOT(engine, "GetIClientUser"))(engine, user, pipe);
    if (!iuser) { puts("iclientuser=0"); return 8; }
    printf("cached_login_set=%d\n", ((unsigned char (*)(void *, const char *, unsigned char))
           USER_SLOT(iuser, "SetAccountNameForCachedCredentialLogin"))(iuser, account, 0) != 0);
    SecureZeroMemory(account, sizeof(account));
    printf("logon_call_eresult=%d\n", ((int (*)(void *, uint64_t))USER_SLOT(iuser, "LogOn"))(iuser, steamid));
    while (result < 0 && GetTickCount() - start < (DWORD)wait_s * 1000u) {
        CallbackMsg_t msg;
        ((void (*)(void *))SLOT(engine, "RunFrame"))(engine);
        while (bget(pipe, &msg)) {
            if (msg.id == CB_SERVERS_CONNECTED) result = 1;
            else if (msg.id == CB_SERVER_CONNECT_FAILURE && msg.size >= 4) {
                memcpy(&eresult, msg.param, 4);
                result = 0;
            }
            bfree(pipe);
        }
        Sleep(100);
    }
    if (result == 1) printf("logon=1\n");
    else printf("logon=0 eresult=%d%s\n", eresult, result < 0 ? " timeout=1" : "");
    if (!active_pid_is_self()) {
        wrote = write_active_process(dll);
        printf("activeprocess_written_by=steamhost ok=%d\n", wrote);
    } else puts("activeprocess_written_by=valve");
    printf("activeprocess_pid_is_self=%d\n", active_pid_is_self());
    if (result != 1) return 9;
    /* --serve (B73): stay the master for the game's steam_api; same pump, callbacks discarded. */
    if (serve_s > 0) {
        printf("serving=%d\n", serve_s);
        for (start = GetTickCount(); GetTickCount() - start < (DWORD)serve_s * 1000u; Sleep(100)) {
            CallbackMsg_t msg;
            ((void (*)(void *))SLOT(engine, "RunFrame"))(engine);
            while (bget(pipe, &msg)) bfree(pipe);
        }
        puts("serve_done=1");
    }
    return 0;
}

int main(int argc, char **argv) {
    char dll[MAX_PATH];
    HMODULE client;
    typedef void *(__cdecl *factory_fn)(const char *, int *);
    factory_fn factory;
    int rc = 0, selftest = 0, check = 0, wait_s = 0, serve_s = 0;
    DWORD len;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 2 && !strcmp(argv[1], "--selftest")) selftest = 1;
    else if (argc == 2 && !strcmp(argv[1], "--check-pin")) check = 1;
    else if (!((argc == 4 || (argc == 6 && !strcmp(argv[4], "--serve") &&
                (serve_s = atoi(argv[5])) > 0 && serve_s <= 3600)) &&
               !strcmp(argv[1], "--logon") && !strcmp(argv[2], "--wait") &&
               (wait_s = atoi(argv[3])) > 0 && wait_s <= 3600)) {
        fprintf(stderr, "usage: steamhost64.exe --selftest | --check-pin | --logon --wait SECONDS [--serve SECONDS]\n");
        return 2;
    }
    const char *valid = getenv("STEAMHOST_VALIDATED_MAP");
    if (!valid || strcmp(valid, HOST_MAP_SHA256)) { puts("map_valid=0"); return 12; }
    puts("map_valid=1");
    /* steamhost spawns no process; there is no webhelper (or any child) code path. */
    if (!selftest) puts("webhelper_started=0");
    len = GetModuleFileNameA(NULL, install_dir, sizeof(install_dir));
    if (!len || len >= sizeof(install_dir)) return 2;
    char *slash = strrchr(install_dir, '\\');
    if (!slash) return 2;
    *slash = 0;
    if (snprintf(dll, sizeof(dll), "%s\\steamclient64.dll", install_dir) >= (int)sizeof(dll) ||
        snprintf(logging_dir, sizeof(logging_dir), "%s\\logs", install_dir) >= (int)sizeof(logging_dir)) return 2;
    if (!matches_pin(dll)) { puts("pin=mismatch"); return 3; }
    puts("pin=ok");
    if (!selftest) {
        printf("iface_tables_build=%u client_build=%u\n", (unsigned)HOST_MAP_BUILD, (unsigned)PIN_VERSION);
        if (!tables_match()) {
            puts("iface_pin=mismatch");
            puts("logon=not_attempted reason=no_published_vtable_tables_for_pinned_build");
            puts("activeprocess_pid_is_self=0");
            return 6;
        }
        puts("iface_pin=match");
        if (check) return 0;
    }
    CreateDirectoryA(logging_dir, NULL);
    SetDllDirectoryA(install_dir);
    client = LoadLibraryExA(dll, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!client) { printf("load=0 error=%lu\n", GetLastError()); return 4; }
    factory = (factory_fn)(void *)GetProcAddress(client, "CreateInterface");
    if (!factory || !GetProcAddress(client, "Steam_BGetCallback") ||
        !GetProcAddress(client, "Steam_FreeLastCallback")) { puts("exports=0"); return 4; }
    puts("exports=1");
    void *engine = factory("CLIENTENGINE_INTERFACE_VERSION005", &rc);
    printf("engine=%d\n", engine != NULL);
    if (!engine) return 5;
    /* --selftest: no vtable calls, users, logon, or credentials. Process exit unloads DLL. */
    return selftest ? 0 : logon(client, engine, dll, wait_s, serve_s);
}
