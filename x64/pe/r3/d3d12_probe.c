/* d3d12_probe.c - tiny D3D12 smoke test for the R3 graphics path (VKD3D-Proton vs Wine's vkd3d).
 * Creates a device, queue, flip-model swapchain, clears the backbuffer to a known colour,
 * presents a few frames and reports which d3d12.dll / adapter was used.
 * Build: x86_64-w64-mingw32-gcc -O2 -o d3d12_probe.exe d3d12_probe.c -ld3d12 -ldxgi -luser32 -luuid */
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#define INITGUID
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <stdio.h>
#include <string.h>

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("%s failed hr=0x%08lx\n", #x, hr_); return 1; } } while (0)

int main(void)
{
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "r3d3d12";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("r3d3d12", "R3 D3D12 probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                              340, 0, 320, 240, NULL, NULL, wc.hInstance, NULL);

    IDXGIFactory4 *fac = NULL;
    CHECK(CreateDXGIFactory1(&IID_IDXGIFactory4, (void **)&fac));
    IDXGIAdapter1 *ad = NULL;
    DXGI_ADAPTER_DESC1 adesc;
    CHECK(IDXGIFactory4_EnumAdapters1(fac, 0, &ad));
    IDXGIAdapter1_GetDesc1(ad, &adesc);
    printf("adapter: %ls (vendor 0x%04x)\n", adesc.Description, adesc.VendorId);

    ID3D12Device *dev = NULL;
    CHECK(D3D12CreateDevice((IUnknown *)ad, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev));
    char path[MAX_PATH] = "?";
    const char *base = (const char *)GetModuleHandleA("d3d12.dll");
    GetModuleFileNameA((HMODULE)base, path, sizeof(path));
    printf("d3d12.dll: %s\n", path);
    printf("d3d12.dll kind: %s\n", memcmp(base + 0x40, "Wine builtin DLL", 16) ? "native (e.g. VKD3D-Proton)" : "Wine builtin (vkd3d)");

    D3D12_COMMAND_QUEUE_DESC qd = {0};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue *q = NULL;
    CHECK(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&q));

    DXGI_SWAP_CHAIN_DESC1 sd = {0};
    sd.Width = 320;
    sd.Height = 240;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1 *sc1 = NULL;
    IDXGISwapChain3 *sc = NULL;
    CHECK(IDXGIFactory4_CreateSwapChainForHwnd(fac, (IUnknown *)q, hwnd, &sd, NULL, NULL, &sc1));
    CHECK(IDXGISwapChain1_QueryInterface(sc1, &IID_IDXGISwapChain3, (void **)&sc));

    D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 2;
    ID3D12DescriptorHeap *heap = NULL;
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&heap));
    UINT inc = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE h0;
    /* mingw C binding with WIDL_C_INLINE_WRAPPERS returns the handle by value */
    h0 = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(heap);
    ID3D12Resource *bufs[2];
    for (UINT i = 0; i < 2; i++) {
        CHECK(IDXGISwapChain3_GetBuffer(sc, i, &IID_ID3D12Resource, (void **)&bufs[i]));
        D3D12_CPU_DESCRIPTOR_HANDLE h = { h0.ptr + i * inc };
        ID3D12Device_CreateRenderTargetView(dev, bufs[i], NULL, h);
    }

    ID3D12CommandAllocator *alloc = NULL;
    ID3D12GraphicsCommandList *cl = NULL;
    ID3D12Fence *fence = NULL;
    CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc));
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList, (void **)&cl));
    ID3D12GraphicsCommandList_Close(cl);
    CHECK(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence));
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    UINT64 fv = 0;

    const float colour[4] = { 1.0f, 0.5f, 0.0f, 1.0f };
    int frames = 0;
    for (; frames < 30; frames++) {
        UINT idx = IDXGISwapChain3_GetCurrentBackBufferIndex(sc);
        ID3D12CommandAllocator_Reset(alloc);
        ID3D12GraphicsCommandList_Reset(cl, alloc, NULL);
        D3D12_RESOURCE_BARRIER b = {0};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = bufs[idx];
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &b);
        D3D12_CPU_DESCRIPTOR_HANDLE h = { h0.ptr + idx * inc };
        ID3D12GraphicsCommandList_ClearRenderTargetView(cl, h, colour, 0, NULL);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &b);
        ID3D12GraphicsCommandList_Close(cl);
        ID3D12CommandQueue_ExecuteCommandLists(q, 1, (ID3D12CommandList **)&cl);
        if (FAILED(IDXGISwapChain3_Present(sc, 0, 0))) break;
        ID3D12CommandQueue_Signal(q, fence, ++fv);
        if (ID3D12Fence_GetCompletedValue(fence) < fv) {
            ID3D12Fence_SetEventOnCompletion(fence, fv, ev);
            WaitForSingleObject(ev, 5000);
        }
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
    }
    printf("presented %d frames, fence=%llu\n", frames, (unsigned long long)ID3D12Fence_GetCompletedValue(fence));
    Sleep(1500); /* leave the frame on screen for an external screenshot */
    printf("d3d12 probe done\n");
    return 0;
}
