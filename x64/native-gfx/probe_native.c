/* probe_native.c - R4N O1: one-shot native-API probe for the R4D on-device ladder.
 *
 * Prints one "NATIVE <what> ..." line per API family and mirrors every line to a
 * log file (--log <path>, default probe_native.log in the cwd):
 *   NATIVE host     which loader/OS we are under (Wine = off-device stand-in)
 *   NATIVE memory   GlobalMemoryStatusEx avail/total
 *   NATIVE gaming   GetGamingDeviceModelInformation (SKIP if the api set is absent)
 *   NATIVE dxgi     CreateDXGIFactory2 + QI IDXGIFactory2
 *   NATIVE d3d11    D3D11CreateDevice(HARDWARE, 11_1..10_0): hr, FL, adapter, WARP?
 *   NATIVE d3d12    D3D12CreateDevice(default adapter, FL 11_0)
 *   NATIVE xaudio2  XAudio2Create via LoadLibraryW("xaudio2_9.dll")
 * Cheap/safe probes run first and every line is flushed, so a crash inside a
 * driver still leaves the earlier lines in the log. Always exits 0; a failing
 * API is reported as a line with status=FAIL, not as a process failure. */
#define COBJMACROS
#define INITGUID
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_3.h>
#include <xaudio2.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* gamingdeviceinformation.h is not in mingw-w64; layout from the Windows SDK. */
typedef struct {
    UINT32 vendorId;
    UINT32 deviceId;
} probe_gaming_device_model_information;
typedef HRESULT (WINAPI *pfn_GetGamingDeviceModelInformation)(probe_gaming_device_model_information *);
typedef HRESULT (WINAPI *pfn_XAudio2Create)(IXAudio2 **, UINT32, XAUDIO2_PROCESSOR);
typedef const char *(CDECL *pfn_wine_get_version)(void);

static FILE *g_log;

static void emit(const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    fprintf(stdout, "%s\n", line);
    fflush(stdout);
    if (g_log) {
        fprintf(g_log, "%s\n", line);
        fflush(g_log);
    }
}

static const char *status(HRESULT hr) { return SUCCEEDED(hr) ? "OK" : "FAIL"; }

/* "builtin" = Wine's own PE dll (carries the marker after the DOS header),
 * "native" = a real Windows/console dll or a drop-in like DXVK. */
static const char *module_kind(const char *name)
{
    const char *base = (const char *)GetModuleHandleA(name);
    if (!base) return "notloaded";
    return memcmp(base + 0x40, "Wine builtin DLL", 16) ? "native" : "builtin";
}

static void probe_host(void)
{
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    pfn_wine_get_version wgv = ntdll ? (pfn_wine_get_version)(void *)GetProcAddress(ntdll, "wine_get_version") : NULL;
    if (wgv)
        emit("NATIVE host os=wine-%s note=stand-in (off-device; results describe Wine, not the Xbox)", wgv());
    else
        emit("NATIVE host os=windows note=real system dlls");
}

static void probe_memory(void)
{
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms))
        emit("NATIVE memory status=OK avail_mb=%llu total_mb=%llu load=%lu%%",
             (unsigned long long)(ms.ullAvailPhys >> 20), (unsigned long long)(ms.ullTotalPhys >> 20),
             (unsigned long)ms.dwMemoryLoad);
    else
        emit("NATIVE memory status=FAIL err=%lu", GetLastError());
}

static const char *gaming_device_name(UINT32 id)
{
    switch (id) {
    case 0x768BAE26: return "XboxOne";
    case 0x2A7361D9: return "XboxOneS";
    case 0x5AD617C7: return "XboxOneX";
    case 0x10F7CDE3: return "XboxOneXDevkit";
    case 0x1D27FABB: return "XboxSeriesS";
    case 0x2F7A3DFF: return "XboxSeriesX";
    case 0xDE8A5661: return "XboxSeriesXDevkit";
    case 0x00000000: return "Unknown";
    default:         return "other";
    }
}

static void probe_gaming(void)
{
    HMODULE m = LoadLibraryW(L"api-ms-win-gaming-deviceinformation-l1-1-0.dll");
    pfn_GetGamingDeviceModelInformation fn =
        m ? (pfn_GetGamingDeviceModelInformation)(void *)GetProcAddress(m, "GetGamingDeviceModelInformation") : NULL;
    if (!fn) {
        emit("NATIVE gaming status=SKIP reason=%s", m ? "no-export" : "no-apiset");
        return;
    }
    probe_gaming_device_model_information info = {0, 0};
    HRESULT hr = fn(&info);
    emit("NATIVE gaming status=%s hr=0x%lx vendor=0x%08x device=0x%08x model=%s", status(hr),
         (unsigned long)hr, info.vendorId, info.deviceId, gaming_device_name(info.deviceId));
}

static void probe_dxgi(void)
{
    IDXGIFactory2 *f = NULL;
    HRESULT hr = CreateDXGIFactory2(0, &IID_IDXGIFactory2, (void **)&f);
    HRESULT qhr = E_FAIL;
    if (SUCCEEDED(hr)) {
        IDXGIFactory2 *f2 = NULL;
        qhr = IDXGIFactory2_QueryInterface(f, &IID_IDXGIFactory2, (void **)&f2);
        if (SUCCEEDED(qhr)) IDXGIFactory2_Release(f2);
        IDXGIFactory2_Release(f);
    }
    emit("NATIVE dxgi status=%s hr=0x%lx qi_factory2_hr=0x%lx dll=%s", status(SUCCEEDED(hr) ? qhr : hr),
         (unsigned long)hr, (unsigned long)qhr, module_kind("dxgi.dll"));
}

static void probe_d3d11(void)
{
    static const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
    };
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    D3D_FEATURE_LEVEL fl = 0;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, levels, ARRAYSIZE(levels),
                                   D3D11_SDK_VERSION, &dev, &fl, &ctx);
    /* Pre-11.1 runtimes reject 11_1 in the list with E_INVALIDARG; retry without it. */
    if (hr == E_INVALIDARG)
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, levels + 1, ARRAYSIZE(levels) - 1,
                               D3D11_SDK_VERSION, &dev, &fl, &ctx);
    if (FAILED(hr)) {
        emit("NATIVE d3d11 status=FAIL hr=0x%lx dll=%s", (unsigned long)hr, module_kind("d3d11.dll"));
        return;
    }

    char adapter[256] = "?";
    unsigned vendor = 0, device = 0, software = 0;
    unsigned long long vram_mb = 0;
    IDXGIDevice *xdev = NULL;
    IDXGIAdapter *ad = NULL;
    if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **)&xdev)) &&
        SUCCEEDED(IDXGIDevice_GetAdapter(xdev, &ad))) {
        IDXGIAdapter1 *ad1 = NULL;
        DXGI_ADAPTER_DESC1 d1;
        DXGI_ADAPTER_DESC d;
        if (SUCCEEDED(IDXGIAdapter_QueryInterface(ad, &IID_IDXGIAdapter1, (void **)&ad1)) &&
            SUCCEEDED(IDXGIAdapter1_GetDesc1(ad1, &d1))) {
            WideCharToMultiByte(CP_UTF8, 0, d1.Description, -1, adapter, sizeof(adapter), NULL, NULL);
            vendor = d1.VendorId;
            device = d1.DeviceId;
            vram_mb = d1.DedicatedVideoMemory >> 20;
            software = (d1.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        } else if (SUCCEEDED(IDXGIAdapter_GetDesc(ad, &d))) {
            WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, adapter, sizeof(adapter), NULL, NULL);
            vendor = d.VendorId;
            device = d.DeviceId;
            vram_mb = d.DedicatedVideoMemory >> 20;
        }
        if (ad1) IDXGIAdapter1_Release(ad1);
    }
    if (ad) IDXGIAdapter_Release(ad);
    if (xdev) IDXGIDevice_Release(xdev);
    /* WARP / "Microsoft Basic Render Driver" is vendor 0x1414 device 0x8c. */
    int warp = software || (vendor == 0x1414 && device == 0x8c);

    emit("NATIVE d3d11 status=OK hr=0x%lx fl=0x%x adapter=\"%s\" vendor=0x%04x device=0x%04x vram_mb=%llu warp=%d dll=%s",
         (unsigned long)hr, (unsigned)fl, adapter, vendor, device, vram_mb, warp, module_kind("d3d11.dll"));
    ID3D11DeviceContext_Release(ctx);
    ID3D11Device_Release(dev);
}

static void probe_d3d12(void)
{
    ID3D12Device *dev = NULL;
    HRESULT hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev);
    emit("NATIVE d3d12 status=%s hr=0x%lx fl=0x%x dll=%s", status(hr), (unsigned long)hr,
         (unsigned)D3D_FEATURE_LEVEL_11_0, module_kind("d3d12.dll"));
    if (dev) ID3D12Device_Release(dev);
}

static void probe_xaudio2(void)
{
    HMODULE m = LoadLibraryW(L"xaudio2_9.dll");
    pfn_XAudio2Create fn = m ? (pfn_XAudio2Create)(void *)GetProcAddress(m, "XAudio2Create") : NULL;
    if (!fn) {
        emit("NATIVE xaudio2 status=FAIL reason=%s err=%lu", m ? "no-export" : "no-dll", GetLastError());
        return;
    }
    IXAudio2 *xa = NULL;
    HRESULT hr = fn(&xa, 0, XAUDIO2_DEFAULT_PROCESSOR);
    emit("NATIVE xaudio2 status=%s hr=0x%lx dll=%s", status(hr), (unsigned long)hr, module_kind("xaudio2_9.dll"));
    if (xa) IXAudio2_Release(xa);
}

int main(int argc, char **argv)
{
    const char *log_path = "probe_native.log";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--log") && i + 1 < argc)
            log_path = argv[++i];
        else
            fprintf(stderr, "probe_native: ignoring argument '%s' (usage: probe_native.exe [--log <path>])\n", argv[i]);
    }
    g_log = fopen(log_path, "w");
    if (!g_log)
        fprintf(stderr, "probe_native: cannot open log '%s', stdout only\n", log_path);

    CoInitializeEx(NULL, COINIT_MULTITHREADED); /* XAudio2 needs COM on the calling thread */
    probe_host();
    probe_memory();
    probe_gaming();
    probe_dxgi();
    probe_d3d11();
    probe_d3d12();
    probe_xaudio2();
    emit("PROBE_NATIVE done log=%s", g_log ? log_path : "(none)");
    CoUninitialize();

    if (g_log) fclose(g_log);
    return 0;
}
