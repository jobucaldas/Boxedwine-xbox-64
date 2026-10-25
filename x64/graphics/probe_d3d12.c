/* R4G O4: self-contained D3D12 device probe for on-device packaging (R4D).
 *
 * Enumerates DXGI adapters, creates a D3D12 device, clears a 64x64 RGBA8
 * render target, copies it to a readback buffer and checks every pixel. If a
 * Win32 window can be created it also clears/presents a flip-model swapchain
 * for a few frames; on UWP/Xbox that window path is expected to be SKIP.
 * Writes one PASS/FAIL line plus adapter details to a log and to stdout.
 *
 * d3d12.dll and dxgi.dll are loaded at runtime so a missing runtime is
 * reported as FAIL instead of a loader error. No GPU vendor extensions.
 *
 * Usage: probe_d3d12.exe [--log PATH] [--no-window] [--frames N]
 * PASS = device + exact offscreen clear, and present PASS or SKIP (not FAIL).
 * Exit: 0 PASS, 1 FAIL (device created, later step failed), 2 FAIL (no device). */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define RT_SIZE 64
#define RT_PITCH (RT_SIZE * 4) /* already D3D12_TEXTURE_DATA_PITCH_ALIGNMENT */

typedef HRESULT(WINAPI *PFN_CreateDXGIFactory1)(REFIID, void **);

static FILE *g_log;

static void logf_(const char *fmt, ...)
{
    va_list ap;
    char buf[1024];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fputs(buf, stdout);
    fflush(stdout);
    if (g_log) {
        fputs(buf, g_log);
        fflush(g_log);
    }
    OutputDebugStringA(buf);
}

/* Candidates in order: --log, PROBE_D3D12_LOG, exe dir, LOCALAPPDATA, TEMP.
 * Packaged UWP install dirs are read-only, so the later ones matter there. */
static void open_log(const wchar_t *explicit_path, wchar_t *used, size_t used_len)
{
    wchar_t cand[5][MAX_PATH];
    int n = 0;
    used[0] = 0;
    if (explicit_path)
        swprintf(cand[n++], MAX_PATH, L"%ls", explicit_path);
    if (GetEnvironmentVariableW(L"PROBE_D3D12_LOG", cand[n], MAX_PATH))
        n++;
    DWORD len = GetModuleFileNameW(NULL, cand[n], MAX_PATH);
    if (len && len < MAX_PATH) {
        wchar_t *slash = wcsrchr(cand[n], L'\\');
        if (slash) {
            slash[1] = 0;
            if (wcslen(cand[n]) + 16 < MAX_PATH) {
                wcscat(cand[n], L"probe_d3d12.log");
                n++;
            }
        }
    }
    wchar_t dir[MAX_PATH];
    DWORD dl = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);
    if (dl && dl + 17 < MAX_PATH)
        swprintf(cand[n++], MAX_PATH, L"%ls\\probe_d3d12.log", dir);
    dl = GetTempPathW(MAX_PATH, dir);
    if (dl && dl + 16 < MAX_PATH)
        swprintf(cand[n++], MAX_PATH, L"%lsprobe_d3d12.log", dir);
    for (int i = 0; i < n; i++) {
        g_log = _wfopen(cand[i], L"w");
        if (g_log) {
            swprintf(used, used_len, L"%ls", cand[i]);
            return;
        }
    }
}

static const char *fl_name(D3D_FEATURE_LEVEL fl)
{
    switch (fl) {
    case D3D_FEATURE_LEVEL_12_2: return "12_2";
    case D3D_FEATURE_LEVEL_12_1: return "12_1";
    case D3D_FEATURE_LEVEL_12_0: return "12_0";
    case D3D_FEATURE_LEVEL_11_1: return "11_1";
    case D3D_FEATURE_LEVEL_11_0: return "11_0";
    default: return "other";
    }
}

static void log_adapter(UINT index, const DXGI_ADAPTER_DESC1 *d)
{
    char name[256];
    WideCharToMultiByte(CP_UTF8, 0, d->Description, -1, name, sizeof(name), NULL, NULL);
    logf_("adapter[%u]: \"%s\" vendor=0x%04x device=0x%04x subsys=0x%08x rev=%u "
          "vram=%lluMiB shared=%lluMiB software=%d\n",
          index, name, d->VendorId, d->DeviceId, d->SubSysId, d->Revision,
          (unsigned long long)(d->DedicatedVideoMemory >> 20),
          (unsigned long long)(d->SharedSystemMemory >> 20),
          (d->Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? 1 : 0);
}

/* MinGW's C binding uses the out-pointer ABI for this aggregate return; call the
 * vtable directly instead of requiring WIDL_C_INLINE_WRAPPERS. */
static D3D12_CPU_DESCRIPTOR_HANDLE heap_start(ID3D12DescriptorHeap *heap)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(heap, &h);
    return h;
}

struct gpu {
    ID3D12Device *dev;
    ID3D12CommandQueue *queue;
    ID3D12CommandAllocator *alloc;
    ID3D12GraphicsCommandList *list;
    ID3D12Fence *fence;
    UINT64 fence_value;
    HANDLE event;
};

static HRESULT gpu_wait(struct gpu *g)
{
    HRESULT hr = ID3D12CommandQueue_Signal(g->queue, g->fence, ++g->fence_value);
    if (FAILED(hr))
        return hr;
    if (ID3D12Fence_GetCompletedValue(g->fence) < g->fence_value) {
        hr = ID3D12Fence_SetEventOnCompletion(g->fence, g->fence_value, g->event);
        if (FAILED(hr))
            return hr;
        if (WaitForSingleObject(g->event, 5000) != WAIT_OBJECT_0)
            return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
    }
    return S_OK;
}

static void execute(struct gpu *g)
{
    ID3D12CommandList *lists[] = {(ID3D12CommandList *)g->list};
    ID3D12CommandQueue_ExecuteCommandLists(g->queue, 1, lists);
}

static void barrier(ID3D12GraphicsCommandList *list, ID3D12Resource *res,
                    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b;
    memset(&b, 0, sizeof(b));
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
}

#define CHECK(expr, what)                                                   \
    do {                                                                    \
        hr = (expr);                                                        \
        if (FAILED(hr)) {                                                   \
            logf_("step %s failed hr=0x%08lx\n", what, (unsigned long)hr);  \
            goto done;                                                      \
        }                                                                   \
    } while (0)

/* Offscreen clear + readback; returns mismatching pixel count or -1. */
static long offscreen_clear(struct gpu *g)
{
    static const float color[4] = {1.0f, 0.0f, 1.0f, 1.0f};
    const UINT32 expect = 0xffff00ffu; /* R=ff G=00 B=ff A=ff, little-endian */
    ID3D12DescriptorHeap *heap = NULL;
    ID3D12Resource *tex = NULL, *rb = NULL;
    long bad = -1;
    HRESULT hr;

    D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1,
                                     D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    CHECK(ID3D12Device_CreateDescriptorHeap(g->dev, &hd, &IID_ID3D12DescriptorHeap,
                                            (void **)&heap), "CreateDescriptorHeap");

    D3D12_HEAP_PROPERTIES def = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                                 D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    D3D12_RESOURCE_DESC td;
    memset(&td, 0, sizeof(td));
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = RT_SIZE;
    td.Height = RT_SIZE;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv;
    cv.Format = td.Format;
    memcpy(cv.Color, color, sizeof(color));
    CHECK(ID3D12Device_CreateCommittedResource(g->dev, &def, D3D12_HEAP_FLAG_NONE, &td,
                                               D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
                                               &IID_ID3D12Resource, (void **)&tex),
          "CreateCommittedResource(rt)");

    D3D12_HEAP_PROPERTIES rbh = def;
    rbh.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd;
    memset(&bd, 0, sizeof(bd));
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = RT_PITCH * RT_SIZE;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    CHECK(ID3D12Device_CreateCommittedResource(g->dev, &rbh, D3D12_HEAP_FLAG_NONE, &bd,
                                               D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                                               &IID_ID3D12Resource, (void **)&rb),
          "CreateCommittedResource(readback)");

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = heap_start(heap);
    ID3D12Device_CreateRenderTargetView(g->dev, tex, NULL, rtv);

    CHECK(ID3D12CommandAllocator_Reset(g->alloc), "Allocator.Reset");
    CHECK(ID3D12GraphicsCommandList_Reset(g->list, g->alloc, NULL), "List.Reset");
    ID3D12GraphicsCommandList_ClearRenderTargetView(g->list, rtv, color, 0, NULL);
    barrier(g->list, tex, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst, src;
    memset(&dst, 0, sizeof(dst));
    memset(&src, 0, sizeof(src));
    dst.pResource = rb;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = td.Format;
    dst.PlacedFootprint.Footprint.Width = RT_SIZE;
    dst.PlacedFootprint.Footprint.Height = RT_SIZE;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = RT_PITCH;
    src.pResource = tex;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    ID3D12GraphicsCommandList_CopyTextureRegion(g->list, &dst, 0, 0, 0, &src, NULL);
    CHECK(ID3D12GraphicsCommandList_Close(g->list), "List.Close");
    execute(g);
    CHECK(gpu_wait(g), "fence wait (offscreen)");

    D3D12_RANGE rr = {0, RT_PITCH * RT_SIZE};
    void *mapped = NULL;
    CHECK(ID3D12Resource_Map(rb, 0, &rr, &mapped), "Readback.Map");
    bad = 0;
    UINT32 first_bad = 0;
    for (UINT y = 0; y < RT_SIZE; y++) {
        const UINT32 *row = (const UINT32 *)((const BYTE *)mapped + (size_t)y * RT_PITCH);
        for (UINT x = 0; x < RT_SIZE; x++)
            if (row[x] != expect && bad++ == 0)
                first_bad = row[x];
    }
    D3D12_RANGE none = {0, 0};
    ID3D12Resource_Unmap(rb, 0, &none);
    logf_("offscreen: %ux%u RGBA8 clear expect=0x%08x pixels=%u mismatches=%ld%s",
          RT_SIZE, RT_SIZE, expect, RT_SIZE * RT_SIZE, bad, bad ? "" : "\n");
    if (bad)
        logf_(" first_bad=0x%08x\n", first_bad);
done:
    if (rb) ID3D12Resource_Release(rb);
    if (tex) ID3D12Resource_Release(tex);
    if (heap) ID3D12DescriptorHeap_Release(heap);
    return bad;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    return DefWindowProcW(h, m, w, l);
}

/* Optional desktop HWND presentation: returns 1 PASS, 0 FAIL, -1 SKIP. */
static int window_present(struct gpu *g, IDXGIFactory2 *f2, int frames)
{
    enum { NBUF = 2 };
    HWND hwnd = NULL;
    IDXGISwapChain1 *sc1 = NULL;
    IDXGISwapChain3 *sc = NULL;
    ID3D12DescriptorHeap *heap = NULL;
    ID3D12Resource *bufs[NBUF] = {0};
    int result = 0;
    HRESULT hr;

    if (!f2) {
        logf_("present: SKIP (no IDXGIFactory2)\n");
        return -1;
    }
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"probe_d3d12";
    RegisterClassW(&wc);
    hwnd = CreateWindowExW(0, wc.lpszClassName, L"probe_d3d12", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT, 640, 360, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) {
        logf_("present: SKIP (CreateWindowEx failed err=%lu; UWP needs CoreWindow)\n",
              (unsigned long)GetLastError());
        return -1;
    }

    DXGI_SWAP_CHAIN_DESC1 sd;
    memset(&sd, 0, sizeof(sd));
    sd.Width = 640;
    sd.Height = 360;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = NBUF;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    CHECK(IDXGIFactory2_CreateSwapChainForHwnd(f2, (IUnknown *)g->queue, hwnd, &sd, NULL, NULL,
                                               &sc1), "CreateSwapChainForHwnd");
    CHECK(IDXGISwapChain1_QueryInterface(sc1, &IID_IDXGISwapChain3, (void **)&sc),
          "QI IDXGISwapChain3");
    D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, NBUF,
                                     D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    CHECK(ID3D12Device_CreateDescriptorHeap(g->dev, &hd, &IID_ID3D12DescriptorHeap,
                                            (void **)&heap), "CreateDescriptorHeap(sc)");
    UINT inc = ID3D12Device_GetDescriptorHandleIncrementSize(g->dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE base = heap_start(heap);
    for (UINT i = 0; i < NBUF; i++) {
        CHECK(IDXGISwapChain3_GetBuffer(sc, i, &IID_ID3D12Resource, (void **)&bufs[i]),
              "SwapChain.GetBuffer");
        D3D12_CPU_DESCRIPTOR_HANDLE h = {base.ptr + (SIZE_T)i * inc};
        ID3D12Device_CreateRenderTargetView(g->dev, bufs[i], NULL, h);
    }
    for (int f = 0; f < frames; f++) {
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
            DispatchMessageW(&msg);
        UINT i = IDXGISwapChain3_GetCurrentBackBufferIndex(sc);
        D3D12_CPU_DESCRIPTOR_HANDLE h = {base.ptr + (SIZE_T)i * inc};
        /* Cycle green -> blue so a recording visibly shows live frames. */
        float t = frames > 1 ? (float)f / (float)(frames - 1) : 0.0f;
        const float c[4] = {0.0f, 1.0f - t, t, 1.0f};
        CHECK(ID3D12CommandAllocator_Reset(g->alloc), "Allocator.Reset(sc)");
        CHECK(ID3D12GraphicsCommandList_Reset(g->list, g->alloc, NULL), "List.Reset(sc)");
        barrier(g->list, bufs[i], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        ID3D12GraphicsCommandList_ClearRenderTargetView(g->list, h, c, 0, NULL);
        barrier(g->list, bufs[i], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        CHECK(ID3D12GraphicsCommandList_Close(g->list), "List.Close(sc)");
        execute(g);
        CHECK(IDXGISwapChain3_Present(sc, 1, 0), "Present");
        CHECK(gpu_wait(g), "fence wait (present)");
    }
    logf_("present: PASS (%d frames, 640x360 flip-discard HWND swapchain)\n", frames);
    result = 1;
done:
    if (!result)
        logf_("present: FAIL\n");
    for (UINT i = 0; i < NBUF; i++)
        if (bufs[i]) ID3D12Resource_Release(bufs[i]);
    if (heap) ID3D12DescriptorHeap_Release(heap);
    if (sc) IDXGISwapChain3_Release(sc);
    if (sc1) IDXGISwapChain1_Release(sc1);
    DestroyWindow(hwnd);
    return result;
}

int wmain(int argc, wchar_t **argv)
{
    const wchar_t *log_arg = NULL;
    int want_window = 1, frames = 120;
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--log") && i + 1 < argc)
            log_arg = argv[++i];
        else if (!wcscmp(argv[i], L"--no-window"))
            want_window = 0;
        else if (!wcscmp(argv[i], L"--frames") && i + 1 < argc)
            frames = _wtoi(argv[++i]);
    }
    if (frames < 1)
        frames = 1;
    wchar_t log_path[MAX_PATH];
    open_log(log_arg, log_path, MAX_PATH);

    logf_("probe_d3d12 R4G-O4 build " __DATE__ " " __TIME__ "\n");
    logf_("log: %ls\n", log_path[0] ? log_path : L"(stdout only)");

    struct gpu g;
    memset(&g, 0, sizeof(g));
    IDXGIFactory1 *factory = NULL;
    IDXGIFactory2 *f2 = NULL;
    IDXGIAdapter1 *chosen = NULL;
    int rc = 2, present = -1;
    long bad = -1;
    char chosen_name[256] = "(none)";
    D3D_FEATURE_LEVEL max_fl = 0;
    HRESULT hr = S_OK;

    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    PFN_CreateDXGIFactory1 create_factory =
        dxgi ? (PFN_CreateDXGIFactory1)(void *)GetProcAddress(dxgi, "CreateDXGIFactory1") : NULL;
    PFN_D3D12_CREATE_DEVICE create_device =
        d3d12 ? (PFN_D3D12_CREATE_DEVICE)(void *)GetProcAddress(d3d12, "D3D12CreateDevice") : NULL;
    logf_("runtime: dxgi.dll=%s d3d12.dll=%s\n", create_factory ? "ok" : "MISSING",
          create_device ? "ok" : "MISSING");
    if (!create_device)
        goto done;

    if (create_factory && SUCCEEDED(create_factory(&IID_IDXGIFactory1, (void **)&factory))) {
        IDXGIFactory1_QueryInterface(factory, &IID_IDXGIFactory2, (void **)&f2);
        IDXGIAdapter1 *a;
        for (UINT i = 0; IDXGIFactory1_EnumAdapters1(factory, i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_ADAPTER_DESC1 d;
            if (SUCCEEDED(IDXGIAdapter1_GetDesc1(a, &d))) {
                log_adapter(i, &d);
                if (!chosen && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                    SUCCEEDED(create_device((IUnknown *)a, D3D_FEATURE_LEVEL_11_0,
                                            &IID_ID3D12Device, (void **)&g.dev))) {
                    chosen = a;
                    WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, chosen_name,
                                        sizeof(chosen_name), NULL, NULL);
                    logf_("device: created on adapter[%u]\n", i);
                    continue;
                }
            }
            IDXGIAdapter1_Release(a);
        }
    } else {
        logf_("dxgi: CreateDXGIFactory1 unavailable; trying default adapter\n");
    }
    if (!g.dev) {
        hr = create_device(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&g.dev);
        if (FAILED(hr)) {
            logf_("device: D3D12CreateDevice(default, 11_0) failed hr=0x%08lx\n", (unsigned long)hr);
            goto done;
        }
        logf_("device: created on default adapter\n");
    }
    rc = 1;

    static const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_2};
    D3D12_FEATURE_DATA_FEATURE_LEVELS fls = {sizeof(levels) / sizeof(levels[0]), levels, 0};
    if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(g.dev, D3D12_FEATURE_FEATURE_LEVELS, &fls,
                                                   sizeof(fls)))) {
        max_fl = fls.MaxSupportedFeatureLevel;
        logf_("device: max feature level %s (0x%x)\n", fl_name(max_fl), (unsigned)max_fl);
    }

    D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
    CHECK(ID3D12Device_CreateCommandQueue(g.dev, &qd, &IID_ID3D12CommandQueue, (void **)&g.queue),
          "CreateCommandQueue");
    CHECK(ID3D12Device_CreateCommandAllocator(g.dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              &IID_ID3D12CommandAllocator, (void **)&g.alloc),
          "CreateCommandAllocator");
    CHECK(ID3D12Device_CreateCommandList(g.dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc, NULL,
                                         &IID_ID3D12GraphicsCommandList, (void **)&g.list),
          "CreateCommandList");
    CHECK(ID3D12GraphicsCommandList_Close(g.list), "List.Close(initial)");
    CHECK(ID3D12Device_CreateFence(g.dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence,
                                   (void **)&g.fence), "CreateFence");
    g.event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!g.event) {
        logf_("step CreateEvent failed err=%lu\n", (unsigned long)GetLastError());
        goto done;
    }

    bad = offscreen_clear(&g);
    if (bad == 0)
        rc = 0;
    if (want_window) {
        present = window_present(&g, f2, frames);
        if (present == 0)
            rc = 1;
    } else
        logf_("present: SKIP (--no-window)\n");

done:
    logf_("RESULT: %s adapter=\"%s\" feature_level=%s offscreen=%s present=%s\n",
          rc == 0 ? "PASS" : "FAIL", chosen_name, max_fl ? fl_name(max_fl) : "n/a",
          bad == 0 ? "PASS" : "FAIL", present > 0 ? "PASS" : present == 0 ? "FAIL" : "SKIP");
    if (g.event) CloseHandle(g.event);
    if (g.fence) ID3D12Fence_Release(g.fence);
    if (g.list) ID3D12GraphicsCommandList_Release(g.list);
    if (g.alloc) ID3D12CommandAllocator_Release(g.alloc);
    if (g.queue) ID3D12CommandQueue_Release(g.queue);
    if (g.dev) ID3D12Device_Release(g.dev);
    if (chosen) IDXGIAdapter1_Release(chosen);
    if (f2) IDXGIFactory2_Release(f2);
    if (factory) IDXGIFactory1_Release(factory);
    if (g_log) fclose(g_log);
    return rc;
}
