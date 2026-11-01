// Enumeration-only COM stub. No rendering or platform compositor calls.
#include <windows.h>
#include <dcomp.h>
#include <new>
#include <atomic>
static const GUID device_iid = {0xc37ea93a,0xe7aa,0x450d,{0xb1,0x6f,0x97,0x46,0xcb,0x04,0x07,0xf3}};
class Device final : public IDCompositionDevice {
    std::atomic<ULONG> refs{1};
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != device_iid) return E_NOINTERFACE;
        *out = static_cast<IDCompositionDevice *>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG left = --refs; if (!left) delete this; return left;
    }
    HRESULT STDMETHODCALLTYPE Commit() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE WaitForCommitCompletion() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DCOMPOSITION_FRAME_STATISTICS* a0) override { if (a0) *a0 = {}; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateTargetForHwnd(HWND a0, BOOL a1, IDCompositionTarget** a2) override { if (a2) *a2 = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateVisual(IDCompositionVisual** a0) override { if (a0) *a0 = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateSurface(UINT a0, UINT a1, DXGI_FORMAT a2, DXGI_ALPHA_MODE a3, IDCompositionSurface** a4) override { if (a4) *a4 = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateVirtualSurface(UINT a0, UINT a1, DXGI_FORMAT a2, DXGI_ALPHA_MODE a3, IDCompositionVirtualSurface** a4) override { if (a4) *a4 = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateSurfaceFromHandle(HANDLE a0, IUnknown** a1) override { if (a1) *a1 = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateSurfaceFromHwnd(HWND a0, IUnknown** a1) override { if (a1) *a1 = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateTranslateTransform(IDCompositionTranslateTransform** a0) override { if (a0) *a0 = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateScaleTransform(IDCompositionScaleTransform** a0) override { if (a0) *a0 = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateRotateTransform(IDCompositionRotateTransform** a0) override { if (a0) *a0 = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateSkewTransform(IDCompositionSkewTransform** a0) override { if (a0) *a0 = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateMatrixTransform(IDCompositionMatrixTransform** a0) override { if (a0) *a0 = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateTransformGroup(IDCompositionTransform** a0, UINT a1, IDCompositionTransform** a2) override { if (a2) *a2 = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateTranslateTransform3D(IDCompositionTranslateTransform3D** a0) override { if (a0) *a0 = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateScaleTransform3D(IDCompositionScaleTransform3D** a0) override { if (a0) *a0 = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateRotateTransform3D(IDCompositionRotateTransform3D** a0) override { if (a0) *a0 = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateMatrixTransform3D(IDCompositionMatrixTransform3D** a0) override { if (a0) *a0 = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateTransform3DGroup(IDCompositionTransform3D** a0, UINT a1, IDCompositionTransform3D** a2) override { if (a2) *a2 = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateEffectGroup(IDCompositionEffectGroup** a0) override { if (a0) *a0 = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateRectangleClip(IDCompositionRectangleClip** a0) override { if (a0) *a0 = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateAnimation(IDCompositionAnimation** a0) override { if (a0) *a0 = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CheckDeviceState(BOOL* a0) override { if (a0) *a0 = FALSE; return E_NOTIMPL; }
};
extern "C" __declspec(dllexport) HRESULT WINAPI
DCompositionCreateDevice(IDXGIDevice *dxgi, REFIID iid, void **out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (dxgi) return E_NOTIMPL; // Only Mesa's NULL-device enumeration contract.
    if (iid != device_iid && iid != IID_IUnknown) return E_NOINTERFACE;
    auto *device = new (std::nothrow) Device;
    if (!device) return E_OUTOFMEMORY;
    *out = static_cast<IDCompositionDevice *>(device);
    return S_OK;
}
