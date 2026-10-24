# Enumeration-only DirectComposition stub (R4G O2)

Build: `make -C x64/graphics`

Unit test: `x64/graphics/tests/run.sh` (MinGW x64, Linux Wine and an already
initialized prefix fixture required). The test copies the fixture to ignored
`out/prefix`; set UWPH_FIXTURE_PREFIX to choose it. DISPLAY defaults to :99.
The runner builds the existing uwp-host harness, starts a separate stock
wineserver for that owned prefix, then uses run_via_wine and Wine's own PE
loader with the selected Unix-side uwph preload shim. It explicitly opts into
native dcomp loading for this process tree only. No system DLL is replaced.

The DLL exports DCompositionCreateDevice and implements the complete
IDCompositionDevice vtable using the installed MinGW declarations. NULL DXGI
device + IDCompositionDevice/IUnknown returns an allocated, reference-counted
COM object. Other interfaces are rejected; all compositor methods return
E_NOTIMPL, clearing output pointers but preserving input arrays. No target,
visual, surface, swapchain, GPU work or commit is implemented.

Mesa 25.1.9 wsi_common_win32.cpp:954 requests precisely this object, retains it
at :990, and releases it at :1027. This supplies that enumeration dependency;
it does not implement the later CreateTargetForHwnd/CreateVisual/Commit path.
Place the built DLL beside an owned dzn probe and explicitly select native
`dcomp` to experiment later. O2 does not run or claim dzn enumeration success.

Evidence in evidence/R4G-O2: exact build/test logs and PID-specific shim
reports. Build exit 0; unit exit 0, S_OK. This is THROUGH launcher + selected
uwph routing on desktop Linux Wine, with a separate stock wineserver; not full
uwph acceptance, UWP/AppContainer deployment, CoreWindow presentation, or Xbox
GPU proof. dcomp.h exposes desktop interfaces: this compatibility object makes
no platform compositor calls and does not authorize using desktop DComp APIs
in UWP. Production UWP integration remains unimplemented. Lavapipe is not Xbox
D3D12. No video: the unit test has textual output and no visual presentation.
