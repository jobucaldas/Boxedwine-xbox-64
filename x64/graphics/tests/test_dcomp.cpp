#include <windows.h>
#include <dcomp.h>
#include <cstdio>
#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
int main() {
    const GUID iid = {0xc37ea93a,0xe7aa,0x450d,{0xb1,0x6f,0x97,0x46,0xcb,0x04,0x07,0xf3}};
    const GUID unknown = {0xdeadbeef,0,0,{0}};
    HMODULE mod = LoadLibraryA("dcomp.dll");
    CHECK(mod);
    using Create = HRESULT (WINAPI *)(IDXGIDevice *, REFIID, void **);
    auto create = reinterpret_cast<Create>(reinterpret_cast<void *>(GetProcAddress(mod,"DCompositionCreateDevice")));
    CHECK(create);
    CHECK(create(nullptr, iid, nullptr) == E_POINTER);
    void *out = reinterpret_cast<void *>(1);
    CHECK(create(nullptr, unknown, &out) == E_NOINTERFACE && !out);
    CHECK(create(reinterpret_cast<IDXGIDevice *>(1), iid, &out) == E_NOTIMPL && !out);
    HRESULT hr = create(nullptr, iid, &out);
    CHECK(hr == S_OK && out);
    auto *dev = static_cast<IDCompositionDevice *>(out);
    IUnknown *identity = nullptr;
    CHECK(dev->QueryInterface(IID_IUnknown, reinterpret_cast<void **>(&identity)) == S_OK);
    CHECK(identity == static_cast<IUnknown *>(dev));
    CHECK(identity->Release() == 1);
    CHECK(dev->QueryInterface(unknown, &out) == E_NOINTERFACE && !out);
    IDCompositionVisual *visual = reinterpret_cast<IDCompositionVisual *>(1);
    CHECK(dev->CreateVisual(&visual) == E_NOTIMPL && !visual);
    CHECK(dev->Commit() == E_NOTIMPL);
    CHECK(dev->Release() == 0);
    CHECK(FreeLibrary(mod));
    std::puts("PASS DCompositionCreateDevice HRESULT=0x00000000; COM identity/lifetime/errors; compositor E_NOTIMPL");
    return 0;
}
