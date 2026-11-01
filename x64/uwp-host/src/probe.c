/* Runtime API probe (see probe.h). */
#include "internal.h"
#include <stdio.h>

static const char *const cap_names[UWPH_CAP_COUNT] = {
    "VirtualAllocFromApp", "VirtualProtectFromApp", "VirtualAlloc2FromApp",
    "MapViewOfFileFromApp", "MapViewOfFile3FromApp", "codeGeneration RW->RX execute",
    "AddVectoredExceptionHandler", "SetThreadContext", "RtlInstallFunctionTableCallback",
    "WaitOnAddress",
};
static int caps[UWPH_CAP_COUNT];
static int probed;

const char *uwph_probe_name(enum uwph_cap cap) { return cap < UWPH_CAP_COUNT ? cap_names[cap] : "?"; }
int uwph_probe_has(enum uwph_cap cap) { return cap < UWPH_CAP_COUNT && caps[cap]; }

#ifdef UWPH_BACKEND_WIN32
void *uwph_resolve(const char *fn)
{
    static const WCHAR *const mods[] = { L"kernelbase.dll", L"kernel32.dll", L"ntdll.dll" };
    for (unsigned i = 0; i < sizeof(mods) / sizeof(mods[0]); i++) {
        HMODULE m = GetModuleHandleW(mods[i]);
        FARPROC p = m ? GetProcAddress(m, fn) : NULL;
        if (p) return (void *)p;
    }
    return NULL;
}

static int try_codegen(void)
{
    static const unsigned char code[] = { 0xb8, 0x2a, 0x00, 0x00, 0x00, 0xc3 };  /* mov eax,42; ret */
    void *p = uwph_mmap(NULL, 4096, UWPH_PROT_READ | UWPH_PROT_WRITE, UWPH_MAP_PRIVATE | UWPH_MAP_ANONYMOUS, -1, 0);
    int ok;
    if (p == UWPH_MAP_FAILED) return 0;
    memcpy(p, code, sizeof(code));
    if (uwph_mprotect(p, 4096, UWPH_PROT_READ | UWPH_PROT_EXEC) < 0) { uwph_munmap(p, 4096); return 0; }
    ok = ((int (*)(void))p)() == 42;
    uwph_munmap(p, 4096);
    return ok;
}

int uwph_probe_run(void)
{
    static const char *const syms[UWPH_CAP_COUNT] = {
        "VirtualAllocFromApp", "VirtualProtectFromApp", "VirtualAlloc2FromApp",
        "MapViewOfFileFromApp", "MapViewOfFile3FromApp", NULL,
        "AddVectoredExceptionHandler", "SetThreadContext", "RtlInstallFunctionTableCallback",
        "WaitOnAddress",
    };
    int n = 0;
    for (int i = 0; i < UWPH_CAP_COUNT; i++) {
        caps[i] = syms[i] ? uwph_resolve(syms[i]) != NULL : 0;
    }
    /* imported directly (api-ms-win-core-synch): if we are running, it resolved */
    caps[UWPH_CAP_WAIT_ON_ADDRESS] = 1;
    caps[UWPH_CAP_CODEGEN_EXEC] = try_codegen();
    for (int i = 0; i < UWPH_CAP_COUNT; i++) n += caps[i];
    probed = 1;
    return n;
}
#else
void *uwph_resolve(const char *fn) { (void)fn; return NULL; }
int uwph_probe_run(void) { probed = 1; return 0; }
#endif

void uwph_probe_print(void (*out)(const char *line))
{
    char line[160];
    if (!probed) uwph_probe_run();
    for (int i = 0; i < UWPH_CAP_COUNT; i++) {
        snprintf(line, sizeof(line), "  %-34s %s%s\n", cap_names[i], caps[i] ? "yes" : "no",
                 i == UWPH_CAP_VIRTUALPROTECT_FROMAPP && !caps[i] && uwph_mem_protect_is_fromapp() == 0 &&
                 caps[UWPH_CAP_CODEGEN_EXEC] ? "  (test fallback: VirtualProtect, W^X still enforced)" : "");
        out(line);
    }
}
