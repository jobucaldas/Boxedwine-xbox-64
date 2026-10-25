/* Our bootstrapper host; API signatures follow OSC (see PIN.txt). */
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
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
int main(int argc, char **argv) {
    char dll[MAX_PATH];
    HMODULE client;
    typedef void *(__cdecl *factory_fn)(const char *, int *);
    factory_fn factory;
    int rc = 0;
    DWORD len;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc != 2 || strcmp(argv[1], "--selftest")) {
        fprintf(stderr, "usage: steamhost64.exe --selftest\n"); return 2;
    }
    len = GetModuleFileNameA(NULL, install_dir, sizeof(install_dir));
    if (!len || len >= sizeof(install_dir)) return 2;
    char *slash = strrchr(install_dir, '\\');
    if (!slash) return 2;
    *slash = 0;
    if (snprintf(dll, sizeof(dll), "%s\\steamclient64.dll", install_dir) >= (int)sizeof(dll) ||
        snprintf(logging_dir, sizeof(logging_dir), "%s\\logs", install_dir) >= (int)sizeof(logging_dir)) return 2;
    if (!matches_pin(dll)) { puts("pin=mismatch"); return 3; }
    puts("pin=ok");
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
    /* No vtable calls, users, logon, or credentials. Process exit unloads DLL. */
    return engine ? 0 : 5;
}
