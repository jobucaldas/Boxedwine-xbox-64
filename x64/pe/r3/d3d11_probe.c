/* d3d11_probe.c - tiny D3D11 smoke test for the R3 graphics path.
 * Creates a window + swapchain, clears to a known colour, presents a few frames,
 * reads the backbuffer back through a staging texture and reports which d3d11.dll
 * was loaded (Wine's wined3d builtin vs DXVK) and which adapter it ran on.
 * Build: x86_64-w64-mingw32-gcc -O2 -o d3d11_probe.exe d3d11_probe.c -ld3d11 -ldxgi -luser32 -lgdi32 */
#define COBJMACROS
#define INITGUID
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

int main(void)
{
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "r3d3d11";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("r3d3d11", "R3 D3D11 probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                              0, 0, 320, 240, NULL, NULL, wc.hInstance, NULL);

    DXGI_SWAP_CHAIN_DESC sd = {0};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 320;
    sd.BufferDesc.Height = 240;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    IDXGISwapChain *sc = NULL;
    D3D_FEATURE_LEVEL fl = 0;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
                                               D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx);
    if (FAILED(hr)) { printf("D3D11CreateDeviceAndSwapChain failed hr=0x%08lx\n", hr); return 1; }

    char path[MAX_PATH] = "?";
    GetModuleFileNameA(GetModuleHandleA("d3d11.dll"), path, sizeof(path));
    printf("d3d11.dll: %s\n", path);
    /* Wine builtin PE dlls carry "Wine builtin DLL" right after the DOS header */
    const char *base = (const char *)GetModuleHandleA("d3d11.dll");
    printf("d3d11.dll kind: %s\n", memcmp(base + 0x40, "Wine builtin DLL", 16) ? "native (e.g. DXVK)" : "Wine builtin (wined3d)");
    printf("feature level: 0x%x\n", fl);

    IDXGIDevice *xdev = NULL;
    IDXGIAdapter *ad = NULL;
    DXGI_ADAPTER_DESC adesc;
    if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **)&xdev)) &&
        SUCCEEDED(IDXGIDevice_GetAdapter(xdev, &ad)) && SUCCEEDED(IDXGIAdapter_GetDesc(ad, &adesc)))
        printf("adapter: %ls (vendor 0x%04x)\n", adesc.Description, adesc.VendorId);

    ID3D11Texture2D *bb = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&bb);
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)bb, NULL, &rtv);

    const float colour[4] = { 0.0f, 0.5f, 1.0f, 1.0f };
    for (int i = 0; i < 30; i++) {
        ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, colour);
        IDXGISwapChain_Present(sc, 0, 0);
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
    }

    /* FLIP_DISCARD: re-render into buffer 0 and copy it out before presenting again */
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, colour);
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D_GetDesc(bb, &td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    ID3D11Texture2D *st = NULL;
    ID3D11Device_CreateTexture2D(dev, &td, NULL, &st);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st, (ID3D11Resource *)bb);
    D3D11_MAPPED_SUBRESOURCE map;
    hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &map);
    if (SUCCEEDED(hr)) {
        unsigned char *p = map.pData;
        printf("readback pixel(0,0) RGBA = %u,%u,%u,%u (expect 0,128,255,255)\n", p[0], p[1], p[2], p[3]);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
    } else {
        printf("Map failed hr=0x%08lx\n", hr);
    }
    IDXGISwapChain_Present(sc, 0, 0);
    Sleep(1500); /* leave the frame on screen for an external screenshot */
    printf("d3d11 probe done\n");
    return 0;
}
