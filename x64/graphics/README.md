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

O3 offscreen clear: `make -C x64/graphics`, then
`DISPLAY=:99 x64/graphics/tests/run-clear.sh`. Requires the initialized O2
prefix and existing local dzn runtime at `/workspace/xbox-readiness/dzn/runtime`.
The runner copies dependencies into ignored out/o3, opts into O2's native dcomp
object, and directly loads dzn's ICD export. The 64x64 RGBA8 clear is copied to
host memory and every pixel checked. Exact logs, binary hashes and shim reports
are in evidence/R4G-O3. Both normal and loader-diagnostic runs exit 0, 4096 pixels
and zero mismatches. This clears no window and creates no swapchain. The Linux
backend is lavapipe, regardless of Wine's reported NVIDIA adapter identity;
lavapipe ≠ Xbox D3D12. See NOTES-x64.md for proof limits and artifact paths.

## O4 on-device D3D12 probe (for R4D packaging)

Build: `make -C x64/graphics out/probe_d3d12.exe` (MinGW only, no Xbox needed).
Output `x64/graphics/out/probe_d3d12.exe`: PE32+ x86-64 console exe, imports
only KERNEL32/USER32/msvcrt; d3d12.dll and dxgi.dll are loaded at runtime, so a
missing runtime is logged as FAIL rather than a loader error. No extra DLLs.

What it does: enumerates DXGI adapters (name, vendor/device id, memory,
software flag), creates a D3D12 device at FL 11_0 on the first hardware
adapter (fallback: default adapter), logs the max feature level, clears a
64x64 RGBA8 render target to magenta, copies it to a readback buffer and
checks all 4096 pixels. Then, unless `--no-window`, it tries a Win32 window
plus flip-discard HWND swapchain and presents `--frames N` (default 120)
green-to-blue clears. Under UWP there is normally no HWND: that step logs
`present: SKIP` and does not fail the probe. CoreWindow presentation is not
implemented here.

R4D snippet:

```
Copy x64/graphics/out/probe_d3d12.exe into the package payload.
Args:  probe_d3d12.exe [--log <path>] [--no-window] [--frames N]
Log:   first writable of --log, %PROBE_D3D12_LOG%, <exe dir>\probe_d3d12.log,
       %LOCALAPPDATA%\probe_d3d12.log, %TEMP%\probe_d3d12.log. The package
       install dir is read-only, so pass --log <LocalState>\probe_d3d12.log or
       read it from the app's TEMP/LocalAppData. Also mirrored to stdout and
       OutputDebugString.
Read:  last line `RESULT: PASS|FAIL adapter="..." feature_level=.. offscreen=..
       present=PASS|FAIL|SKIP`.
Exit:  0 PASS; 1 FAIL after device creation; 2 FAIL no device/runtime.
Needs: D3D12 FL 11_0 available to the packaged app (assumed for a Dev Mode
       game-type app; unverified on device). No extra capabilities.
```

Linux smoke: `DISPLAY=:99 x64/graphics/tests/run-probe.sh [--frames N]` runs it
via run_via_wine + selected uwph routing + Wine's own PE loader, with Wine's
builtin d3d12/dxgi (vkd3d) on lavapipe. Evidence in evidence/R4G-O4. This is
not Xbox evidence: Wine reports a fake NVIDIA adapter; lavapipe ≠ Xbox D3D12.
