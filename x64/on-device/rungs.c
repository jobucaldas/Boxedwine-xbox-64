/* Native-API + Game-mode rungs n0-n7 (O5).
 *
 * Packaged (UWP-activated on the console): runs as the app's CoreApplication
 * entry point, so n3 gets the real CoreWindow; log goes to
 * ApplicationData.Current.LocalFolder\ladder-native.log.
 * Unpackaged or --console (Linux box under Wine): no CoreWindow, n3 is SKIP;
 * log goes to --local-state DIR (default .\LocalState).
 * Each rung prints "STAGE nX name STATUS: detail [ctx=...]" and flushes, so a
 * crash or OS kill still leaves every earlier line. No credentials are read. */
#define INITGUID
#define COBJMACROS
#include <windows.h>
#include <appmodel.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <xaudio2.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <roapi.h>
#include <winstring.h>
#include <windows.storage.h>
#include <math.h>
#include <errno.h>
#include <wchar.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if !defined(_WIN64) || !defined(__x86_64__)
#error rungs.exe requires an x64 Windows target.
#endif

#define MB (1024ull * 1024ull)

static FILE *g_log;
static const char *g_ctx = "console";
static char g_host[64] = "windows";
static WCHAR g_state[MAX_PATH];
static int g_big_file;
static int g_device;
static int g_packaged;
static int g_local_state;

static void out_line(const char *line)
{
    if (g_log) {
        fputs(line, g_log);
        fputc('\n', g_log);
        fflush(g_log);
    }
    puts(line);
    fflush(stdout);
}

static void note(const char *fmt, ...)
{
    char body[900], line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    snprintf(line, sizeof(line), "# %s", body);
    out_line(line);
}

static void stage(const char *id, const char *name, const char *status, const char *fmt, ...)
{
    char body[900], line[1200];
    if (!g_device && !strcmp(status, "PASS")) status = "SKIP";
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    snprintf(line, sizeof(line), "STAGE %s %s %s: %s [ctx=%s host=%s]",
             id, name, status, body, g_ctx, g_host);
    out_line(line);
}

/* Readiness PE sources linked into this PE: no child process or custom loader. */
static void state_path(WCHAR *dst, size_t n, const WCHAR *leaf);
int readiness_thread_main(void);
int readiness_uwph_main(int argc, char **argv);
static void readiness_rungs(void)
{
    stage("p1", "seh_probe", "SKIP", "source requires AddVectoredExceptionHandler, absent from app API set; no desktop fallback");
    if (!g_device || !g_packaged || !g_local_state) {
        stage("p2", "thread_probe", "SKIP", "requires verified Xbox UWP activation and LocalState; no off-device execution");
    } else {
        int rc = readiness_thread_main();
        stage("p2", "thread_probe", rc ? "FAIL" : "PASS", "in-process native PE probe exit=%d; native Windows APIs, no Wine", rc);
    }
    stage("p3", "fs_probe", "SKIP", "source uses SHGetFolderPathA/CreateFileA and desktop mappings; app-safe port required");
    stage("p4", "net_probe", "SKIP", "original probe includes Steam directory/CM WebSocket tests; Steam probes excluded by O1");
    stage("p5", "jit_probe", "SKIP", "original uses VirtualProtect/VirtualAlloc and RWX dual mappings; full app-safe VirtualProtectFromApp rebuild pending; original PE never executed");
    if (!g_device || !g_packaged || !g_local_state) {
        stage("p6", "uwph-self-test", "SKIP", "requires verified Xbox UWP activation and LocalState; THROUGH-uwph self-test linked, console unverified");
    } else {
        char root[MAX_PATH];
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, g_state, -1, root, sizeof(root), NULL, NULL)) {
            stage("p6", "uwph-self-test", "FAIL", "LocalState UTF-8 conversion err=%lu", GetLastError());
            return;
        }
        WCHAR fixture[MAX_PATH];
        state_path(fixture, MAX_PATH, L"smoke-input.txt");
        HANDLE file = CreateFile2(fixture, GENERIC_WRITE, 0, CREATE_ALWAYS, NULL);
        const char input[] = "uwph smoke input\n";
        DWORD written = 0;
        BOOL ready = file != INVALID_HANDLE_VALUE && WriteFile(file, input, sizeof(input) - 1, &written, NULL);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        if (!ready || written != sizeof(input) - 1) {
            stage("p6", "uwph-self-test", "FAIL", "could not create LocalState smoke-input.txt fixture");
            return;
        }
        char *argv[] = { "uwph-smoke", root, NULL };
        int rc = readiness_uwph_main(2, argv);
        stage("p6", "uwph-self-test", rc ? "FAIL" : "PASS", "THROUGH-uwph Windows smoke source in-process, exit=%d; no Wine", rc);
    }
}

static unsigned long hx(HRESULT hr) { return (unsigned long)hr; }

static FARPROC sym(const WCHAR *dll, const char *name)
{
    HMODULE m = LoadLibraryExW(dll, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m) m = LoadLibraryW(dll);
    return m ? GetProcAddress(m, name) : NULL;
}

static double now_ms(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

static void state_path(WCHAR *dst, size_t n, const WCHAR *leaf)
{
    _snwprintf(dst, n, L"%ls\\%ls", g_state, leaf);
    dst[n - 1] = 0;
}

/* ---- hand-declared WinRT ABI (mingw-w64 lacks these; IIDs from Wine's IDL /
 * the Windows metadata). Only the slots we call are named. ---- */
typedef struct IInspectableVtblX {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(void *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(void *);
    ULONG (STDMETHODCALLTYPE *Release)(void *);
    HRESULT (STDMETHODCALLTYPE *GetIids)(void *, ULONG *, IID **);
    HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(void *, HSTRING *);
    HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(void *, TrustLevel *);
} IInspectableVtblX;

/* Windows.System.IMemoryManagerStatics {5c6c279c-d7ca-4779-9188-4057219ce64c} */
static const GUID IID_IMemoryManagerStatics_ =
    {0x5c6c279c, 0xd7ca, 0x4779, {0x91, 0x88, 0x40, 0x57, 0x21, 0x9c, 0xe6, 0x4c}};
typedef struct IMemoryManagerStatics_ {
    struct {
        IInspectableVtblX base;
        HRESULT (STDMETHODCALLTYPE *get_AppMemoryUsage)(void *, UINT64 *);
        HRESULT (STDMETHODCALLTYPE *get_AppMemoryUsageLimit)(void *, UINT64 *);
        HRESULT (STDMETHODCALLTYPE *get_AppMemoryUsageLevel)(void *, INT32 *);
    } *lpVtbl;
} IMemoryManagerStatics_;

/* Windows.ApplicationModel.Core.ICoreApplication {0aacf7a4-...} */
static const GUID IID_ICoreApplication_ =
    {0x0aacf7a4, 0x5e1d, 0x49df, {0x80, 0x34, 0xfb, 0x6a, 0x68, 0xbc, 0x5e, 0xd1}};
static const GUID IID_IFrameworkView_ =
    {0xfaab5cd0, 0x8924, 0x45ac, {0xad, 0x0f, 0xa0, 0x8f, 0xae, 0x5d, 0x03, 0x24}};
static const GUID IID_IFrameworkViewSource_ =
    {0xcd770614, 0x65c4, 0x426c, {0x94, 0x94, 0x34, 0xfc, 0x43, 0x55, 0x48, 0x62}};
static const GUID IID_IAgileObject_ =
    {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};
typedef struct ICoreApplication_ {
    struct {
        IInspectableVtblX base;
        void *get_Id, *add_Suspending, *remove_Suspending, *add_Resuming, *remove_Resuming;
        void *get_Properties, *GetCurrentView;
        HRESULT (STDMETHODCALLTYPE *Run)(void *, void *view_source);
    } *lpVtbl;
} ICoreApplication_;

/* Windows.UI.Core.ICoreWindow {79b9d5f2-...}: Dispatcher is slot 3, Activate slot 12. */
typedef struct ICoreWindow_ {
    struct {
        IInspectableVtblX base;
        void *get_AutomationHostProvider, *get_Bounds, *get_CustomProperties;
        HRESULT (STDMETHODCALLTYPE *get_Dispatcher)(void *, void **);
        void *get_FlowDirection, *put_FlowDirection, *get_IsInputEnabled, *put_IsInputEnabled;
        void *get_PointerCursor, *put_PointerCursor, *get_PointerPosition, *get_Visible;
        HRESULT (STDMETHODCALLTYPE *Activate)(void *);
    } *lpVtbl;
} ICoreWindow_;

/* Windows.UI.Core.ICoreDispatcher: ProcessEvents is slot 1. 3 = ProcessAllIfPresent, 2 = ProcessUntilQuit. */
typedef struct ICoreDispatcher_ {
    struct {
        IInspectableVtblX base;
        void *get_HasThreadAccess;
        HRESULT (STDMETHODCALLTYPE *ProcessEvents)(void *, int option);
    } *lpVtbl;
} ICoreDispatcher_;

static ICoreDispatcher_ *g_dispatcher;

static void pump(void)
{
    if (g_dispatcher) g_dispatcher->lpVtbl->ProcessEvents(g_dispatcher, 3);
}

/* ---- n0: R4N probe_native.exe, packaged next to us ---- */
static void rung_n0(void)
{
    if (g_packaged) {
        stage("n0", "probe_native", "SKIP", "packaged probe_native.exe present if supplied; UWP cannot launch a child executable via CreateProcess; INCOMPLETE O5 console n0: needs in-process probe adapter");
        return;
    }
    WCHAR exe[MAX_PATH], dir[MAX_PATH], cmd[3 * MAX_PATH], logw[MAX_PATH];
    GetModuleFileNameW(NULL, dir, MAX_PATH);
    WCHAR *slash = wcsrchr(dir, L'\\');
    if (slash) *slash = 0;
    _snwprintf(exe, MAX_PATH, L"%ls\\probe_native.exe", dir);
    exe[MAX_PATH - 1] = 0;
    if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) {
        stage("n0", "probe_native", "SKIP", "probe_native.exe not packaged next to rungs.exe");
        return;
    }
    state_path(logw, MAX_PATH, L"probe_native.log");
    DeleteFileW(logw);
    _snwprintf(cmd, 3 * MAX_PATH, L"\"%ls\" --log \"%ls\"", exe, logw);
    cmd[3 * MAX_PATH - 1] = 0;
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, g_state, &si, &pi)) {
        stage("n0", "probe_native", "FAIL", "CreateProcessW err=%lu (child processes may be blocked in this app container)",
              GetLastError());
        return;
    }
    DWORD w = WaitForSingleObject(pi.hProcess, 15000), code = 0;
    if (w == WAIT_TIMEOUT) { TerminateProcess(pi.hProcess, 1); WaitForSingleObject(pi.hProcess, 5000); }
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    int lines = 0, fails = 0, skips = 0;
    FILE *f = _wfopen(logw, L"r");
    char buf[1024];
    while (f && fgets(buf, sizeof(buf), f)) {
        if (strncmp(buf, "NATIVE ", 7)) continue;
        buf[strcspn(buf, "\r\n")] = 0;
        lines++;
        fails += strstr(buf, "status=FAIL") != NULL;
        skips += strstr(buf, "status=SKIP") != NULL;
        note("n0 | %s", buf);
    }
    if (f) fclose(f);
    int ok = w != WAIT_TIMEOUT && code == 0 && lines > 0 && fails == 0;
    stage("n0", "probe_native", ok ? "PASS" : "FAIL",
          "exit=%lu%s native_lines=%d fail=%d skip=%d (lines copied above as '# n0 |')",
          code, w == WAIT_TIMEOUT ? " timeout" : "", lines, fails, skips);
}

/* ---- n1: native D3D11 device ---- */
static HRESULT make_d3d11(D3D_DRIVER_TYPE type, ID3D11Device **dev, ID3D11DeviceContext **ctx,
                          D3D_FEATURE_LEVEL *fl)
{
    static const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    PFN_D3D11_CREATE_DEVICE create = (PFN_D3D11_CREATE_DEVICE)(void *)sym(L"d3d11.dll", "D3D11CreateDevice");
    if (!create) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    HRESULT hr = create(NULL, type, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 4,
                        D3D11_SDK_VERSION, dev, fl, ctx);
    if (hr == E_INVALIDARG)
        hr = create(NULL, type, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels + 1, 3,
                    D3D11_SDK_VERSION, dev, fl, ctx);
    return hr;
}

static void rung_n1(void)
{
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    D3D_FEATURE_LEVEL fl = 0;
    HRESULT hr = make_d3d11(D3D_DRIVER_TYPE_HARDWARE, &dev, &ctx, &fl);
    if (FAILED(hr)) {
        ID3D11Device *wdev = NULL;
        ID3D11DeviceContext *wctx = NULL;
        D3D_FEATURE_LEVEL wfl = 0;
        HRESULT whr = make_d3d11(D3D_DRIVER_TYPE_WARP, &wdev, &wctx, &wfl);
        stage("n1", "d3d11-device", "FAIL", "hardware hr=0x%08lx; warp hr=0x%08lx fl=0x%x",
              hx(hr), hx(whr), (unsigned)wfl);
        if (wctx) ID3D11DeviceContext_Release(wctx);
        if (wdev) ID3D11Device_Release(wdev);
        return;
    }
    char name[128] = "?";
    unsigned vendor = 0, device = 0;
    unsigned long long vram = 0;
    int software = 0;
    IDXGIDevice *dxgi = NULL;
    IDXGIAdapter *ad = NULL;
    IDXGIAdapter1 *ad1 = NULL;
    if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **)&dxgi)) &&
        SUCCEEDED(IDXGIDevice_GetAdapter(dxgi, &ad))) {
        DXGI_ADAPTER_DESC d;
        if (SUCCEEDED(IDXGIAdapter_GetDesc(ad, &d))) {
            WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof(name), NULL, NULL);
            vendor = d.VendorId;
            device = d.DeviceId;
            vram = d.DedicatedVideoMemory;
        }
        if (SUCCEEDED(IDXGIAdapter_QueryInterface(ad, &IID_IDXGIAdapter1, (void **)&ad1))) {
            DXGI_ADAPTER_DESC1 d1;
            if (SUCCEEDED(IDXGIAdapter1_GetDesc1(ad1, &d1)))
                software = (d1.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
            IDXGIAdapter1_Release(ad1);
        }
        IDXGIAdapter_Release(ad);
    }
    if (dxgi) IDXGIDevice_Release(dxgi);
    /* WARP / Microsoft Basic Render Driver is vendor 0x1414 device 0x8c. */
    int warp = software || (vendor == 0x1414 && device == 0x8c);
    stage("n1", "d3d11-device", warp ? "FAIL" : "PASS",
          "hr=0x%08lx fl=0x%x adapter=\"%s\" vendor=0x%04x device=0x%04x vram_mb=%llu kind=%s",
          hx(hr), (unsigned)fl, name, vendor, device, vram / MB, warp ? "WARP" : "hardware");
    ID3D11DeviceContext_Release(ctx);
    ID3D11Device_Release(dev);
}

/* ---- n2: native D3D12 device at FL 11_0 ---- */
static void rung_n2(void)
{
    PFN_D3D12_CREATE_DEVICE create = (PFN_D3D12_CREATE_DEVICE)(void *)sym(L"d3d12.dll", "D3D12CreateDevice");
    if (!create) {
        stage("n2", "d3d12-device", "FAIL", "d3d12.dll/D3D12CreateDevice not found err=%lu", GetLastError());
        return;
    }
    ID3D12Device *dev = NULL;
    HRESULT hr = create(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev);
    if (FAILED(hr)) {
        stage("n2", "d3d12-device", "FAIL", "D3D12CreateDevice(FL 11_0) hr=0x%08lx", hx(hr));
        return;
    }
    static const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D12_FEATURE_DATA_FEATURE_LEVELS fls = {4, levels, 0};
    HRESULT fhr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FEATURE_LEVELS, &fls, sizeof(fls));
    D3D12_FEATURE_DATA_SHADER_MODEL sm = {D3D_SHADER_MODEL_6_7};
    HRESULT shr;
    /* Older runtimes reject shader models they don't know: step down. */
    while ((shr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))) == E_INVALIDARG &&
           sm.HighestShaderModel > D3D_SHADER_MODEL_5_1)
        sm.HighestShaderModel--;
    stage("n2", "d3d12-device", "PASS", "hr=0x%08lx max_fl=0x%x (hr=0x%08lx) shader_model=0x%x (hr=0x%08lx)",
          hx(hr), SUCCEEDED(fhr) ? (unsigned)fls.MaxSupportedFeatureLevel : 0u, hx(fhr),
          SUCCEEDED(shr) ? (unsigned)sm.HighestShaderModel : 0u, hx(shr));
    ID3D12Device_Release(dev);
}

/* ---- n3: swapchain on the real CoreWindow ---- */
/* Kept alive after n3 so the known colour stays on screen during later rungs. */
static IDXGISwapChain1 *g_swap;

static void rung_n3(IUnknown *window)
{
    if (!window) {
        stage("n3", "corewindow-swapchain", "SKIP", "no CoreWindow: not UWP-activated (unpackaged/--console)");
        return;
    }
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    D3D_FEATURE_LEVEL fl = 0;
    HRESULT hr = make_d3d11(D3D_DRIVER_TYPE_HARDWARE, &dev, &ctx, &fl);
    if (FAILED(hr)) {
        stage("n3", "corewindow-swapchain", "FAIL", "D3D11CreateDevice hr=0x%08lx", hx(hr));
        return;
    }
    IDXGIDevice *dxgi = NULL;
    IDXGIAdapter *ad = NULL;
    IDXGIFactory2 *fac = NULL;
    ID3D11Texture2D *tex = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    hr = ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **)&dxgi);
    if (SUCCEEDED(hr)) hr = IDXGIDevice_GetAdapter(dxgi, &ad);
    if (SUCCEEDED(hr)) hr = IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory2, (void **)&fac);
    if (FAILED(hr)) {
        stage("n3", "corewindow-swapchain", "FAIL", "IDXGIFactory2 hr=0x%08lx", hx(hr));
        goto cleanup;
    }
    DXGI_SWAP_CHAIN_DESC1 d = {0};
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.Scaling = DXGI_SCALING_STRETCH;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    hr = IDXGIFactory2_CreateSwapChainForCoreWindow(fac, (IUnknown *)dev, window, &d, NULL, &g_swap);
    if (FAILED(hr)) {
        stage("n3", "corewindow-swapchain", "FAIL", "CreateSwapChainForCoreWindow(FLIP_DISCARD x2) hr=0x%08lx", hx(hr));
        goto cleanup;
    }
    IDXGISwapChain1_GetDesc1(g_swap, &d);
    hr = IDXGISwapChain1_GetBuffer(g_swap, 0, &IID_ID3D11Texture2D, (void **)&tex);
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)tex, NULL, &rtv);
    if (FAILED(hr)) {
        stage("n3", "corewindow-swapchain", "FAIL", "back buffer RTV hr=0x%08lx", hx(hr));
        goto cleanup;
    }
    /* Known colour: #0078D7 (R=0 G=120 B=215). */
    static const float colour[4] = {0.0f, 120.0f / 255.0f, 215.0f / 255.0f, 1.0f};
    double prev = now_ms(), sum = 0, worst = 0;
    int frames = 0;
    HRESULT phr = S_OK;
    for (int i = 0; i < 120; i++) {
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
        ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, colour);
        phr = IDXGISwapChain1_Present(g_swap, 1, 0);
        if (FAILED(phr)) break;
        pump();
        double t = now_ms(), dt = t - prev;
        prev = t;
        sum += dt;
        if (dt > worst) worst = dt;
        frames++;
    }
    stage("n3", "corewindow-swapchain", frames == 120 ? "PASS" : "FAIL",
          "hr=0x%08lx size=%ux%u BGRA8 FLIP_DISCARD x2 colour=#0078D7 frames=%d/120 present_hr=0x%08lx "
          "avg_ms=%.2f max_ms=%.2f fl=0x%x",
          hx(hr), d.Width, d.Height, frames, hx(phr), frames ? sum / frames : 0.0, worst, (unsigned)fl);
cleanup:
    if (rtv) ID3D11RenderTargetView_Release(rtv);
    if (tex) ID3D11Texture2D_Release(tex);
    if (fac) IDXGIFactory2_Release(fac);
    if (ad) IDXGIAdapter_Release(ad);
    if (dxgi) IDXGIDevice_Release(dxgi);
    ID3D11DeviceContext_Release(ctx);
    ID3D11Device_Release(dev);
}

/* ---- n4: XAudio2 sine + WASAPI activation ---- */
static int wasapi_probe(char *msg, size_t n)
{
    FARPROC activate = sym(L"mmdevapi.dll", "ActivateAudioInterfaceAsync");
    snprintf(msg, n, "wasapi: ActivateAudioInterfaceAsync %s (availability only)",
             activate ? "available" : "export missing");
    return activate != NULL;
}

static void rung_n4(void)
{
    if (!g_device) {
        stage("n4", "audio", "SKIP", "native audio requires verified Xbox UWP activation; no off-device playback");
        return;
    }
    char xa[300], wa[300];
    int xa_ok = 0;
    typedef HRESULT (WINAPI *xa2_fn)(IXAudio2 **, UINT32, XAUDIO2_PROCESSOR);
    xa2_fn create = (xa2_fn)(void *)sym(L"xaudio2_9.dll", "XAudio2Create");
    if (!create) {
        snprintf(xa, sizeof(xa), "xaudio2: xaudio2_9.dll/XAudio2Create not found err=%lu", GetLastError());
    } else {
        IXAudio2 *x = NULL;
        IXAudio2MasteringVoice *mv = NULL;
        IXAudio2SourceVoice *sv = NULL;
        HRESULT hr = create(&x, 0, XAUDIO2_DEFAULT_PROCESSOR);
        const char *step = "XAudio2Create";
        if (SUCCEEDED(hr)) {
            step = "CreateMasteringVoice";
            hr = IXAudio2_CreateMasteringVoice(x, &mv, XAUDIO2_DEFAULT_CHANNELS, XAUDIO2_DEFAULT_SAMPLERATE, 0,
                                               NULL, NULL, AudioCategory_GameEffects);
        }
        static short pcm[48000];
        WAVEFORMATEX wf = {WAVE_FORMAT_PCM, 1, 48000, 96000, 2, 16, 0};
        if (SUCCEEDED(hr)) {
            step = "CreateSourceVoice";
            hr = IXAudio2_CreateSourceVoice(x, &sv, &wf, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL);
        }
        if (SUCCEEDED(hr)) {
            for (int i = 0; i < 48000; i++)
                pcm[i] = (short)(8000.0 * sin(2.0 * 3.14159265358979 * 440.0 * i / 48000.0));
            XAUDIO2_BUFFER b = {0};
            b.Flags = XAUDIO2_END_OF_STREAM;
            b.AudioBytes = sizeof(pcm);
            b.pAudioData = (const BYTE *)pcm;
            step = "SubmitSourceBuffer";
            hr = IXAudio2SourceVoice_SubmitSourceBuffer(sv, &b, NULL);
        }
        if (SUCCEEDED(hr)) {
            step = "Start";
            hr = IXAudio2SourceVoice_Start(sv, 0, XAUDIO2_COMMIT_NOW);
        }
        UINT64 played = 0;
        double t0 = now_ms();
        if (SUCCEEDED(hr)) {
            XAUDIO2_VOICE_STATE st;
            do {
                Sleep(20);
                pump();
                IXAudio2SourceVoice_GetState(sv, &st, 0);
                played = st.SamplesPlayed;
            } while (st.BuffersQueued && now_ms() - t0 < 3000);
            xa_ok = played >= 48000;
            snprintf(xa, sizeof(xa), "xaudio2: 440Hz 1s mono 48k played=%llu samples in %.0f ms",
                     (unsigned long long)played, now_ms() - t0);
        } else {
            snprintf(xa, sizeof(xa), "xaudio2: %s hr=0x%08lx", step, hx(hr));
        }
        if (sv) IXAudio2SourceVoice_DestroyVoice(sv);
        if (mv) IXAudio2MasteringVoice_DestroyVoice(mv);
        if (x) IXAudio2_Release(x);
    }
    int wa_ok = wasapi_probe(wa, sizeof(wa));
    stage("n4", "audio", xa_ok && wa_ok ? "PASS" : "FAIL", "%s; %s (audibility not verified)", xa, wa);
}

/* ---- n5: Game-mode memory ---- */
static void rung_n5(void)
{
    char lim[300] = "";
    UINT64 usage = 0, limit = 0;
    INT32 level = -1;
    HSTRING cls = NULL;
    static const WCHAR name[] = L"Windows.System.MemoryManager";
    IMemoryManagerStatics_ *mm = NULL;
    HRESULT hr = WindowsCreateString(name, (UINT32)wcslen(name), &cls);
    if (SUCCEEDED(hr)) hr = RoGetActivationFactory(cls, &IID_IMemoryManagerStatics_, (void **)&mm);
    if (cls) WindowsDeleteString(cls);
    HRESULT lhr = hr;
    if (SUCCEEDED(hr)) {
        lhr = mm->lpVtbl->get_AppMemoryUsageLimit(mm, &limit);
        mm->lpVtbl->get_AppMemoryUsage(mm, &usage);
        mm->lpVtbl->get_AppMemoryUsageLevel(mm, &level);
        snprintf(lim, sizeof(lim), "AppMemoryUsageLimit=%llu MB (hr=0x%08lx) usage=%llu MB level=%d mode-guess=%s",
                 (unsigned long long)(limit / MB), hx(lhr), (unsigned long long)(usage / MB), (int)level,
                 limit >= 4000 * MB ? "Game" : limit && limit <= 1536 * MB ? "App" : "unknown");
        mm->lpVtbl->base.Release(mm);
    } else {
        snprintf(lim, sizeof(lim), "MemoryManager RoGetActivationFactory hr=0x%08lx", hx(hr));
    }
    if (!g_device) {
        stage("n5", "game-memory", "SKIP", "%s; commit test requires console activation", lim);
        return;
    }
    note("n5 commit test starting; if the log ends here the OS ended the app at its memory limit");
    typedef PVOID (WINAPI *valloc_fn)(PVOID, SIZE_T, ULONG, ULONG);
    valloc_fn valloc = (valloc_fn)(void *)sym(L"kernelbase.dll", "VirtualAllocFromApp");
    enum { CHUNKS = 18 }; /* 18 x 256 MB = 4.5 GB */
    void *chunk[CHUNKS] = {0};
    int got = 0;
    DWORD err = 0;
    if (valloc) {
        for (; got < CHUNKS; got++) {
            chunk[got] = valloc(NULL, 256 * MB, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (!chunk[got]) {
                err = GetLastError();
                break;
            }
            ((volatile char *)chunk[got])[0] = 1; /* commit charge, first page touched only */
        }
        for (int i = 0; i < got; i++) VirtualFree(chunk[i], 0, MEM_RELEASE);
    }
    int ok = SUCCEEDED(lhr) && got > 0;
    stage("n5", "game-memory", ok ? "PASS" : "FAIL",
          "%s; commit %s max=%d MB of 4608 (256 MB chunks, %s) all freed", lim,
          valloc ? "VirtualAllocFromApp" : "VirtualAllocFromApp missing", got * 256,
          got == CHUNKS ? "cap reached" : "stopped");
    if (err) note("n5 commit stop err=%lu", err);
}

/* ---- n6: Series S vs X ---- */
typedef struct {
    UINT32 vendorId; /* GAMING_DEVICE_VENDOR_ID */
    UINT32 deviceId; /* GAMING_DEVICE_DEVICE_ID */
} gaming_model_t;

static const char *model_name(UINT32 id)
{
    switch (id) {
    case 0: return "not-xbox";
    case 0x768BAE26: return "XboxOne";
    case 0x2A7361D9: return "XboxOneS";
    case 0x5AD617C7: return "XboxOneX";
    case 0x10F7CDE3: return "XboxOneXDevkit";
    /* Series values as in the Windows SDK header (also used by R4N probe_native). */
    case 0x1D27FABB: return "XboxSeriesS";
    case 0x2F7A3DFF: return "XboxSeriesX";
    case 0xDE8A5661: return "XboxSeriesXDevkit";
    default: return "unknown";
    }
}

static void rung_n6(void)
{
    typedef HRESULT (WINAPI *info_fn)(gaming_model_t *);
    info_fn f = (info_fn)(void *)sym(L"api-ms-win-gaming-deviceinformation-l1-1-0.dll",
                                    "GetGamingDeviceModelInformation");
    if (!f) {
        stage("n6", "gaming-device-model", "SKIP", "GetGamingDeviceModelInformation unavailable (API set missing)");
        return;
    }
    gaming_model_t m = {0, 0};
    HRESULT hr = f(&m);
    int ok = SUCCEEDED(hr) && m.vendorId == 0xC2EC5032 && m.deviceId != 0;
    stage("n6", "gaming-device-model", ok ? "PASS" : "FAIL", "hr=0x%08lx vendor=0x%x device=0x%08x model=%s",
          hx(hr), m.vendorId, m.deviceId, model_name(m.deviceId));
}

/* ---- n7: >2 GB file in LocalState ---- */
static void rung_n7(void)
{
    WCHAR path[MAX_PATH];
    if (!g_big_file) {
        stage("n7", "big-file", "SKIP", "not requested (--big-file)");
        return;
    }
    if (!g_device || !g_local_state) {
        stage("n7", "big-file", "SKIP", "requires Xbox UWP activation and verified ApplicationData LocalState");
        return;
    }
    const UINT64 total = 2 * 1024 * MB + 256 * MB, block = 4 * MB;
    ULARGE_INTEGER avail;
    if (GetDiskFreeSpaceExW(g_state, &avail, NULL, NULL) && avail.QuadPart < total + 256 * MB) {
        stage("n7", "big-file", "SKIP", "only %llu MB free in LocalState", (unsigned long long)(avail.QuadPart / MB));
        return;
    }
    state_path(path, MAX_PATH, L"big-file.bin");
    CREATEFILE2_EXTENDED_PARAMETERS ep = {sizeof(ep), FILE_ATTRIBUTE_NORMAL,
                                          FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_SEQUENTIAL_SCAN, 0, NULL, NULL};
    HANDLE h = CreateFile2(path, GENERIC_READ | GENERIC_WRITE, 0, CREATE_ALWAYS, &ep);
    if (h == INVALID_HANDLE_VALUE) {
        stage("n7", "big-file", "FAIL", "CreateFile2 err=%lu", GetLastError());
        return;
    }
    unsigned char *buf = VirtualAlloc(NULL, block, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    UINT64 written = 0, verified = 0;
    DWORD werr = 0, rerr = 0, io;
    double t0 = now_ms();
    for (UINT64 i = 0; buf && written < total; i++) {
        memset(buf, (int)(i * 31 + 7) & 0xff, block);
        memcpy(buf, &i, sizeof(i));
        if (!WriteFile(h, buf, (DWORD)block, &io, NULL) || io != block) {
            werr = GetLastError();
            break;
        }
        written += io;
        if ((i & 63) == 0) pump();
    }
    double tw = now_ms() - t0;
    FlushFileBuffers(h);
    LARGE_INTEGER size = {0}, zero = {0};
    GetFileSizeEx(h, &size);
    SetFilePointerEx(h, zero, NULL, FILE_BEGIN);
    t0 = now_ms();
    for (UINT64 i = 0; buf && verified < written; i++) {
        UINT64 tag;
        if (!ReadFile(h, buf, (DWORD)block, &io, NULL) || io != block) {
            rerr = GetLastError();
            break;
        }
        memcpy(&tag, buf, sizeof(tag));
        if (tag != i || buf[block - 1] != (unsigned char)((i * 31 + 7) & 0xff)) {
            rerr = ERROR_CRC;
            break;
        }
        verified += io;
        if ((i & 63) == 0) pump();
    }
    double tr = now_ms() - t0;
    CloseHandle(h); /* delete-on-close */
    if (buf) VirtualFree(buf, 0, MEM_RELEASE);
    int ok = written == total && verified == total && (UINT64)size.QuadPart == total;
    stage("n7", "big-file", ok ? "PASS" : "FAIL",
          "target=%llu MB written=%llu MB (err=%lu, %.0f ms) size=%llu MB verified=%llu MB (err=%lu, %.0f ms) deleted",
          (unsigned long long)(total / MB), (unsigned long long)(written / MB), werr, tw,
          (unsigned long long)((UINT64)size.QuadPart / MB), (unsigned long long)(verified / MB), rerr, tr);
}

static void run_all(IUnknown *window)
{
    if (!g_device) note("off-device: successful stand-in API calls are labelled SKIP; no console PASS");
    note("rungs.exe O5: native-API + Game-mode rungs; ctx=%s host=%s", g_ctx, g_host);
    readiness_rungs();
    rung_n0();
    rung_n1();
    rung_n2();
    rung_n3(window);
    rung_n4();
    rung_n6();
    rung_n7();
    rung_n5(); /* last: the commit test may hit the app's memory limit */
    note("rungs done; run once as App and once as Game and keep both logs");
}

static HRESULT open_log(void)
{
    WCHAR p[MAX_PATH];
    if (!CreateDirectoryW(g_state, NULL)) {
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS) return HRESULT_FROM_WIN32(err);
    }
    state_path(p, MAX_PATH, L"ladder-native.log");
    g_log = _wfopen(p, L"w");
    if (g_log) return S_OK;
    DWORD err = GetLastError();
    return err ? HRESULT_FROM_WIN32(err) : E_FAIL;
}

/* ---- CoreApplication entry (packaged only) ---- */
typedef struct {
    struct {
        IInspectableVtblX base;
        HRESULT (STDMETHODCALLTYPE *Initialize)(void *, void *);
        HRESULT (STDMETHODCALLTYPE *SetWindow)(void *, void *);
        HRESULT (STDMETHODCALLTYPE *Load)(void *, HSTRING);
        HRESULT (STDMETHODCALLTYPE *Run)(void *);
        HRESULT (STDMETHODCALLTYPE *Uninitialize)(void *);
    } *lpVtbl;
} view_t;

typedef struct {
    struct {
        IInspectableVtblX base;
        HRESULT (STDMETHODCALLTYPE *CreateView)(void *, view_t **);
    } *lpVtbl;
} source_t;

static ICoreWindow_ *g_window;

/* Static singletons: reference counting is a no-op. */
static view_t g_view;
static source_t g_source;

static HRESULT STDMETHODCALLTYPE obj_qi(void *This, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IInspectable) || IsEqualIID(riid, &IID_IAgileObject_) ||
        (This == &g_view && IsEqualIID(riid, &IID_IFrameworkView_)) ||
        (This == &g_source && IsEqualIID(riid, &IID_IFrameworkViewSource_))) {
        *out = This;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE obj_addref(void *This) { (void)This; return 2; }
static ULONG STDMETHODCALLTYPE obj_release(void *This) { (void)This; return 1; }
static HRESULT STDMETHODCALLTYPE obj_iids(void *This, ULONG *n, IID **iids)
{
    (void)This;
    *n = 0;
    *iids = NULL;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE obj_name(void *This, HSTRING *s) { (void)This; *s = NULL; return S_OK; }
static HRESULT STDMETHODCALLTYPE obj_trust(void *This, TrustLevel *t) { (void)This; *t = BaseTrust; return S_OK; }

static HRESULT STDMETHODCALLTYPE view_init(void *This, void *app_view) { (void)This; (void)app_view; return S_OK; }
static HRESULT STDMETHODCALLTYPE view_set_window(void *This, void *window)
{
    (void)This;
    g_window = window;
    g_window->lpVtbl->base.AddRef(g_window);
    return S_OK;
}
/* Load receives the manifest EntryPoint, including for Dev Home activation.
 * A separate explicitly named tile provides opt-in without desktop argv. */
static HRESULT STDMETHODCALLTYPE view_load(void *This, HSTRING entry)
{
    (void)This;
    UINT32 n = 0;
    const WCHAR *name = WindowsGetStringRawBuffer(entry, &n);
    static const WCHAR opt_in[] = L"Boxedwine.BigFile";
    g_big_file = n == (sizeof(opt_in) / sizeof(*opt_in) - 1) &&
                 !wmemcmp(name, opt_in, n);
    note("activation EntryPoint=%.*ls --big-file=%s", (int)n, name,
         g_big_file ? "enabled by Big file opt-in tile" : "disabled");
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE view_run(void *This)
{
    (void)This;
    g_window->lpVtbl->Activate(g_window);
    g_window->lpVtbl->get_Dispatcher(g_window, (void **)&g_dispatcher);
    pump();
    run_all((IUnknown *)g_window);
    if (g_log) fclose(g_log);
    g_log = NULL;
    /* Keep the window (and the n3 colour) up until the user leaves the app. */
    if (g_dispatcher) g_dispatcher->lpVtbl->ProcessEvents(g_dispatcher, 2);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE view_uninit(void *This) { (void)This; return S_OK; }

static HRESULT STDMETHODCALLTYPE source_create(void *This, view_t **out);
static struct {
    IInspectableVtblX base;
    HRESULT (STDMETHODCALLTYPE *Initialize)(void *, void *);
    HRESULT (STDMETHODCALLTYPE *SetWindow)(void *, void *);
    HRESULT (STDMETHODCALLTYPE *Load)(void *, HSTRING);
    HRESULT (STDMETHODCALLTYPE *Run)(void *);
    HRESULT (STDMETHODCALLTYPE *Uninitialize)(void *);
} view_vtbl = {{obj_qi, obj_addref, obj_release, obj_iids, obj_name, obj_trust},
               view_init, view_set_window, view_load, view_run, view_uninit};
static struct {
    IInspectableVtblX base;
    HRESULT (STDMETHODCALLTYPE *CreateView)(void *, view_t **);
} source_vtbl = {{obj_qi, obj_addref, obj_release, obj_iids, obj_name, obj_trust}, source_create};
static view_t g_view = {(void *)&view_vtbl};
static source_t g_source = {(void *)&source_vtbl};

static HRESULT STDMETHODCALLTYPE source_create(void *This, view_t **out)
{
    (void)This;
    *out = &g_view;
    return S_OK;
}

/* Packaged evidence must stay in verified ApplicationData LocalState. */
static HRESULT packaged_state_dir(void)
{
    HSTRING cls = NULL, path = NULL;
    static const WCHAR name[] = L"Windows.Storage.ApplicationData";
    __x_ABI_CWindows_CStorage_CIApplicationDataStatics *stat = NULL;
    __x_ABI_CWindows_CStorage_CIApplicationData *data = NULL;
    __x_ABI_CWindows_CStorage_CIStorageFolder *folder = NULL;
    __x_ABI_CWindows_CStorage_CIStorageItem *item = NULL;
    HRESULT hr = WindowsCreateString(name, (UINT32)wcslen(name), &cls);
    if (SUCCEEDED(hr)) hr = RoGetActivationFactory(cls, &IID___x_ABI_CWindows_CStorage_CIApplicationDataStatics, (void **)&stat);
    if (SUCCEEDED(hr)) hr = __x_ABI_CWindows_CStorage_CIApplicationDataStatics_get_Current(stat, &data);
    if (SUCCEEDED(hr)) hr = __x_ABI_CWindows_CStorage_CIApplicationData_get_LocalFolder(data, &folder);
    if (SUCCEEDED(hr)) hr = __x_ABI_CWindows_CStorage_CIStorageFolder_QueryInterface(folder, &IID___x_ABI_CWindows_CStorage_CIStorageItem, (void **)&item);
    if (SUCCEEDED(hr)) hr = __x_ABI_CWindows_CStorage_CIStorageItem_get_Path(item, &path);
    if (SUCCEEDED(hr)) {
        UINT32 n;
        const WCHAR *raw = WindowsGetStringRawBuffer(path, &n);
        g_local_state = n > 0 && n < MAX_PATH;
        if (!g_local_state) hr = HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME);
        _snwprintf(g_state, MAX_PATH, L"%.*ls", (int)n, raw);
        g_state[MAX_PATH - 1] = 0;
    }
    if (path) WindowsDeleteString(path);
    if (item) __x_ABI_CWindows_CStorage_CIStorageItem_Release(item);
    if (folder) __x_ABI_CWindows_CStorage_CIStorageFolder_Release(folder);
    if (data) __x_ABI_CWindows_CStorage_CIApplicationData_Release(data);
    if (stat) __x_ABI_CWindows_CStorage_CIApplicationDataStatics_Release(stat);
    if (cls) WindowsDeleteString(cls);
    return hr;
}

int main(int argc, char **argv)
{
    int console = 0;
    wcscpy(g_state, L"LocalState");
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--console")) console = 1;
        else if (!strcmp(argv[i], "--big-file")) g_big_file = 1;
        else if (!strcmp(argv[i], "--local-state") && i + 1 < argc)
            MultiByteToWideChar(CP_ACP, 0, argv[++i], -1, g_state, MAX_PATH);
        else {
            fprintf(stderr, "usage: rungs.exe [--console] [--local-state DIR] [--big-file]\n");
            return 2;
        }
    }
    typedef const char *(CDECL *wine_ver_fn)(void);
    wine_ver_fn wv = (wine_ver_fn)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version");
    if (wv) snprintf(g_host, sizeof(g_host), "wine-%s(stand-in)", wv());
    RoInitialize(RO_INIT_SINGLETHREADED);
    UINT32 len = 0;
    int packaged = GetCurrentPackageFullName(&len, NULL) != APPMODEL_ERROR_NO_PACKAGE;
    g_packaged = packaged;
    typedef HRESULT (WINAPI *model_fn)(gaming_model_t *);
    model_fn model = (model_fn)(void *)sym(L"api-ms-win-gaming-deviceinformation-l1-1-0.dll",
                                           "GetGamingDeviceModelInformation");
    gaming_model_t info = {0, 0};
    g_device = packaged && !wv && !console && model && SUCCEEDED(model(&info)) &&
               info.vendorId == 0xC2EC5032 &&
               (info.deviceId == 0x1D27FABB || info.deviceId == 0x2F7A3DFF || info.deviceId == 0xDE8A5661);
    if (packaged && !console) {
        g_ctx = "uwp-corewindow";
        HRESULT evidence_hr = packaged_state_dir();
        if (SUCCEEDED(evidence_hr)) evidence_hr = open_log();
        if (FAILED(evidence_hr)) {
            WCHAR message[256];
            _snwprintf(message, 256, L"FATAL: verified LocalState/evidence log unavailable; hr=0x%08lx errno=%d. No probes ran.\n",
                       hx(evidence_hr), errno);
            OutputDebugStringW(message);
            fwprintf(stderr, L"%ls", message);
            /* Propagate the original failure to activation/Device Portal diagnostics. */
            return (int)evidence_hr;
        }
        static const WCHAR name[] = L"Windows.ApplicationModel.Core.CoreApplication";
        HSTRING cls = NULL;
        ICoreApplication_ *app = NULL;
        HRESULT hr = WindowsCreateString(name, (UINT32)wcslen(name), &cls);
        if (SUCCEEDED(hr)) hr = RoGetActivationFactory(cls, &IID_ICoreApplication_, (void **)&app);
        if (SUCCEEDED(hr)) hr = app->lpVtbl->Run(app, &g_source);
        if (FAILED(hr)) {
            g_device = 0;
            note("CoreApplication.Run hr=0x%08lx; running rungs without a CoreWindow", hx(hr));
            g_ctx = "packaged-no-corewindow";
            run_all(NULL);
        }
        if (cls) WindowsDeleteString(cls);
    } else {
        g_ctx = packaged ? "packaged-console" : "console";
        if (FAILED(open_log())) {
            fprintf(stderr, "rungs.exe: cannot write ladder-native.log in --local-state\n");
            return 1;
        }
        run_all(NULL);
    }
    if (g_log) fclose(g_log);
    return 0;
}
