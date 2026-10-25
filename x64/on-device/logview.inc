/* B61: live on-screen log for the console. Every out_line() goes to LocalState
 * (ladder-native.log) AND to this CoreWindow view. CPU text into the swapchain
 * back buffer via UpdateSubresource: no XAML, no shaders, D3D11 FL 11_0 is enough.
 * The view shares g_swap with rung n3: it detaches before n3 (so n3 still creates
 * its own swapchain, unchanged) and re-attaches to n3's swapchain afterwards. */
#include "logfont.h"

#define CON_LINES 600
#define CON_WRAP 160
static char con_ring[CON_LINES][CON_WRAP + 1];
static int con_count;
static ID3D11Device *con_dev;
static ID3D11DeviceContext *con_ctx;
static ID3D11Texture2D *con_tex;
static UINT con_w, con_h;
static uint32_t *con_px;
static int con_ready, con_dirty, con_busy;
static double con_last;

static void con_push(const char *line)
{
    size_t n = strlen(line), off = 0;
    do {
        size_t take = n - off > CON_WRAP ? CON_WRAP : n - off;
        char *dst = con_ring[con_count % CON_LINES];
        memcpy(dst, line + off, take);
        dst[take] = 0;
        con_count++;
        off += take;
    } while (off < n);
}

static uint32_t con_colour(const char *s)
{
    if (strstr(s, " FAIL") || strstr(s, "FATAL") || strstr(s, "REFUSED")) return 0xFF5050;
    if (strstr(s, " PASS") || strstr(s, " OK")) return 0x60E070;
    if (strstr(s, " SKIP")) return 0xE0C040;
    if (s[0] == '#') return 0xA8B0C0;
    return 0xF0F0F0;
}

static void con_text(int x, int y, const char *s, uint32_t fg)
{
    for (; *s && x + LOGFONT_W <= (int)con_w; s++, x += LOGFONT_W) {
        unsigned char c = (unsigned char)*s;
        if (c < 32 || c > 126) c = '?';
        if (c == ' ') continue;
        const unsigned char *g = logfont[c - 32];
        for (int r = 0; r < LOGFONT_H && y + r < (int)con_h; r++) {
            uint32_t *row = con_px + (size_t)(y + r) * con_w + x;
            for (int k = 0; k < LOGFONT_W; k++) {
                unsigned a = g[r * LOGFONT_W + k];
                if (!a) continue;
                uint32_t bg = row[k], o = 0;
                for (int sh = 0; sh < 24; sh += 8) {
                    unsigned b = (bg >> sh) & 255, f = (fg >> sh) & 255;
                    o |= ((b * (255 - a) + f * a) / 255) << sh;
                }
                row[k] = 0xFF000000u | o;
            }
        }
    }
}

static void con_draw(int force)
{
    if (!con_ready || con_busy) { con_dirty = 1; return; }
    double t = now_ms();
    if (!force && t - con_last < 40) { con_dirty = 1; return; }
    con_busy = 1;
    for (size_t i = 0, n = (size_t)con_w * con_h; i < n; i++) con_px[i] = 0xFF101820;
    int x0 = (int)(con_w * 0.05), y0 = (int)(con_h * 0.05);   /* TV title-safe margin */
    int rows = ((int)(con_h * 0.90) - 2 * LOGFONT_H) / LOGFONT_H;
    for (int y = y0 + LOGFONT_H + 4; y < y0 + LOGFONT_H + 6; y++)
        for (int x = x0; x < (int)con_w - x0; x++) con_px[(size_t)y * con_w + x] = 0xFF0078D7;
    char head[200];
    snprintf(head, sizeof head, "Boxedwine Dev Mode - live log  (%d lines, also LocalState\\ladder-native.log)  ctx=%s", con_count, g_ctx);
    con_text(x0, y0, head, 0x40A8FF);
    int first = con_count > rows ? con_count - rows : 0;
    if (con_count - first > CON_LINES) first = con_count - CON_LINES;
    for (int i = first, y = y0 + 2 * LOGFONT_H; i < con_count; i++, y += LOGFONT_H) {
        const char *s = con_ring[i % CON_LINES];
        con_text(x0, y, s, con_colour(s));
    }
    ID3D11DeviceContext_UpdateSubresource(con_ctx, (ID3D11Resource *)con_tex, 0, NULL, con_px, con_w * 4, 0);
    IDXGISwapChain1_Present(g_swap, 1, 0);
    con_last = now_ms();
    con_dirty = 0;
    pump();
    con_busy = 0;
}

static void con_log(const char *line)
{
    con_push(line);
    con_draw(1);   /* every line is shown as it happens (~1 vsync each) */
}

static void con_detach(void)
{
    if (!con_ready) return;
    con_ready = 0;
    if (con_tex) ID3D11Texture2D_Release(con_tex);
    if (con_ctx) { ID3D11DeviceContext_ClearState(con_ctx); ID3D11DeviceContext_Flush(con_ctx); ID3D11DeviceContext_Release(con_ctx); }
    if (con_dev) ID3D11Device_Release(con_dev);
    con_tex = NULL; con_ctx = NULL; con_dev = NULL;
    if (g_swap) { IDXGISwapChain1_Release(g_swap); g_swap = NULL; }
}

static HRESULT con_attach(IUnknown *window)
{
    HRESULT hr = S_OK;
    if (!window || con_ready) return S_OK;
    if (!g_swap) {   /* own swapchain until n3, same parameters as n3 */
        ID3D11Device *dev = NULL; ID3D11DeviceContext *ctx = NULL; D3D_FEATURE_LEVEL fl = 0;
        IDXGIDevice *dxgi = NULL; IDXGIAdapter *ad = NULL; IDXGIFactory2 *fac = NULL;
        DXGI_SWAP_CHAIN_DESC1 d = {0};
        hr = make_d3d11(D3D_DRIVER_TYPE_HARDWARE, &dev, &ctx, &fl);
        if (SUCCEEDED(hr)) hr = ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **)&dxgi);
        if (SUCCEEDED(hr)) hr = IDXGIDevice_GetAdapter(dxgi, &ad);
        if (SUCCEEDED(hr)) hr = IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory2, (void **)&fac);
        if (SUCCEEDED(hr)) hr = swapchain_size(window, &d.Width, &d.Height);
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d.BufferCount = 2;
        d.Scaling = DXGI_SCALING_STRETCH; d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        if (SUCCEEDED(hr)) hr = IDXGIFactory2_CreateSwapChainForCoreWindow(fac, (IUnknown *)dev, window, &d, NULL, &g_swap);
        if (fac) IDXGIFactory2_Release(fac);
        if (ad) IDXGIAdapter_Release(ad);
        if (dxgi) IDXGIDevice_Release(dxgi);
        if (ctx) ID3D11DeviceContext_Release(ctx);
        if (dev) ID3D11Device_Release(dev);
        if (FAILED(hr)) { g_swap = NULL; return hr; }
    }
    DXGI_SWAP_CHAIN_DESC1 d;
    IDXGISwapChain1_GetDesc1(g_swap, &d);
    hr = IDXGISwapChain1_GetDevice(g_swap, &IID_ID3D11Device, (void **)&con_dev);
    if (SUCCEEDED(hr)) { ID3D11Device_GetImmediateContext(con_dev, &con_ctx); hr = IDXGISwapChain1_GetBuffer(g_swap, 0, &IID_ID3D11Texture2D, (void **)&con_tex); }
    if (SUCCEEDED(hr) && (d.Width != con_w || d.Height != con_h || !con_px)) {
        free(con_px);
        con_w = d.Width; con_h = d.Height;
        con_px = malloc((size_t)con_w * con_h * 4);
        if (!con_px) hr = E_OUTOFMEMORY;
    }
    if (FAILED(hr)) { con_ready = 1; con_detach(); return hr; }
    con_ready = 1;
    con_draw(1);
    return S_OK;
}
